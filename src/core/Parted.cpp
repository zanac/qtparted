/*
 * QtParted - a Qt frontend to libparted for manipulating disk partitions
 * Copyright (C) 2002-2026 Vanni Brutto <zanac4ever@gmail.com>
 *
 * Device scanning and the partition table changes are ported from GParted_Core
 * (https://gparted.org), Copyright (C) 2004 Bart Hakvoort and the GParted contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "Parted.h"

#include "Luks.h"

#include "Command.h"
#include "FileSystem.h"
#include "OperationDetail.h"
#include "Probe.h"
#include "SystemInfo.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QThread>
#include <QtEndian>

#include <parted/parted.h>

#include <fcntl.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <memory>
#include <vector>

namespace QtParted {
namespace Parted {

namespace {

QMutex g_prompterMutex;
Prompter g_prompter;


PedExceptionOption exceptionHandler(PedException *e)
{
    const QString message = QString::fromLocal8Bit(e->message);
    OperationDetail::pushLibpartedMessage(message);

    int count = 0;
    int single = 0;
    for (int opt = PED_EXCEPTION_OPTION_FIRST; opt <= PED_EXCEPTION_OPTION_LAST; opt <<= 1)
        if (e->options & opt) {
            ++count;
            single = opt;
        }
    // A single choice needs no question (e.g. "unrecognised disk label" with Cancel).
    if (count == 1 && e->type != PED_EXCEPTION_BUG && e->type != PED_EXCEPTION_FATAL)
        return PedExceptionOption(single);

    Prompter p;
    {
        QMutexLocker lock(&g_prompterMutex);
        p = g_prompter;
    }
    if (p) {
        const int answer = p(int(e->type), message, int(e->options));
        if (answer & e->options)
            return PedExceptionOption(answer);
    }
    return PED_EXCEPTION_UNHANDLED;
}

// RAII owners for libparted objects.
struct DeviceRef {
    PedDevice *dev = nullptr;
    explicit DeviceRef(const QString &path) { dev = ped_device_get(path.toLocal8Bit().constData()); }
    ~DeviceRef() { if (dev) ped_device_destroy(dev); }
    DeviceRef(const DeviceRef &) = delete;
    DeviceRef &operator=(const DeviceRef &) = delete;
    explicit operator bool() const { return dev != nullptr; }
};

struct DiskRef {
    PedDisk *disk = nullptr;
    explicit DiskRef(PedDevice *dev) { if (dev) disk = ped_disk_new(dev); }
    DiskRef(PedDevice *dev, const PedDiskType *type) { if (dev && type) disk = ped_disk_new_fresh(dev, type); }
    ~DiskRef() { if (disk) ped_disk_destroy(disk); }
    DiskRef(const DiskRef &) = delete;
    DiskRef &operator=(const DiskRef &) = delete;
    explicit operator bool() const { return disk != nullptr; }
};

QString partitionPath(const PedPartition *part)
{
    char *p = ped_partition_get_path(part);
    if (!p)
        return QString();
    QString s = QString::fromLocal8Bit(p);
    std::free(p);
    return s;
}

std::vector<PedPartitionFlag> allFlags()
{
    std::vector<PedPartitionFlag> flags;
    for (PedPartitionFlag f = ped_partition_flag_next(PedPartitionFlag(0)); f; f = ped_partition_flag_next(f))
        flags.push_back(f);
    return flags;
}

bool usableDevice(const PedDevice *dev)
{
    const int fd = ::open(dev->path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return false;
    std::vector<char> buf(size_t(dev->sector_size));
    const ssize_t n = ::read(fd, buf.data(), buf.size());
    ::close(fd);
    return n == ssize_t(buf.size());
}

int maxNameLength(const QString &diskType)
{
    if (diskType == QLatin1String("gpt"))   return 36;
    if (diskType == QLatin1String("mac"))   return 32;
    if (diskType == QLatin1String("amiga")) return 32;
    if (diskType == QLatin1String("dvh"))   return 8;
    if (diskType == QLatin1String("pc98"))  return 16;
    return 32;
}

FsType fsTypeFromLibparted(const char *name)
{
    if (!name)
        return FsType::Unknown;
    const QString n = QString::fromLatin1(name);
    if (n.startsWith(QLatin1String("linux-swap")))
        return FsType::LinuxSwap;
    if (n == QLatin1String("hfs+") || n == QLatin1String("hfsx"))
        return FsType::HfsPlus;
    if (n == QLatin1String("fat16"))
        return FsType::Fat16;
    if (n == QLatin1String("fat32"))
        return FsType::Fat32;
    return fsTypeFromBlkid(n, QString());
}

bool commitToOs(PedDisk *disk)
{
    const bool ok = ped_disk_commit_to_os(disk);
    settle();
    return ok;
}

bool commit(PedDisk *disk)
{
    // Keep the device open across both commits so the kernel and udev see one change.
    const bool opened = ped_device_open(disk->dev);
    bool ok = ped_disk_commit_to_dev(disk);
    ok = commitToOs(disk) && ok;
    if (opened) {
        ped_device_close(disk->dev);
        settle();
    }
    return ok;
}

// The libparted partition matching `p`.  Never return free space or metadata entries,
// and refuse a partition whose type or boundaries differ from what QtParted expects:
// acting on it would change something the user did not choose.
PedPartition *findPartition(const PedDisk *disk, const Partition &p)
{
    PedPartition *lp = p.type == PartitionType::Extended ? ped_disk_extended_partition(disk)
                                                          : ped_disk_get_partition_by_sector(disk, p.middleSector());
    if (!lp)
        return nullptr;
    PedPartitionType expected = PED_PARTITION_NORMAL;
    if (p.type == PartitionType::Logical)
        expected = PED_PARTITION_LOGICAL;
    else if (p.type == PartitionType::Extended)
        expected = PED_PARTITION_EXTENDED;
    if (lp->type != expected || !ped_partition_is_active(lp)) {
        OperationDetail::pushLibpartedMessage(
            QCoreApplication::translate("Parted", "No %1 found at sector %2 of the partition table").arg(p.path).arg(p.middleSector()));
        return nullptr;
    }
    if (lp->geom.start != p.start || lp->geom.end != p.end) {
        OperationDetail::pushLibpartedMessage(
            QCoreApplication::translate("Parted", "The partition table changed: %1 was expected at sectors %2-%3 but sectors %4-%5 were found")
                .arg(p.path).arg(p.start).arg(p.end).arg(lp->geom.start).arg(lp->geom.end));
        return nullptr;
    }
    return lp;
}

QString geometryText(const Partition &p, bool device)
{
    return QCoreApplication::translate("Parted", "path: %1 (%2)").arg(p.path, device ? QCoreApplication::translate("Parted", "device") : QCoreApplication::translate("Parted", "partition")) + QLatin1Char('\n')
         + QCoreApplication::translate("Parted", "start: %1").arg(p.start) + QLatin1Char('\n')
         + QCoreApplication::translate("Parted", "end: %1").arg(p.end) + QLatin1Char('\n')
         + QCoreApplication::translate("Parted", "size: %1 (%2)").arg(p.length()).arg(formatSize(p.length(), p.sectorSize));
}

void waitForPath(const QString &path)
{
    if (!path.startsWith(QLatin1String("/dev/")))
        return;
    QElapsedTimer t;
    t.start();
    while (!QFileInfo::exists(path) && t.elapsed() < 10000)
        QThread::msleep(100);
}

void fillPartitionDetails(Partition &p, const Device &device, const SystemInfo &sys)
{
    const bool nodeExists = !device.isImageFile && QFileInfo::exists(p.path);
    if (p.type != PartitionType::Extended) {
        ProbeResult pr;
        if (nodeExists) {
            pr = Probe::path(p.path);
            if (!pr.ok) {
                // The node may be being recreated by udev: wait for it, else read the disk.
                waitForPath(p.path);
                pr = Probe::path(p.path);
                if (!pr.ok)
                    pr = Probe::range(device.path, p.start * p.sectorSize, p.byteLength());
            }
        } else
            pr = Probe::range(device.path, p.start * p.sectorSize, p.byteLength());
        if (pr.fsType != FsType::Unknown)
            p.fsType = pr.fsType;
        p.fsLabel = pr.label;
        p.uuid = pr.uuid;
        if (pr.ambivalent)
            p.messages.append(QCoreApplication::translate("Parted", "Several file system signatures were found; the content can not be identified safely."));
    }

    if (nodeExists && p.fsType == FsType::Luks) {
        // Port of GParted's set_luks_partition() and luks::set_used_sectors().
        const LuksMapping m = Luks::mapping(p.path);
        if (!m.isOpen()) {
            // Closed; still busy if something else holds the partition.
            p.busy = SystemInfo::hasHolders(p.path) || SystemInfo::isExclusivelyBusy(p.path);
            p.setUsage(p.length(), 0);  // the encrypted data fills the partition
            return;
        }
        p.busy = true;
        p.luks = m;
        p.luksHeader = (m.offset + p.sectorSize - 1) / p.sectorSize;  // never smaller than the header
        p.setUsage((m.offset + m.length) / p.sectorSize, 0);
        Partition in;
        in.devicePath = m.path();
        in.path = m.path();
        in.number = -1;
        in.type = PartitionType::Unpartitioned;
        in.fsType = FsType::Unknown;
        in.sectorSize = p.sectorSize;
        in.start = 0;
        in.end = m.length / p.sectorSize - 1;
        const ByteValue innerSector = SystemInfo::logicalSectorSize(in.path);
        if (innerSector > 0 && innerSector != p.sectorSize) {
            // LUKS2 with larger encryption sectors: the mapping counts in other units.
            // Only closing it is offered (no encrypted partition is attached).
            p.messages.append(QCoreApplication::translate("Parted", "The encryption mapping %1 uses %2 byte sectors, the partition %3 byte sectors: "
                                 "the file system inside it can not be changed by QtParted.")
                                  .arg(in.path).arg(innerSector).arg(p.sectorSize));
            return;
        }
        if (QFileInfo::exists(in.path)) {
            fillPartitionDetails(in, device, sys);
        } else {
            in.messages.append(QCoreApplication::translate("Parted", "The encryption mapping %1 has no device node.").arg(in.path));
        }
        p.encrypted.emplace(in);
        return;
    }

    if (nodeExists) {
        p.busy = sys.isMounted(p.path) || sys.isSwapActive(p.path)
              || (p.type != PartitionType::Extended
                  && (SystemInfo::hasHolders(p.path) || SystemInfo::isExclusivelyBusy(p.path)));
        if (sys.isMounted(p.path)) {
            p.mountPoints = sys.mountPoints(p.path);
            p.fsReadonly = sys.isMountedReadonly(p.path);
        } else if (p.fsType == FsType::Lvm2Pv && Tools::has(QStringLiteral("lvm"))) {
            const CommandResult r = runCommand(QStringLiteral("lvm"),
                                               { QStringLiteral("pvs"), QStringLiteral("--noheadings"),
                                                 QStringLiteral("-o"), QStringLiteral("vg_name"), p.path });
            const QString vg = r.output.trimmed();
            if (r.ok() && !vg.isEmpty())
                p.mountPoints = { vg };
        } else if (!p.busy && p.type != PartitionType::Extended && p.fsType != FsType::LinuxSwap) {
            p.mountPoints = sys.fstabMountPoints(p.path, p.uuid, p.fsLabel);
        }
    }

    if (p.fsType == FsType::Unknown && p.type != PartitionType::Extended) {
        p.messages.append(QCoreApplication::translate("Parted", "Unable to detect file system! Possible reasons are:") + QLatin1String("\n- ")
                          + QCoreApplication::translate("Parted", "The file system is damaged") + QLatin1String("\n- ")
                          + QCoreApplication::translate("Parted", "The file system is unknown to QtParted") + QLatin1String("\n- ")
                          + QCoreApplication::translate("Parted", "There is no file system available (unformatted)") + QLatin1String("\n- ")
                          + QCoreApplication::translate("Parted", "The device entry %1 is missing").arg(p.path));
    }

    if (nodeExists && fsIsReal(p.fsType)) {
        FileSystemRegistry::instance().readUsage(p);
        const FileSystem *fs = FileSystemRegistry::instance().get(p.fsType);
        if (!p.usageKnown() && fs) {
            QString m = QCoreApplication::translate("Parted", "Unable to read the contents of this file system!") + QLatin1Char('\n')
                      + QCoreApplication::translate("Parted", "Because of this some operations may be unavailable.");
            if (!fs->software().isEmpty())
                m += QLatin1Char('\n')
                   + QCoreApplication::translate("Parted", "The following software is required for %1 file system support: %2.")
                         .arg(fsName(p.fsType), fs->software());
            p.messages.append(m);
        } else if (p.usageKnown() && p.sectorsUnallocated > 0
                   && p.sectorsUnallocated * p.sectorSize >= 2 * MiB) {
            p.messages.append(QCoreApplication::translate("Parted", "%1 of unallocated space within the partition.")
                                  .arg(formatSize(p.sectorsUnallocated, p.sectorSize)));
        }
    }
}

void readPartitions(Device &device, PedDisk *disk, const SystemInfo &sys)
{
    int extIndex = -1;
    const auto flags = allFlags();

    for (PedPartition *lp = ped_disk_next_partition(disk, nullptr); lp; lp = ped_disk_next_partition(disk, lp)) {
        if (lp->type != PED_PARTITION_NORMAL && lp->type != PED_PARTITION_LOGICAL
            && lp->type != PED_PARTITION_EXTENDED)
            continue;

        OperationDetail::takeLibpartedMessages();
        Partition p;
        p.devicePath = device.path;
        p.path = partitionPath(lp);
        p.number = lp->num;
        p.start = lp->geom.start;
        p.end = lp->geom.end;
        p.sectorSize = device.sectorSize;
        if (lp->type == PED_PARTITION_EXTENDED) {
            p.type = PartitionType::Extended;
            p.fsType = FsType::Extended;
        } else {
            p.type = lp->type == PED_PARTITION_LOGICAL ? PartitionType::Logical : PartitionType::Primary;
            p.insideExtended = lp->type == PED_PARTITION_LOGICAL;
            p.fsType = FsType::Unknown;
        }

        for (PedPartitionFlag f : flags) {
            if (!ped_partition_is_flag_available(lp, f))
                continue;
            const QString name = QString::fromLatin1(ped_partition_flag_get_name(f));
            p.availableFlags.append(name);
            if (ped_partition_get_flag(lp, f))
                p.flags.append(name);
        }
        if (device.partitionNamingSupported())
            p.name = QString::fromUtf8(ped_partition_get_name(lp));

        fillPartitionDetails(p, device, sys);
        if (p.fsType == FsType::Unknown && lp->fs_type) {
            const FsType t = fsTypeFromLibparted(lp->fs_type->name);
            if (t != FsType::Unknown) {
                p.fsType = t;
                const QString unknown = QCoreApplication::translate("Parted", "Unable to detect file system! Possible reasons are:");
                p.messages.erase(std::remove_if(p.messages.begin(), p.messages.end(),
                                                [&](const QString &m) { return m.startsWith(unknown); }),
                                 p.messages.end());
            }
        }
        if (p.type == PartitionType::Logical && p.isFlagSet(QStringLiteral("esp")))
            p.messages.append(QCoreApplication::translate("Parted", "UEFI firmware does not support booting from a logical EFI System Partition (ESP)"));
        p.messages += OperationDetail::takeLibpartedMessages();

        if (p.busy && p.number > device.highestBusy)
            device.highestBusy = p.number;

        if (p.type == PartitionType::Extended) {
            extIndex = int(device.partitions.size());
            device.partitions.push_back(p);
        } else if (p.insideExtended && extIndex >= 0) {
            device.partitions[size_t(extIndex)].logicals.push_back(p);
        } else {
            device.partitions.push_back(p);
        }
    }

    if (extIndex >= 0) {
        Partition &ext = device.partitions[size_t(extIndex)];
        insertUnallocated(device.path, ext.logicals, ext.start, ext.end, device.sectorSize, true);
        for (const Partition &l : ext.logicals)
            if (l.busy)
                ext.busy = true;
    }
    insertUnallocated(device.path, device.partitions, 0, device.length - 1, device.sectorSize, false);
}

} // namespace

// ---------------------------------------------------------------------------------

void initialize()
{
    ped_exception_set_handler(exceptionHandler);
}

void setPrompter(Prompter prompter)
{
    QMutexLocker lock(&g_prompterMutex);
    g_prompter = std::move(prompter);
}

QString version()
{
    return QString::fromLatin1(ped_get_version());
}

QList<QPair<int, QString>> exceptionOptions(int options)
{
    QList<QPair<int, QString>> list;
    for (int opt = PED_EXCEPTION_OPTION_FIRST; opt <= PED_EXCEPTION_OPTION_LAST; opt <<= 1)
        if (options & opt)
            list.append({ opt, QString::fromLatin1(ped_exception_get_option_string(PedExceptionOption(opt))) });
    return list;
}

QString exceptionTypeName(int type)
{
    switch (type) {
    case PED_EXCEPTION_INFORMATION: return QCoreApplication::translate("Parted", "Libparted Information");
    case PED_EXCEPTION_WARNING:     return QCoreApplication::translate("Parted", "Libparted Warning");
    case PED_EXCEPTION_ERROR:       return QCoreApplication::translate("Parted", "Libparted Error");
    case PED_EXCEPTION_FATAL:       return QCoreApplication::translate("Parted", "Libparted Fatal");
    case PED_EXCEPTION_BUG:         return QCoreApplication::translate("Parted", "Libparted Bug");
    case PED_EXCEPTION_NO_FEATURE:  return QCoreApplication::translate("Parted", "Libparted Unsupported Feature");
    default:                        return QCoreApplication::translate("Parted", "Libparted unknown exception");
    }
}

QStringList diskLabelTypes()
{
    QStringList types;
    for (PedDiskType *t = ped_disk_type_get_next(nullptr); t; t = ped_disk_type_get_next(t))
        types.append(QString::fromLatin1(t->name));
    return types;
}

DiskTypeInfo diskTypeInfo(const QString &devicePath, const QString &diskType)
{
    DiskTypeInfo info;
    const PedDiskType *type = ped_disk_type_get(diskType.toLatin1().constData());
    if (!type)
        return info;
    info.supportsExtended = ped_disk_type_check_feature(type, PED_DISK_TYPE_EXTENDED);
    if (ped_disk_type_check_feature(type, PED_DISK_TYPE_PARTITION_NAME))
        info.maxNameLength = maxNameLength(diskType);
    // An in-memory fresh table (never committed) tells the primary partition limit.
    DeviceRef dev(devicePath);
    if (dev) {
        DiskRef disk(dev.dev, type);
        if (disk)
            info.maxPrimaries = ped_disk_get_max_primary_partition_count(disk.disk);
    }
    OperationDetail::takeLibpartedMessages();
    return info;
}

QStringList probeDevicePaths()
{
    QStringList candidates = SystemInfo::wholeDiskPaths();
    if (candidates.isEmpty()) {
        ped_device_probe_all();
        for (PedDevice *d = ped_device_get_next(nullptr); d; d = ped_device_get_next(d))
            candidates.append(QString::fromLocal8Bit(d->path));
    }
    QStringList usable;
    for (const QString &c : candidates) {
        DeviceRef dev(c);
        if (dev && usableDevice(dev.dev))
            usable.append(c);
    }
    OperationDetail::takeLibpartedMessages();
    std::sort(usable.begin(), usable.end());
    usable.removeDuplicates();
    return usable;
}

std::optional<Device> readDevice(const QString &path, const SystemInfo &sys)
{
    OperationDetail::takeLibpartedMessages();
    DeviceRef dev(path);
    if (!dev || !usableDevice(dev.dev))
        return std::nullopt;

    Device device;
    device.path = path;
    device.model = QString::fromLocal8Bit(dev.dev->model).trimmed();
    device.length = dev.dev->length;
    device.sectorSize = dev.dev->sector_size;
    device.heads = dev.dev->bios_geom.heads;
    device.sectorsPerTrack = dev.dev->bios_geom.sectors;
    device.cylinders = dev.dev->bios_geom.cylinders;
    device.cylinderSize = Sector(device.heads) * device.sectorsPerTrack;
    if (device.cylinderSize < MiB / device.sectorSize)
        device.cylinderSize = MiB / device.sectorSize;
    device.isImageFile = dev.dev->type == PED_DEVICE_FILE || SystemInfo::isRegularFile(path);
    if (!device.isImageFile)
        device.serial = SystemInfo::serialNumber(path);

    // In use as a whole: mounted, swap, held by device-mapper/md, or claimed exclusively.
    auto wholeDiskBusy = [&]() {
        return !device.isImageFile
            && (sys.isMounted(path) || sys.isSwapActive(path) || SystemInfo::hasHolders(path)
                || SystemInfo::isExclusivelyBusy(path));
    };

    // 1/4: file system on the whole disk device (no partition table)
    const ProbeResult whole = Probe::path(path, true);
    if (whole.ptType.isEmpty() && whole.fsType != FsType::Unknown) {
        device.diskType = QStringLiteral("none");
        device.maxPrimaries = 1;
        Partition p = Partition::makeUnpartitioned(path, path, whole.fsType, device.length, device.sectorSize,
                                                   false);
        p.fsLabel = whole.label;
        p.uuid = whole.uuid;
        if (!device.isImageFile) {
            p.busy = wholeDiskBusy();
            if (sys.isMounted(path))
                p.mountPoints = sys.mountPoints(path);
            FileSystemRegistry::instance().readUsage(p);
        }
        if (p.busy)
            device.highestBusy = 1;
        device.partitions.push_back(p);
        OperationDetail::takeLibpartedMessages();
        return device;
    }

    // 2/4: no partition table recognised
    const PedDiskType *probed = ped_disk_probe(dev.dev);
    if (!probed) {
        device.diskType = QCoreApplication::translate("Parted", "unrecognized");
        device.maxPrimaries = 1;
        Partition p = Partition::makeUnpartitioned(path, QString(), FsType::Unallocated, device.length,
                                                   device.sectorSize, wholeDiskBusy());
        p.messages += OperationDetail::takeLibpartedMessages();
        if (p.busy) {
            device.highestBusy = 1;
            p.messages.append(QCoreApplication::translate("Parted", "The device is in use."));
        }
        device.partitions.push_back(p);
        return device;
    }

    DiskRef disk(dev.dev);
    // libparted opened the disk read-write to read the table; closing it makes udev
    // re-read the partitions, which can drop and recreate their device nodes.  Let that
    // finish before the partitions are probed through those nodes.
    if (!device.isImageFile)
        settle();
    if (!disk) {
        device.diskType = QCoreApplication::translate("Parted", "unrecognized");
        device.maxPrimaries = 1;
        Partition p = Partition::makeUnpartitioned(path, QString(), FsType::Unallocated, device.length,
                                                   device.sectorSize, wholeDiskBusy());
        p.messages += OperationDetail::takeLibpartedMessages();
        if (p.busy) {
            device.highestBusy = 1;
            p.messages.append(QCoreApplication::translate("Parted", "The device is in use."));
        }
        device.partitions.push_back(p);
        return device;
    }

    // 3/4: libparted "loop" label only, i.e. unknown whole disk content
    if (qstrcmp(disk.disk->type->name, "loop") == 0) {
        device.diskType = QStringLiteral("loop");
        device.maxPrimaries = 1;
        Partition p = Partition::makeUnpartitioned(path, path, FsType::Unknown, device.length, device.sectorSize,
                                                   wholeDiskBusy());
        if (p.busy)
            device.highestBusy = 1;
        p.messages.append(QCoreApplication::translate("Parted", "Unable to detect file system! Possible reasons are:") + QLatin1String("\n- ")
                          + QCoreApplication::translate("Parted", "The file system is damaged") + QLatin1String("\n- ")
                          + QCoreApplication::translate("Parted", "The file system is unknown to QtParted") + QLatin1String("\n- ")
                          + QCoreApplication::translate("Parted", "There is no file system available (unformatted)"));
        device.partitions.push_back(p);
        OperationDetail::takeLibpartedMessages();
        return device;
    }

    // 4/4: partitioned
    device.diskType = QString::fromLatin1(disk.disk->type->name);
    device.hasPartitionTable = true;
    device.maxPrimaries = ped_disk_get_max_primary_partition_count(disk.disk);
    device.supportsExtended = ped_disk_type_check_feature(disk.disk->type, PED_DISK_TYPE_EXTENDED);
    if (ped_disk_type_check_feature(disk.disk->type, PED_DISK_TYPE_PARTITION_NAME))
        device.maxNameLength = maxNameLength(device.diskType);
    device.messages += OperationDetail::takeLibpartedMessages();

    readPartitions(device, disk.disk, sys);

    // The whole disk is used by device-mapper (multipath, dmraid) or md: hands off.
    if (!device.isImageFile && SystemInfo::hasHolders(path)) {
        std::function<void(PartitionVector &)> markBusy = [&](PartitionVector &v) {
            for (Partition &p : v) {
                p.busy = true;
                markBusy(p.logicals);
            }
        };
        markBusy(device.partitions);
        device.readonly = true;
        device.highestBusy = std::max(device.highestBusy, 1);
        device.messages.append(QCoreApplication::translate("Parted", "%1 is used as a whole by another device (device-mapper, RAID).").arg(path));
        return device;
    }

    if (device.highestBusy > 0 && !device.isImageFile) {
        // Can the kernel be told about changes while partitions are in use?
        device.readonly = !commitToOs(disk.disk);
        OperationDetail::takeLibpartedMessages();
    }
    return device;
}

// ---------------------------------------------------------------------------------

void settle()
{
    if (Tools::has(QStringLiteral("udevadm")))
        runCommand(QStringLiteral("udevadm"), { QStringLiteral("settle"), QStringLiteral("--timeout=10") },
                   nullptr, QByteArray(), 15000);
    else
        QThread::msleep(300);
}

bool createDiskLabel(const Device &device, const QString &diskType, OperationDetail &detail)
{
    // Wipe any whole disk file system so neither libparted nor blkid prefer it to the new table.
    Partition whole = Partition::makeUnpartitioned(device.path, device.path, FsType::Unknown, device.length,
                                                   device.sectorSize, false);
    OperationDetail &od = detail.addChild(QCoreApplication::translate("Parted", "create new %1 partition table on %2").arg(diskType, device.path));
    if (!eraseSignatures(whole, od)) {
        od.setSuccessAndCaptureErrors(false);
        return false;
    }
    bool ok = false;
    DeviceRef dev(device.path);
    const PedDiskType *type = ped_disk_type_get(diskType.toLatin1().constData());
    if (dev && type) {
        DiskRef disk(dev.dev, type);
        ok = disk && commit(disk.disk);
    }
    od.setSuccessAndCaptureErrors(ok);
    return ok;
}

bool calibrate(Partition &p, OperationDetail &detail)
{
    if (!p.isPartitionInTable() && p.type != PartitionType::Unpartitioned)
        return true;

    OperationDetail &od = detail.addChild(QCoreApplication::translate("Parted", "calibrate %1").arg(p.path));
    bool ok = false;
    DeviceRef dev(p.devicePath);
    if (dev) {
        if (p.type == PartitionType::Unpartitioned) {
            ok = true;
        } else {
            DiskRef disk(dev.dev);
            if (disk) {
                if (PedPartition *lp = findPartition(disk.disk, p)) {
                    // Real path of a partition created earlier in this batch, and the
                    // boundaries libparted actually used.
                    if (!QFileInfo::exists(p.path))
                        p.path = partitionPath(lp);
                    p.number = lp->num;
                    p.start = lp->geom.start;
                    p.end = lp->geom.end;
                    ok = true;
                }
            }
        }
    }
    if (ok)
        od.addChild(geometryText(p, p.type == PartitionType::Unpartitioned), OperationDetail::Status::None,
                    OperationDetail::Font::Italic);
    od.setSuccessAndCaptureErrors(ok);
    return ok;
}

static QString sysfsValue(const QString &devPath, const QString &attribute)
{
    const quint64 d = SystemInfo::devNumber(devPath);
    if (!d)
        return QString();
    QFile f(QStringLiteral("/sys/dev/block/%1:%2/%3").arg(major(d)).arg(minor(d)).arg(attribute));
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromLatin1(f.readAll()).trimmed();
}

bool verifyTarget(const Partition &p, bool checkKernelGeometry, OperationDetail &detail, bool expectInUse)
{
    OperationDetail &od = detail.addChild(expectInUse ? QCoreApplication::translate("Parted", "check the position of %1").arg(p.path)
                                                      : QCoreApplication::translate("Parted", "check that %1 is not in use").arg(p.path));
    // Open LUKS encryption is in use even when nothing else holds the partition.
    bool inUse = SystemInfo::isInUse(p.path);
    if (!inUse && p.fsType == FsType::Luks) {
        Luks::loadCache();
        inUse = Luks::mapping(p.path).isOpen();
    }
    if (expectInUse && p.type != PartitionType::Extended && !inUse) {
        // Queued as an online change: the file system tools would act on whatever is
        // mounted at the old mount point now.
        od.addChild(QCoreApplication::translate("Parted", "%1 is no longer in use, but the operation was queued for a mounted file system. "
                       "Refresh the devices and queue it again. Nothing was changed on it.").arg(p.path),
                    OperationDetail::Status::None, OperationDetail::Font::Italic);
        od.setStatus(OperationDetail::Status::Error);
        return false;
    }
    if (!expectInUse && inUse) {
        od.addChild(QCoreApplication::translate("Parted", "%1 is in use (mounted, active swap, or used by another device). "
                       "Nothing was changed on it.").arg(p.path),
                    OperationDetail::Status::None, OperationDetail::Font::Italic);
        od.setStatus(OperationDetail::Status::Error);
        return false;
    }
    if (checkKernelGeometry && p.type != PartitionType::Unpartitioned && QFileInfo::exists(p.path)) {
        // sysfs reports start and size in 512 byte units.
        bool okStart = false, okSize = false;
        const qint64 start = sysfsValue(p.path, QStringLiteral("start")).toLongLong(&okStart);
        const qint64 size = sysfsValue(p.path, QStringLiteral("size")).toLongLong(&okSize);
        const qint64 factor = p.sectorSize / 512;
        // The kernel registers an msdos extended partition with a tiny size (it holds only
        // the EBR chain), so only its start can be compared.
        const bool sizeDiffers = p.type != PartitionType::Extended && size != p.length() * factor;
        if (okStart && okSize && (start != p.start * factor || sizeDiffers)) {
            od.addChild(QCoreApplication::translate("Parted", "The kernel sees %1 at a different position than the partition table "
                           "(kernel: start %2, size %3; table: start %4, size %5, in 512 byte units). "
                           "Refresh the devices or reboot before changing it.")
                            .arg(p.path).arg(start).arg(size).arg(p.start * factor).arg(p.length() * factor),
                        OperationDetail::Status::None, OperationDetail::Font::Italic);
            od.setStatus(OperationDetail::Status::Error);
            return false;
        }
    }
    od.setStatus(OperationDetail::Status::Success);
    return true;
}

bool verifyFollowingLogicalsUnused(const Partition &p, OperationDetail &detail)
{
    if (p.type != PartitionType::Logical)
        return true;
    OperationDetail &od = detail.addChild(QCoreApplication::translate("Parted", "check that the logical partitions after %1 are not in use").arg(p.path));
    for (const QString &part : SystemInfo::kernelPartitions(p.devicePath)) {
        const int number = sysfsValue(part, QStringLiteral("partition")).toInt();
        if (number > p.number && SystemInfo::isInUse(part)) {
            od.addChild(QCoreApplication::translate("Parted", "%1 is in use and would be renumbered. Unmount it first.").arg(part),
                        OperationDetail::Status::None, OperationDetail::Font::Italic);
            od.setStatus(OperationDetail::Status::Error);
            return false;
        }
    }
    od.setStatus(OperationDetail::Status::Success);
    return true;
}

bool verifyDiskUnused(const Device &device, OperationDetail &detail)
{
    OperationDetail &od = detail.addChild(QCoreApplication::translate("Parted", "check that no partition of %1 is in use").arg(device.path));
    QStringList busy;
    if (!device.isImageFile && SystemInfo::hasHolders(device.path))
        busy.append(device.path);
    for (const QString &part : SystemInfo::kernelPartitions(device.path))
        if (SystemInfo::isInUse(part))
            busy.append(part);
    if (!busy.isEmpty()) {
        od.addChild(QCoreApplication::translate("Parted", "In use: %1. Nothing was changed.").arg(busy.join(QStringLiteral(", "))),
                    OperationDetail::Status::None, OperationDetail::Font::Italic);
        od.setStatus(OperationDetail::Status::Error);
        return false;
    }
    od.setStatus(OperationDetail::Status::Success);
    return true;
}

bool createPartition(Partition &p, Sector minSize, OperationDetail &detail)
{
    OperationDetail &od = detail.addChild(QCoreApplication::translate("Parted", "create empty partition"));
    bool ok = false;
    DeviceRef dev(p.devicePath);
    if (dev) {
        DiskRef disk(dev.dev);
        if (disk) {
            PedPartitionType type = PED_PARTITION_NORMAL;
            if (p.type == PartitionType::Logical)
                type = PED_PARTITION_LOGICAL;
            else if (p.type == PartitionType::Extended)
                type = PED_PARTITION_EXTENDED;
            const PedFileSystemType *fsType =
                p.type == PartitionType::Extended ? nullptr : ped_file_system_type_get("ext2");

            PedPartition *lp = ped_partition_new(disk.disk, type, fsType, p.start, p.end);
            if (lp) {
                PedConstraint *constraint = nullptr;
                if (p.alignment == Alignment::MiB || p.alignment == Alignment::None) {
                    if (PedGeometry *geom = ped_geometry_new(dev.dev, p.start, p.length())) {
                        constraint = ped_constraint_exact(geom);
                        ped_geometry_destroy(geom);
                    }
                } else {
                    constraint = ped_constraint_any(dev.dev);
                }
                if (constraint) {
                    if (minSize > 0 && p.fsType != FsType::Xfs)
                        constraint->min_size = minSize;
                    if (!ped_disk_add_partition(disk.disk, lp, constraint)) {
                        ped_partition_destroy(lp);  // not owned by the disk
                    } else if (commit(disk.disk)) {
                        p.path = partitionPath(lp);
                        p.number = lp->num;
                        p.start = lp->geom.start;
                        p.end = lp->geom.end;
                        ok = true;
                    }
                    // After a successful add the disk owns lp, even when the commit failed.
                    ped_constraint_destroy(constraint);
                } else {
                    ped_partition_destroy(lp);
                }
            }
        }
    }
    if (ok) {
        waitForPath(p.path);
        od.addChild(geometryText(p, false), OperationDetail::Status::None, OperationDetail::Font::Italic);
    }
    od.setSuccessAndCaptureErrors(ok);
    return ok;
}

bool deletePartition(const Partition &p, OperationDetail &detail)
{
    OperationDetail &od = detail.addChild(QCoreApplication::translate("Parted", "delete partition"));
    bool ok = false;
    DeviceRef dev(p.devicePath);
    if (dev) {
        DiskRef disk(dev.dev);
        if (disk) {
            if (PedPartition *lp = findPartition(disk.disk, p))
                ok = ped_disk_delete_partition(disk.disk, lp) && commit(disk.disk);
        }
    }
    od.setSuccessAndCaptureErrors(ok);
    return ok;
}

static bool setFlagStep(PedPartition *lp, PedPartitionFlag flag, bool state, OperationDetail &od)
{
    OperationDetail &c = od.addChild(state ? QCoreApplication::translate("Parted", "new partition flag: %1").arg(QString::fromLatin1(ped_partition_flag_get_name(flag)))
                                           : QCoreApplication::translate("Parted", "clear partition flag: %1").arg(QString::fromLatin1(ped_partition_flag_get_name(flag))));
    const bool ok = ped_partition_set_flag(lp, flag, state ? 1 : 0);
    c.setSuccessAndCaptureErrors(ok);
    return ok;
}

bool setPartitionType(const Partition &p, OperationDetail &detail)
{
    if (p.type == PartitionType::Unpartitioned)
        return true;  // nothing to do on a whole disk device

    OperationDetail &od = detail.addChild(QCoreApplication::translate("Parted", "set partition type on %1").arg(p.path));
    DeviceRef dev(p.devicePath);
    if (!dev) {
        od.setSuccessAndCaptureErrors(false);
        return false;
    }
    DiskRef disk(dev.dev);
    PedPartition *lp = disk ? findPartition(disk.disk, p) : nullptr;
    if (!lp) {
        od.setSuccessAndCaptureErrors(false);
        return false;
    }

    bool ok = false;
    const bool isFat = p.fsType == FsType::Fat16 || p.fsType == FsType::Fat32;
    if (p.type == PartitionType::Extended && ped_partition_is_flag_available(lp, PED_PARTITION_LBA)) {
        ok = setFlagStep(lp, PED_PARTITION_LBA, true, od);
    } else if (isFat && p.isFlagSet(QStringLiteral("esp")) && ped_partition_is_flag_available(lp, PED_PARTITION_ESP)) {
        // UEFI only supports FAT in EFI System Partitions.
        ok = setFlagStep(lp, PED_PARTITION_ESP, true, od);
    } else if (p.fsType == FsType::Lvm2Pv && ped_partition_is_flag_available(lp, PED_PARTITION_LVM)) {
        ok = setFlagStep(lp, PED_PARTITION_LVM, true, od);
    } else {
        const PedFileSystemType *fsType = nullptr;
        if (fsIsReal(p.fsType))
            fsType = ped_file_system_type_get(fsName(p.fsType).toLatin1().constData());
        // UDF and exFAT need the "Basic Data" / IFS type: use ntfs for that.
        if (!fsType && (p.fsType == FsType::Udf || p.fsType == FsType::Exfat))
            fsType = ped_file_system_type_get("ntfs");
        if (!fsType)
            fsType = ped_file_system_type_get("ext2");  // Linux (83)
        if (fsType) {
            OperationDetail &c = od.addChild(QCoreApplication::translate("Parted", "new partition type: %1").arg(QString::fromLatin1(fsType->name)));
            ok = true;
            // Clear libparted flags so they don't override the type implied by the file system.
            for (PedPartitionFlag f : allFlags())
                if (ped_partition_is_flag_available(lp, f) && ped_partition_get_flag(lp, f))
                    ok = ped_partition_set_flag(lp, f, 0) && ok;
            ok = ok && ped_partition_set_system(lp, fsType);
            c.setSuccessAndCaptureErrors(ok);
            if (ok && p.isFlagSet(QStringLiteral("lba")) && ped_partition_is_flag_available(lp, PED_PARTITION_LBA))
                ok = setFlagStep(lp, PED_PARTITION_LBA, true, od);
        }
    }
    ok = ok && commit(disk.disk);
    od.setSuccessAndCaptureErrors(ok);
    return ok;
}

bool setFlags(const Partition &p, const QStringList &from, const QStringList &to, OperationDetail &detail)
{
    OperationDetail &od = detail.addChild(QCoreApplication::translate("Parted", "set partition flags on %1").arg(p.path));
    DeviceRef dev(p.devicePath);
    DiskRef disk(dev ? dev.dev : nullptr);
    PedPartition *lp = disk ? findPartition(disk.disk, p) : nullptr;
    if (!lp) {
        od.setSuccessAndCaptureErrors(false);
        return false;
    }
    bool ok = true;
    bool changed = false;
    // Pass 1 clears the flags removed by the user, pass 2 sets every wanted flag that
    // is not set at that point.  Re-reading the state between the passes copes with
    // aliased flags (clearing "boot" on GPT also clears "esp").
    for (PedPartitionFlag f : allFlags()) {
        if (!ped_partition_is_flag_available(lp, f))
            continue;
        const QString name = QString::fromLatin1(ped_partition_flag_get_name(f));
        if (from.contains(name) && !to.contains(name) && ped_partition_get_flag(lp, f)) {
            ok = setFlagStep(lp, f, false, od) && ok;
            changed = true;
        }
    }
    for (PedPartitionFlag f : allFlags()) {
        if (!ok)
            break;
        if (!ped_partition_is_flag_available(lp, f))
            continue;
        const QString name = QString::fromLatin1(ped_partition_flag_get_name(f));
        if (to.contains(name) && !ped_partition_get_flag(lp, f)) {
            ok = setFlagStep(lp, f, true, od) && ok;
            changed = true;
        }
    }
    if (ok && changed)
        ok = commit(disk.disk);
    od.setSuccessAndCaptureErrors(ok);
    return ok;
}

QStringList readFlags(const Partition &p)
{
    QStringList flags;
    DeviceRef dev(p.devicePath);
    DiskRef disk(dev ? dev.dev : nullptr);
    PedPartition *lp = disk ? findPartition(disk.disk, p) : nullptr;
    if (lp)
        for (PedPartitionFlag f : allFlags())
            if (ped_partition_is_flag_available(lp, f) && ped_partition_get_flag(lp, f))
                flags.append(QString::fromLatin1(ped_partition_flag_get_name(f)));
    OperationDetail::takeLibpartedMessages();
    return flags;
}

bool setName(const Partition &p, OperationDetail &detail)
{
    OperationDetail &od = detail.addChild(p.name.isEmpty()
                                              ? QCoreApplication::translate("Parted", "Clear partition name on %1").arg(p.path)
                                              : QCoreApplication::translate("Parted", "Set partition name to \"%1\" on %2").arg(p.name, p.path));
    bool ok = false;
    DeviceRef dev(p.devicePath);
    if (dev) {
        DiskRef disk(dev.dev);
        if (disk) {
            if (PedPartition *lp = findPartition(disk.disk, p))
                ok = ped_partition_set_name(lp, p.name.toUtf8().constData()) && commit(disk.disk);
        }
    }
    od.setSuccessAndCaptureErrors(ok);
    return ok;
}

bool eraseSignatures(const Partition &p, OperationDetail &detail)
{
    OperationDetail &od = detail.addChild(QCoreApplication::translate("Parted", "clear old file system signatures in %1").arg(p.path));

    DeviceRef dev(p.devicePath);
    if (!dev) {
        od.setSuccessAndCaptureErrors(false);
        return false;
    }
    const ByteValue sectorSize = dev.dev->sector_size;
    const ByteValue bufSize = std::max<ByteValue>(4 * KiB, sectorSize);
    std::vector<char> buf(size_t(bufSize), 0);

    PedGeometry *geom = nullptr;
    if (p.type == PartitionType::Unpartitioned) {
        geom = ped_geometry_new(dev.dev, p.start, p.length());
    } else {
        DiskRef disk(dev.dev);
        if (disk)
            if (PedPartition *lp = findPartition(disk.disk, p))
                geom = ped_geometry_duplicate(&lp->geom);
    }
    bool ok = geom && ped_device_open(dev.dev);
    const bool opened = ok;

    // Super block locations, see GParted_Core::erase_filesystem_signatures().
    struct Range { ByteValue offset, rounding, length; };
    static const Range ranges[] = {
        { 0, 1, 512 * KiB },                 // super blocks at start (incl. ZFS L0/L1)
        { 64 * MiB, 1, 4 * KiB },            // btrfs mirror copies
        { 256 * GiB, 1, 4 * KiB },
        { 1 * PiB, 1, 4 * KiB },
        { -3087LL * 512, 1, 512 },           // Promise FastTrack RAID
        { -1 * MiB, 128 * KiB, 4 * KiB },    // bcachefs backup super block
        { -1 * MiB, 256 * KiB, 4 * KiB },
        { -1 * MiB, 512 * KiB, 4 * KiB },
        { -1 * MiB, 1 * MiB, 4 * KiB },
        { -512 * KiB, 256 * KiB, 768 * KiB } // super blocks at end (RAID, nilfs2, ZFS L2/L3)
    };
    const ByteValue partBytes = p.byteLength();
    ByteValue prevOffset = -1, prevLen = -1;
    for (const Range &r : ranges) {
        if (!ok)
            break;
        const ByteValue rounding = ceilSize(r.rounding, sectorSize);
        ByteValue offset, len;
        if (r.offset >= 0) {
            offset = floorSize(r.offset, rounding);
            len = ceilSize(r.offset + r.length, sectorSize) - offset;
        } else {
            const ByteValue notional = floorSize(partBytes + r.offset, r.rounding);
            offset = floorSize(notional, rounding);
            len = ceilSize(notional + r.length, sectorSize) - offset;
        }
        if (offset + len <= 0)
            continue;
        if (offset < 0) {
            len += offset;
            offset = 0;
        }
        if (offset >= partBytes)
            continue;
        if (offset + len > partBytes)
            len = partBytes - offset;
        if (offset == prevOffset && len == prevLen)
            continue;

        OperationDetail &c = od.addChild(QCoreApplication::translate("Parted", "write %1 of zeros at byte offset %2").arg(formatBytes(len)).arg(offset));
        ByteValue written = 0;
        bool zeroOk = true;
        while (written < len) {
            const ByteValue amount = std::min(bufSize, len - written);
            zeroOk = ped_geometry_write(geom, buf.data(), (offset + written) / sectorSize, amount / sectorSize);
            if (!zeroOk)
                break;
            written += amount;
        }
        prevOffset = offset;
        prevLen = len;
        c.setSuccessAndCaptureErrors(zeroOk);
        ok = ok && zeroOk;
    }
    if (geom)
        ped_geometry_destroy(geom);

    // The kernel doesn't keep whole disk and partition caches coherent: flush.
    if (ok) {
        OperationDetail &c = od.addChild(QCoreApplication::translate("Parted", "flush operating system cache of %1").arg(QString::fromLocal8Bit(dev.dev->path)));
        const bool synced = ped_device_sync(dev.dev);
        c.setSuccessAndCaptureErrors(synced);
        ok = synced;
    }
    if (opened) {
        ped_device_close(dev.dev);
        settle();
    }
    od.setSuccessAndCaptureErrors(ok);
    return ok;
}

// --- F2: resize / move / copy primitives -------------------------------------------

static QString resizeDescription(const Partition &o, const Partition &n)
{
    const QString from = formatSize(o.length(), o.sectorSize);
    const QString to = formatSize(n.length(), n.sectorSize);
    const bool grow = n.length() > o.length();
    const bool shrink = n.length() < o.length();
    const bool right = n.start > o.start && n.end > o.end;
    const bool left = n.start < o.start && n.end < o.end;
    if (right)
        return grow ? QCoreApplication::translate("Parted", "move partition to the right and grow it from %1 to %2").arg(from, to)
             : shrink ? QCoreApplication::translate("Parted", "move partition to the right and shrink it from %1 to %2").arg(from, to)
                      : QCoreApplication::translate("Parted", "move partition to the right");
    if (left)
        return grow ? QCoreApplication::translate("Parted", "move partition to the left and grow it from %1 to %2").arg(from, to)
             : shrink ? QCoreApplication::translate("Parted", "move partition to the left and shrink it from %1 to %2").arg(from, to)
                      : QCoreApplication::translate("Parted", "move partition to the left");
    if (grow)
        return QCoreApplication::translate("Parted", "grow partition from %1 to %2").arg(from, to);
    if (shrink)
        return QCoreApplication::translate("Parted", "shrink partition from %1 to %2").arg(from, to);
    return QCoreApplication::translate("Parted", "resize/move partition");
}

static QString boundsText(const char *startLabel, const char *endLabel, const char *sizeLabel, Sector start,
                          Sector end, ByteValue sectorSize)
{
    return QCoreApplication::translate("Parted", startLabel).arg(start) + QLatin1Char('\n') + QCoreApplication::translate("Parted", endLabel).arg(end) + QLatin1Char('\n')
         + QCoreApplication::translate("Parted", sizeLabel).arg(end - start + 1).arg(formatSize(end - start + 1, sectorSize));
}

// Change the geometry of the partition found by `lookup` (strict match, or any active
// partition of the right type at the given sector for rollback).
static bool setPartitionGeometry(const Partition &lookup, bool strictLookup, const Partition &target,
                                 Sector &newStart, Sector &newEnd)
{
    DeviceRef dev(lookup.devicePath);
    DiskRef disk(dev ? dev.dev : nullptr);
    if (!disk)
        return false;
    PedPartition *lp = nullptr;
    if (strictLookup) {
        lp = findPartition(disk.disk, lookup);
    } else {
        lp = lookup.type == PartitionType::Extended ? ped_disk_extended_partition(disk.disk)
                                                     : ped_disk_get_partition_by_sector(disk.disk, lookup.middleSector());
        if (lp && !ped_partition_is_active(lp))
            lp = nullptr;
    }
    if (!lp)
        return false;
    PedConstraint *constraint = nullptr;
    if (PedGeometry *geom = ped_geometry_new(dev.dev, target.start, target.length())) {
        constraint = ped_constraint_exact(geom);
        ped_geometry_destroy(geom);
    }
    if (!constraint)
        return false;
    bool ok = false;
    if (ped_disk_set_partition_geom(disk.disk, lp, constraint, target.start, target.end)) {
        newStart = lp->geom.start;
        newEnd = lp->geom.end;
        ok = newStart == target.start && newEnd == target.end && commit(disk.disk);
    }
    ped_constraint_destroy(constraint);
    return ok;
}

bool resizeMovePartition(const Partition &o, Partition &n, OperationDetail &detail, bool rollbackOnFail)
{
    if (n.type == PartitionType::Unpartitioned)
        return true;  // nothing to change on a whole disk device

    OperationDetail &od = detail.addChild(resizeDescription(o, n));
    if (o.start == n.start && o.end == n.end) {
        od.addChild(QCoreApplication::translate("Parted", "new and old partition have the same size and position.  Hence skipping this operation"),
                    OperationDetail::Status::None, OperationDetail::Font::Italic);
        od.setSuccessAndCaptureErrors(true);
        return true;
    }
    od.addChild(boundsText("old start: %1", "old end: %1", "old size: %1 (%2)", o.start, o.end, o.sectorSize),
                OperationDetail::Status::None, OperationDetail::Font::Italic);

    Sector newStart = -1, newEnd = -1;
    const bool ok = setPartitionGeometry(o, true, n, newStart, newEnd);
    if (ok) {
        od.addChild(boundsText("new start: %1", "new end: %1", "new size: %1 (%2)", newStart, newEnd, n.sectorSize),
                    OperationDetail::Status::None, OperationDetail::Font::Italic);
        waitForPath(n.path);
    } else {
        od.addChild(boundsText("requested start: %1", "requested end: %1", "requested size: %1 (%2)", n.start, n.end,
                               n.sectorSize),
                    OperationDetail::Status::None, OperationDetail::Font::Italic);
    }
    od.setSuccessAndCaptureErrors(ok);

    if (!ok && rollbackOnFail) {
        // The failed change may or may not have reached the disk: find the partition
        // by the middle of the old/new intersection and put the old geometry back.
        OperationDetail &rb = detail.addChild(QCoreApplication::translate("Parted", "attempt to rollback failed change to the partition"));
        Partition intersection = n;
        intersection.start = std::max(o.start, n.start);
        intersection.end = std::min(o.end, n.end);
        Sector s = -1, e = -1;
        const bool restored = setPartitionGeometry(intersection, false, o, s, e);
        rb.addChild(boundsText("original start: %1", "original end: %1", "original size: %1 (%2)", o.start, o.end,
                               o.sectorSize),
                    OperationDetail::Status::None, OperationDetail::Font::Italic);
        rb.setSuccessAndCaptureErrors(restored);
    }
    return ok;
}

bool calculateExactGeometry(const Partition &o, Partition &n, OperationDetail &detail)
{
    OperationDetail &od = detail.addChild(QCoreApplication::translate("Parted", "calculate new size and position of %1").arg(n.path));
    od.addChild(boundsText("requested start: %1", "requested end: %1", "requested size: %1 (%2)", n.start, n.end,
                           n.sectorSize),
                OperationDetail::Status::None, OperationDetail::Font::Italic);
    bool ok = false;
    DeviceRef dev(o.devicePath);
    DiskRef disk(dev ? dev.dev : nullptr);
    if (disk) {
        if (PedPartition *lp = findPartition(disk.disk, o)) {
            if (PedConstraint *constraint = ped_constraint_any(dev.dev)) {
                // In memory only: the table is not committed.
                if (ped_disk_set_partition_geom(disk.disk, lp, constraint, n.start, n.end)) {
                    n.start = lp->geom.start;
                    n.end = lp->geom.end;
                    ok = true;
                }
                ped_constraint_destroy(constraint);
            }
        }
    }
    if (ok)
        od.addChild(boundsText("new start: %1", "new end: %1", "new size: %1 (%2)", n.start, n.end, n.sectorSize),
                    OperationDetail::Status::None, OperationDetail::Font::Italic);
    od.setSuccessAndCaptureErrors(ok);
    return ok;
}

bool libpartedFsResize(const Partition &o, const Partition &n, OperationDetail &detail)
{
    detail.addChild(QCoreApplication::translate("Parted", "using libparted"), OperationDetail::Status::None);
    bool ok = false;
    DeviceRef dev(o.devicePath);
    if (!dev)
        return false;
    if (PedGeometry *oldGeom = ped_geometry_new(dev.dev, o.start, o.length())) {
        PedFileSystem *fs = ped_file_system_open(oldGeom);
        ped_geometry_destroy(oldGeom);
        if (fs) {
            if (PedGeometry *newGeom = ped_geometry_new(dev.dev, n.start, n.length())) {
                ok = ped_file_system_resize(fs, newGeom, nullptr);
                ped_geometry_destroy(newGeom);
            }
            ped_file_system_close(fs);
        }
    }
    if (ok) {
        // Flush the caches so later tools working on the partition node see the change.
        if (ped_device_open(dev.dev)) {
            ped_device_sync(dev.dev);
            ped_device_close(dev.dev);
        }
    }
    return ok;
}

void libpartedFsUsage(Partition &p)
{
    DeviceRef dev(p.devicePath);
    if (!dev)
        return;
    if (PedGeometry *geom = ped_geometry_new(dev.dev, p.start, p.length())) {
        if (PedFileSystem *fs = ped_file_system_open(geom)) {
            if (PedConstraint *c = ped_file_system_get_resize_constraint(fs)) {
                p.setUsage(fs->geom->length, fs->geom->length - c->min_size);
                ped_constraint_destroy(c);
            }
            ped_file_system_close(fs);
        }
        ped_geometry_destroy(geom);
    }
    OperationDetail::takeLibpartedMessages();
}

bool copyBlocks(const QString &srcDevice, const QString &dstDevice, Sector srcStart, Sector dstStart,
                ByteValue length, OperationDetail &detail, ByteValue &done)
{
    done = 0;
    detail.addChild(QCoreApplication::translate("Parted", "using internal algorithm"), OperationDetail::Status::None);
    OperationDetail &od = detail.addChild(QCoreApplication::translate("Parted", "copy %1").arg(formatBytes(length)));

    DeviceRef src(srcDevice);
    std::unique_ptr<DeviceRef> dstOwner;
    PedDevice *dst = nullptr;
    if (src && srcDevice != dstDevice) {
        dstOwner = std::make_unique<DeviceRef>(dstDevice);
        dst = dstOwner->dev;
    } else {
        dst = src.dev;
    }
    if (!src || !dst) {
        od.setSuccessAndCaptureErrors(false);
        return false;
    }
    const ByteValue sectorSize = src.dev->sector_size;
    if (dst->sector_size != sectorSize || length % sectorSize != 0) {
        od.addChild(QCoreApplication::translate("Parted", "Copying between devices with different sector sizes is not supported"),
                    OperationDetail::Status::Error);
        return false;
    }
    if (!ped_device_open(src.dev)) {
        od.setSuccessAndCaptureErrors(false);
        return false;
    }
    if (dst != src.dev && !ped_device_open(dst)) {
        ped_device_close(src.dev);
        od.setSuccessAndCaptureErrors(false);
        return false;
    }

    // Within one device, a copy to a higher address goes backwards so overlapping
    // source data is read before it is overwritten.
    const bool backwards = dst == src.dev && dstStart > srcStart;
    constexpr ByteValue kBlock = 4 * MiB;
    void *raw = nullptr;
    if (posix_memalign(&raw, 4096, size_t(kBlock)) != 0)
        raw = nullptr;
    std::unique_ptr<void, decltype(&std::free)> buffer(raw, &std::free);

    OperationDetail &progress = od.addChild(QString(), OperationDetail::Status::None, OperationDetail::Font::Italic);
    QElapsedTimer tick;
    tick.start();
    bool ok = buffer != nullptr;
    QString error;
    while (ok && done < length) {
        const ByteValue chunk = std::min(kBlock, length - done);
        const ByteValue offset = backwards ? length - done - chunk : done;
        const Sector count = chunk / sectorSize;
        if (!ped_device_read(src.dev, buffer.get(), srcStart + offset / sectorSize, count)) {
            error = QCoreApplication::translate("Parted", "Error while reading block at sector %1").arg(srcStart + offset / sectorSize);
            ok = false;
        } else if (!ped_device_write(dst, buffer.get(), dstStart + offset / sectorSize, count)) {
            error = QCoreApplication::translate("Parted", "Error while writing block at sector %1").arg(dstStart + offset / sectorSize);
            ok = false;
        } else {
            done += chunk;
        }
        if (tick.elapsed() > 500 || done == length || !ok) {
            progress.setDescription(QCoreApplication::translate("Parted", "%1 of %2 copied").arg(formatBytes(done), formatBytes(length)));
            tick.restart();
        }
    }
    if (!error.isEmpty())
        progress.addChild(error, OperationDetail::Status::None, OperationDetail::Font::Italic);
    ok = ped_device_sync(dst) && ok;
    if (dst != src.dev)
        ped_device_close(dst);
    ped_device_close(src.dev);
    settle();
    detail.addChild(QCoreApplication::translate("Parted", "%1 (%2 B) copied").arg(formatBytes(done)).arg(done), OperationDetail::Status::None);
    od.setSuccessAndCaptureErrors(ok);
    return ok;
}

bool updateBootSector(const Partition &p, OperationDetail &detail)
{
    if (p.fsType != FsType::Ntfs)
        return true;
    // Windows only boots when the NTFS boot sector's "hidden sectors" (offset 0x1C)
    // matches the partition start.
    OperationDetail &od = detail.addChild(QCoreApplication::translate("Parted", "update boot sector of %1 file system on %2").arg(fsName(p.fsType), p.path));
    if ((p.start >> 32) != 0) {
        od.addChild(QCoreApplication::translate("Parted", "Partition start (%1) is beyond sector 4294967295 (2^32-1).\n"
                       "Windows will not be able to boot from this file system.").arg(p.start),
                    OperationDetail::Status::None, OperationDetail::Font::Italic);
        od.setStatus(OperationDetail::Status::Warning);
        return true;
    }
    const quint32 v = qToLittleEndian(quint32(p.start));
    const int fd = ::open(p.path.toLocal8Bit().constData(), O_WRONLY | O_CLOEXEC);
    bool ok = fd >= 0 && ::pwrite(fd, &v, sizeof v, 0x1C) == ssize_t(sizeof v) && ::fsync(fd) == 0;
    if (fd >= 0)
        ::close(fd);
    if (!ok)
        od.addChild(QCoreApplication::translate("Parted", "Error trying to write to boot sector in %1").arg(p.path), OperationDetail::Status::None,
                    OperationDetail::Font::Italic);
    od.setSuccessAndCaptureErrors(ok);
    return ok;
}

void composePartitionFlags(Partition &p, const QString &diskType)
{
    const bool isFat = p.fsType == FsType::Fat16 || p.fsType == FsType::Fat32;
    if (p.isFlagSet(QStringLiteral("esp")) && isFat) {
        p.setOnlyFlag(QStringLiteral("esp"));
        return;
    }
    if (p.fsType == FsType::Lvm2Pv) {
        p.setOnlyFlag(QStringLiteral("lvm"));
        return;
    }
    if (diskType == QLatin1String("msdos") && (p.type == PartitionType::Extended || isFat)) {
        p.setOnlyFlag(QStringLiteral("lba"));
        return;
    }
    p.flags.clear();
}

} // namespace Parted
} // namespace QtParted

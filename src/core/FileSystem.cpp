/*
 * QtParted - a Qt frontend to libparted for manipulating disk partitions
 * Copyright (C) 2002-2026 Vanni Brutto <zanac4ever@gmail.com>
 *
 * Commands, limits and usage parsing for each file system are ported from GParted
 * (https://gparted.org), Copyright (C) 2004 Bart Hakvoort and the GParted contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "FileSystem.h"

#include "Command.h"
#include "OperationDetail.h"

#include <QCoreApplication>
#include <QFile>
#include <QRegularExpression>

#include <cerrno>
#include <cstring>

#include <sys/statvfs.h>

namespace QtParted {

namespace {

// Return the first capture of `pattern` (multiline) parsed as an integer, or -1.
qint64 scan(const QString &text, const QString &pattern, int base = 10)
{
    const QRegularExpression re(pattern, QRegularExpression::MultilineOption);
    const auto m = re.match(text);
    if (!m.hasMatch())
        return -1;
    bool ok = false;
    const qint64 v = m.captured(1).remove(QLatin1Char(' ')).toLongLong(&ok, base);
    return ok ? v : -1;
}

void addToolMessages(Partition &p, const CommandResult &r)
{
    if (!r.output.trimmed().isEmpty())
        p.messages.append(r.output.trimmed());
    if (!r.error.trimmed().isEmpty())
        p.messages.append(r.error.trimmed());
}

QStringList labelArgs(const QString &option, const Partition &p)
{
    if (p.fsLabel.isEmpty())
        return {};
    return { option, p.fsLabel };
}

// --- usage readers (unmounted) ---------------------------------------------------

void ext2Usage(Partition &p)
{
    const CommandResult r = runCommand(QStringLiteral("dumpe2fs"), { QStringLiteral("-h"), p.path });
    if (!r.ok()) {
        addToolMessages(p, r);
        return;
    }
    const qint64 blockCount = scan(r.output, QStringLiteral("^Block count:\\s+(\\d+)"));
    const qint64 blockSize = scan(r.output, QStringLiteral("^Block size:\\s+(\\d+)"));
    qint64 freeBlocks = -1;
    // resize2fs won't shrink below its own estimated minimum, so use it for free space.
    const CommandResult r2 = runCommand(QStringLiteral("resize2fs"), { QStringLiteral("-P"), p.path });
    if (r2.ok()) {
        const qint64 minBlocks = scan(r2.output, QStringLiteral("Estimated minimum size of the filesystem:\\s+(\\d+)"));
        if (minBlocks > -1 && blockCount > -1)
            freeBlocks = blockCount - minBlocks;
    }
    if (freeBlocks == -1)
        freeBlocks = scan(r.output, QStringLiteral("^Free blocks:\\s+(\\d+)"));
    if (blockCount > -1 && blockSize > -1 && freeBlocks > -1) {
        p.setUsage(blockCount * blockSize / p.sectorSize, freeBlocks * blockSize / p.sectorSize);
        p.fsBlockSize = blockSize;
    }
}

void fatUsage(Partition &p)
{
    const CommandResult r = runCommand(QStringLiteral("mdir"), { QStringLiteral("-i"), p.path, QStringLiteral("::/") });
    if (!r.ok()) {
        addToolMessages(p, r);
        return;
    }
    const qint64 bytesFree = scan(r.output, QStringLiteral("^\\s*([0-9 ]*) bytes free$"));
    const CommandResult r2 = runCommand(QStringLiteral("minfo"), { QStringLiteral("-i"), p.path, QStringLiteral("::") });
    if (!r2.ok()) {
        addToolMessages(p, r2);
        return;
    }
    const qint64 logicalSectorSize = scan(r2.output, QStringLiteral("sector size:\\s*(\\d+) bytes"));
    const qint64 clusterSize = scan(r2.output, QStringLiteral("cluster size:\\s*(\\d+) sectors"));
    const qint64 smallSize = scan(r2.output, QStringLiteral("small size:\\s*(\\d+) sectors"));
    const qint64 bigSize = scan(r2.output, QStringLiteral("big size:\\s*(\\d+) sectors"));
    const qint64 logicalSectors = smallSize > 0 ? smallSize : bigSize;
    if (bytesFree > -1 && logicalSectorSize > 0 && clusterSize > 0 && logicalSectors > 0) {
        p.setUsage(logicalSectors * logicalSectorSize / p.sectorSize, bytesFree / p.sectorSize);
        p.fsBlockSize = logicalSectorSize * clusterSize;
    }
}

void ntfsUsage(Partition &p)
{
    const CommandResult r = runCommand(QStringLiteral("ntfsinfo"), { QStringLiteral("--mft"), QStringLiteral("--force"), p.path });
    if (!r.ok()) {
        addToolMessages(p, r);
        return;
    }
    const qint64 clusterSize = scan(r.output, QStringLiteral("Cluster Size:\\s*(\\d+)"));
    const qint64 volumeSize = scan(r.output, QStringLiteral("Volume Size in Clusters:\\s*(\\d+)"));
    const qint64 freeClusters = scan(r.output, QStringLiteral("Free Clusters:\\s*(\\d+)"));
    if (clusterSize > -1 && volumeSize > -1 && freeClusters > -1) {
        p.setUsage(volumeSize * clusterSize / p.sectorSize, freeClusters * clusterSize / p.sectorSize);
        p.fsBlockSize = clusterSize;
    }
}

void btrfsUsage(Partition &p)
{
    const CommandResult r = runCommand(QStringLiteral("btrfs"),
                                       { QStringLiteral("inspect-internal"), QStringLiteral("dump-super"), p.path });
    if (!r.ok()) {
        addToolMessages(p, r);
        return;
    }
    const qint64 totalBytes = scan(r.output, QStringLiteral("^total_bytes\\s+(\\d+)"));
    const qint64 bytesUsed = scan(r.output, QStringLiteral("^bytes_used\\s+(\\d+)"));
    const qint64 sectorSize = scan(r.output, QStringLiteral("^sectorsize\\s+(\\d+)"));
    const qint64 devTotal = scan(r.output, QStringLiteral("^dev_item\\.total_bytes\\s+(\\d+)"));
    if (totalBytes > 0 && bytesUsed > -1 && devTotal > -1 && sectorSize > -1) {
        const Sector fsSize = devTotal / p.sectorSize;
        const double fraction = double(devTotal) / double(totalBytes);
        const Sector used = qRound64(bytesUsed * fraction) / p.sectorSize;
        p.setUsage(fsSize, fsSize - used);
        p.fsBlockSize = sectorSize;
    }
}

void xfsUsage(Partition &p)
{
    const CommandResult r = runCommand(QStringLiteral("xfs_db"),
                                       { QStringLiteral("-r"), QStringLiteral("-c"), QStringLiteral("sb 0"),
                                         QStringLiteral("-c"), QStringLiteral("print blocksize"),
                                         QStringLiteral("-c"), QStringLiteral("print dblocks"),
                                         QStringLiteral("-c"), QStringLiteral("print fdblocks"), p.path });
    if (!r.ok()) {
        addToolMessages(p, r);
        return;
    }
    const qint64 blockSize = scan(r.output, QStringLiteral("^blocksize = (\\d+)"));
    const qint64 dblocks = scan(r.output, QStringLiteral("^dblocks = (\\d+)"));
    const qint64 fdblocks = scan(r.output, QStringLiteral("^fdblocks = (\\d+)"));
    if (blockSize > -1 && dblocks > -1 && fdblocks > -1) {
        p.setUsage(dblocks * blockSize / p.sectorSize, fdblocks * blockSize / p.sectorSize);
        p.fsBlockSize = blockSize;
    }
}

void exfatUsage(Partition &p)
{
    const CommandResult r = runCommand(QStringLiteral("dump.exfat"), { p.path });
    if (!r.ok()) {
        addToolMessages(p, r);
        return;
    }
    const qint64 volumeLength = scan(r.output, QStringLiteral("Volume Length\\(sectors\\):\\s*(\\d+)"));
    qint64 bytesPerSector = scan(r.output, QStringLiteral("Bytes per Sector:\\s*(\\d+)"));
    if (bytesPerSector == -1) {
        const qint64 shift = scan(r.output, QStringLiteral("Sector Size Bits:\\s*(\\d+)"));
        if (shift > -1)
            bytesPerSector = 1LL << shift;
    }
    qint64 sectorsPerCluster = scan(r.output, QStringLiteral("Sectors per Cluster:\\s*(\\d+)"));
    if (sectorsPerCluster == -1) {
        const qint64 shift = scan(r.output, QStringLiteral("Sector per Cluster bits:\\s*(\\d+)"));
        if (shift > -1)
            sectorsPerCluster = 1LL << shift;
    }
    const qint64 freeClusters = scan(r.output, QStringLiteral("Free Clusters:\\s*(\\d+)"));
    if (volumeLength > -1 && bytesPerSector > -1 && sectorsPerCluster > -1 && freeClusters > -1) {
        const qint64 clusterSize = bytesPerSector * sectorsPerCluster;
        p.setUsage(volumeLength * bytesPerSector / p.sectorSize, freeClusters * clusterSize / p.sectorSize);
        p.fsBlockSize = clusterSize;
    }
}

void f2fsUsage(Partition &p)
{
    const CommandResult r = runCommand(QStringLiteral("dump.f2fs"), { QStringLiteral("-d"), QStringLiteral("1"), p.path });
    if (!r.ok()) {
        addToolMessages(p, r);
        return;
    }
    const qint64 userBlocks = scan(r.output, QStringLiteral("user_block_count\\s*\\[0x\\s*[0-9a-f]+ : (\\d+)\\]"));
    const qint64 validBlocks = scan(r.output, QStringLiteral("valid_block_count\\s*\\[0x\\s*[0-9a-f]+ : (\\d+)\\]"));
    const qint64 logBlockSize = scan(r.output, QStringLiteral("log_blocksize\\s*\\[0x\\s*[0-9a-f]+ : (\\d+)\\]"));
    const qint64 fsSectorSize = scan(r.output, QStringLiteral("Info: sector size = (\\d+)"));
    const qint64 totalFsSectors = scan(r.output, QStringLiteral("Info: total FS sectors = (\\d+)"));
    if (userBlocks > -1 && validBlocks > -1 && logBlockSize > -1) {
        const qint64 blockSize = 1LL << logBlockSize;
        Sector fsSize = -1;
        if (fsSectorSize > -1 && totalFsSectors > -1)
            fsSize = totalFsSectors * fsSectorSize / p.sectorSize;
        p.setUsage(fsSize, (userBlocks - validBlocks) * blockSize / p.sectorSize);
        p.fsBlockSize = blockSize;
    }
}

void jfsUsage(Partition &p)
{
    const CommandResult r = runCommand(QStringLiteral("jfs_debugfs"), { p.path }, nullptr,
                                       QByteArrayLiteral("superblock\nx\ndmap\nx\nquit\n"));
    if (!r.ok()) {
        addToolMessages(p, r);
        return;
    }
    const qint64 aggSize = scan(r.output, QStringLiteral("s_size:\\s*0x([0-9a-fA-F]+)"), 16);
    const qint64 aggBlockSize = scan(r.output, QStringLiteral("s_bsize:\\s*(\\d+)"));
    const qint64 physBlockSize = scan(r.output, QStringLiteral("s_pbsize:\\s*(\\d+)"));
    const qint64 logLen = scan(r.output, QStringLiteral("s_logpxd\\.len:\\s*(\\d+)"));
    const qint64 fsckLen = scan(r.output, QStringLiteral("s_fsckpxd\\.len:\\s*(\\d+)"));
    const qint64 freeBlocks = scan(r.output, QStringLiteral("dn_nfree:\\s*0x([0-9a-fA-F]+)"), 16);
    if (aggSize > -1 && aggBlockSize > 0 && physBlockSize > -1 && logLen > -1 && fsckLen > -1 && freeBlocks > -1) {
        const Sector fsSize = (aggSize * physBlockSize + logLen * aggBlockSize + fsckLen * aggBlockSize) / p.sectorSize;
        p.setUsage(fsSize, freeBlocks * aggBlockSize / p.sectorSize);
        p.fsBlockSize = aggBlockSize;
    }
}

void nilfs2Usage(Partition &p)
{
    const CommandResult r = runCommand(QStringLiteral("nilfs-tune"), { QStringLiteral("-l"), p.path });
    if (!r.ok()) {
        addToolMessages(p, r);
        return;
    }
    const qint64 deviceSize = scan(r.output, QStringLiteral("^Device size:\\s*(\\d+)"));
    const qint64 freeBlocks = scan(r.output, QStringLiteral("^Free blocks count:\\s*(\\d+)"));
    const qint64 blockSize = scan(r.output, QStringLiteral("^Block size:\\s*(\\d+)"));
    if (deviceSize > -1 && freeBlocks > -1 && blockSize > -1) {
        p.setUsage(deviceSize / p.sectorSize, freeBlocks * blockSize / p.sectorSize);
        p.fsBlockSize = blockSize;
    }
}

void udfUsage(Partition &p)
{
    const CommandResult r = runCommand(QStringLiteral("udfinfo"), { QStringLiteral("--utf8"), p.path });
    if (!r.ok()) {
        addToolMessages(p, r);
        return;
    }
    const qint64 blockSize = scan(r.output, QStringLiteral("^blocksize=(\\d+)$"));
    const qint64 blocks = scan(r.output, QStringLiteral("^blocks=(\\d+)$"));
    qint64 usedBlocks = scan(r.output, QStringLiteral("^usedblocks=(\\d+)$"));
    qint64 freeBlocks = scan(r.output, QStringLiteral("^freeblocks=(\\d+)$"));
    const qint64 behindBlocks = scan(r.output, QStringLiteral("^behindblocks=(\\d+)$"));
    if (blockSize < 0 || blocks < 0 || usedBlocks < 0 || freeBlocks < 0 || behindBlocks < 0)
        return;
    const qint64 udfBlocks = blocks - behindBlocks;
    if (usedBlocks > udfBlocks)
        usedBlocks = udfBlocks;
    if (freeBlocks > udfBlocks - usedBlocks)
        freeBlocks = 0;
    p.setUsage((udfBlocks * blockSize + p.sectorSize - 1) / p.sectorSize, freeBlocks * blockSize / p.sectorSize);
    p.fsBlockSize = blockSize;
}

void swapUsage(Partition &p)
{
    // Inactive swap space: all of it is free.
    p.setUsage(p.length(), p.length());
}

void lvm2PvUsage(Partition &p)
{
    const CommandResult r = runCommand(QStringLiteral("lvm"),
                                       { QStringLiteral("pvs"), QStringLiteral("--noheadings"), QStringLiteral("--units"),
                                         QStringLiteral("b"), QStringLiteral("--nosuffix"), QStringLiteral("-o"),
                                         QStringLiteral("pv_size,pv_free"), p.path });
    if (!r.ok()) {
        addToolMessages(p, r);
        return;
    }
    const QStringList f = r.output.simplified().split(QLatin1Char(' '));
    if (f.size() >= 2) {
        bool ok1 = false, ok2 = false;
        const qint64 size = f[0].toLongLong(&ok1);
        const qint64 freeB = f[1].toLongLong(&ok2);
        if (ok1 && ok2)
            p.setUsage(size / p.sectorSize, freeB / p.sectorSize);
    }
}

QString fatLabel(const QString &label)
{
    static const QString prohibited = QStringLiteral("*?.,;:/\\|+=<>[]\"'");
    QString out;
    for (const QChar c : label.toUpper())
        if (!prohibited.contains(c) && c >= QLatin1Char(' '))
            out += c;
    return out;
}

} // namespace

// ---------------------------------------------------------------------------------

FileSystem::FileSystem(Definition def)
    : m_def(std::move(def))
{
}

static bool hasAll(const QStringList &programs)
{
    for (const QString &p : programs)
        if (!Tools::has(p))
            return false;
    return true;
}

void FileSystem::detect()
{
    m_support = FsSupport();
    m_support.create = !m_def.mkfsProgram.isEmpty() && Tools::has(m_def.mkfsProgram);
    m_support.createWithLabel = m_support.create && m_def.labelAtCreate;
    bool usage = bool(m_def.readUsage);
    for (const QString &p : m_def.usagePrograms)
        usage = usage && Tools::has(p);
    m_support.readUsage = usage;

    const bool checkOk = !m_def.checkProgram.isEmpty() && Tools::has(m_def.checkProgram);
    m_support.check = checkOk;
    const bool resizeAllowed = !m_def.requireCheckForResize || checkOk;

    if (m_def.recreateResize && m_support.create) {
        m_support.grow = m_support.shrink = FsMethod::Recreate;
    } else {
        if (m_def.libpartedGrow)
            m_support.grow = FsMethod::Libparted;
        else if (m_def.resize && resizeAllowed && !m_def.growPrograms.isEmpty() && hasAll(m_def.growPrograms))
            m_support.grow = FsMethod::External;
        if (m_def.libpartedShrink)
            m_support.shrink = FsMethod::Libparted;
        else if (m_def.resize && resizeAllowed && !m_def.shrinkPrograms.isEmpty() && hasAll(m_def.shrinkPrograms)
                 && (!m_def.shrinkNeedsUsage || m_support.readUsage))
            m_support.shrink = FsMethod::External;
    }
    if (m_def.onlineGrow && m_support.grow == FsMethod::External)
        m_support.onlineGrow = FsMethod::External;
    if (m_def.onlineShrink && m_support.shrink == FsMethod::External)
        m_support.onlineShrink = FsMethod::External;

    const bool moveAllowed = !m_def.moveNeedsCheck || checkOk;
    if (m_def.recreateResize) {
        if (m_support.create)
            m_support.move = m_support.copy = FsMethod::Recreate;
    } else {
        if (m_def.movable && moveAllowed)
            m_support.move = FsMethod::Block;
        if (m_def.copyable && moveAllowed)
            m_support.copy = FsMethod::Block;
    }

    m_support.writeLabel = bool(m_def.writeLabel) && !m_def.labelPrograms.isEmpty() && hasAll(m_def.labelPrograms);
    m_support.onlineWriteLabel = m_support.writeLabel && m_def.onlineLabel;
    m_support.writeUuid = bool(m_def.writeUuid) && !m_def.uuidPrograms.isEmpty() && hasAll(m_def.uuidPrograms);
}

QStringList FileSystem::allPrograms() const
{
    QStringList all;
    if (!m_def.mkfsProgram.isEmpty())
        all << m_def.mkfsProgram;
    all += m_def.usagePrograms;
    all += m_def.growPrograms;
    all += m_def.shrinkPrograms;
    if (!m_def.checkProgram.isEmpty())
        all << m_def.checkProgram;
    all += m_def.labelPrograms;
    all += m_def.uuidPrograms;
    all.removeDuplicates();
    return all;
}

bool FileSystem::resize(const Partition &partition, OperationDetail &detail, bool fill) const
{
    if (!m_def.resize)
        return false;
    return m_def.resize(partition, detail, fill);
}

bool FileSystem::check(const Partition &partition, OperationDetail &detail) const
{
    if (!m_support.check || !m_def.checkArgs)
        return false;
    const CommandResult r = runCommand(m_def.checkProgram, m_def.checkArgs(partition), &detail);
    const bool ok = r.started && m_def.checkSuccessCodes.contains(r.exitCode);
    // The command detail shows the raw exit status; repairs (e2fsck exit 1) are fine.
    if (ok && r.exitCode != 0)
        detail.lastChild().setStatus(OperationDetail::Status::Success);
    return ok;
}

bool FileSystem::writeLabel(const Partition &partition, OperationDetail &detail) const
{
    if (!m_support.writeLabel)
        return false;
    Partition p = partition;
    p.fsLabel = sanitizeLabel(p.fsLabel);
    return m_def.writeLabel(p, detail);
}

bool FileSystem::writeUuid(const Partition &partition, OperationDetail &detail) const
{
    return m_support.writeUuid && m_def.writeUuid(partition, detail);
}

QString FileSystem::sanitizeLabel(const QString &label) const
{
    QString l = m_def.sanitizeLabel ? m_def.sanitizeLabel(label) : label;
    return l.left(fsLabelMaxLength(m_def.type));
}

bool FileSystem::create(const Partition &partition, OperationDetail &detail) const
{
    if (!m_support.create) {
        detail.addChild(QCoreApplication::translate("FileSystem", "%1 is not installed").arg(m_def.mkfsProgram),
                        OperationDetail::Status::Error);
        return false;
    }
    Partition p = partition;
    p.fsLabel = m_def.labelAtCreate ? sanitizeLabel(p.fsLabel) : QString();
    return runCommand(m_def.mkfsProgram, m_def.mkfsArgs(p), &detail).ok();
}

void FileSystem::readUsage(Partition &partition) const
{
    if (m_support.readUsage && m_def.readUsage)
        m_def.readUsage(partition);
}

// ---------------------------------------------------------------------------------

bool mountedUsage(const QString &mountPoint, ByteValue &fsSize, ByteValue &fsFree, QString &error)
{
    struct statvfs sfs;
    if (::statvfs(mountPoint.toLocal8Bit().constData(), &sfs) != 0) {
        error = QStringLiteral("statvfs(\"%1\"): %2").arg(mountPoint, QString::fromLocal8Bit(strerror(errno)));
        return false;
    }
    fsSize = ByteValue(sfs.f_blocks) * ByteValue(sfs.f_frsize);
    fsFree = ByteValue(sfs.f_bfree) * ByteValue(sfs.f_frsize);
    return true;
}

FileSystemRegistry &FileSystemRegistry::instance()
{
    static FileSystemRegistry registry;
    return registry;
}

FileSystemRegistry::FileSystemRegistry()
{
    using D = FileSystem::Definition;
    auto add = [this](D d) {
        const FsType t = d.type;
        m_fs.emplace(t, std::make_unique<FileSystem>(std::move(d)));
    };

    auto ext = [&](FsType t, const QString &mkfs) {
        D d;
        d.type = t;
        d.mkfsProgram = mkfs;
        d.mkfsArgs = [](const Partition &p) {
            return QStringList{ QStringLiteral("-F"), QStringLiteral("-L"), p.fsLabel, p.path };
        };
        d.usagePrograms = { QStringLiteral("dumpe2fs"), QStringLiteral("resize2fs") };
        d.readUsage = ext2Usage;
        // 2^32 - 1 blocks for ext2/3; ext4 in 64-bit mode has no practical limit.
        if (t != FsType::Ext4)
            d.limits.maxSize = floorSize(16 * TiB - 4 * KiB, MiB);
        d.software = QStringLiteral("e2fsprogs");
        add(std::move(d));
    };
    ext(FsType::Ext2, QStringLiteral("mkfs.ext2"));
    ext(FsType::Ext3, QStringLiteral("mkfs.ext3"));
    ext(FsType::Ext4, QStringLiteral("mkfs.ext4"));

    auto fat = [&](FsType t, const QString &bits, FsLimits limits) {
        D d;
        d.type = t;
        d.mkfsProgram = QStringLiteral("mkfs.fat");
        d.mkfsArgs = [bits](const Partition &p) {
            QStringList a{ QStringLiteral("-F") + bits, QStringLiteral("-v"), QStringLiteral("-I") };
            a += labelArgs(QStringLiteral("-n"), p);
            a << p.path;
            return a;
        };
        d.usagePrograms = { QStringLiteral("mdir"), QStringLiteral("minfo") };
        d.readUsage = fatUsage;
        d.limits = limits;
        d.software = QStringLiteral("dosfstools, mtools");
        d.sanitizeLabel = fatLabel;
        add(std::move(d));
    };
    fat(FsType::Fat16, QStringLiteral("16"), { 16 * MiB, (4096 - 1) * MiB });
    fat(FsType::Fat32, QStringLiteral("32"), { 33 * MiB, 2 * TiB });

    {
        D d;
        d.type = FsType::Ntfs;
        d.mkfsProgram = QStringLiteral("mkntfs");
        d.mkfsArgs = [](const Partition &p) {
            return QStringList{ QStringLiteral("-Q"), QStringLiteral("-v"), QStringLiteral("-F"),
                                QStringLiteral("-L"), p.fsLabel, p.path };
        };
        d.usagePrograms = { QStringLiteral("ntfsinfo") };
        d.readUsage = ntfsUsage;
        d.limits.minSize = 2 * MiB;
        d.software = QStringLiteral("ntfs-3g / ntfsprogs");
        add(std::move(d));
    }
    {
        D d;
        d.type = FsType::Btrfs;
        d.mkfsProgram = QStringLiteral("mkfs.btrfs");
        d.mkfsArgs = [](const Partition &p) {
            return QStringList{ QStringLiteral("-f"), QStringLiteral("-L"), p.fsLabel, p.path };
        };
        d.usagePrograms = { QStringLiteral("btrfs") };
        d.readUsage = btrfsUsage;
        d.limits.minSize = 256 * MiB;
        d.software = QStringLiteral("btrfs-progs");
        add(std::move(d));
    }
    {
        D d;
        d.type = FsType::Xfs;
        d.mkfsProgram = QStringLiteral("mkfs.xfs");
        d.mkfsArgs = [](const Partition &p) {
            return QStringList{ QStringLiteral("-f"), QStringLiteral("-L"), p.fsLabel, p.path };
        };
        d.usagePrograms = { QStringLiteral("xfs_db") };
        d.readUsage = xfsUsage;
        d.limits.minSize = 300 * MiB;
        d.software = QStringLiteral("xfsprogs");
        add(std::move(d));
    }
    {
        D d;
        d.type = FsType::Exfat;
        d.mkfsProgram = QStringLiteral("mkfs.exfat");
        d.mkfsArgs = [](const Partition &p) {
            QStringList a = labelArgs(QStringLiteral("-L"), p);
            a << p.path;
            return a;
        };
        d.usagePrograms = { QStringLiteral("dump.exfat") };
        d.readUsage = exfatUsage;
        d.software = QStringLiteral("exfatprogs");
        add(std::move(d));
    }
    {
        D d;
        d.type = FsType::F2fs;
        d.mkfsProgram = QStringLiteral("mkfs.f2fs");
        d.mkfsArgs = [](const Partition &p) {
            QStringList a{ QStringLiteral("-f") };
            a += labelArgs(QStringLiteral("-l"), p);
            a << p.path;
            return a;
        };
        d.usagePrograms = { QStringLiteral("dump.f2fs") };
        d.readUsage = f2fsUsage;
        d.software = QStringLiteral("f2fs-tools");
        add(std::move(d));
    }
    {
        D d;
        d.type = FsType::Hfs;
        d.mkfsProgram = QStringLiteral("hformat");
        d.mkfsArgs = [](const Partition &p) {
            QStringList a = labelArgs(QStringLiteral("-l"), p);
            a << p.path;
            return a;
        };
        d.limits.maxSize = 2048 * MiB;
        d.software = QStringLiteral("hfsutils");
        add(std::move(d));
    }
    {
        D d;
        d.type = FsType::HfsPlus;
        d.mkfsProgram = QStringLiteral("mkfs.hfsplus");
        d.mkfsArgs = [](const Partition &p) {
            QStringList a = labelArgs(QStringLiteral("-v"), p);
            a << p.path;
            return a;
        };
        d.software = QStringLiteral("hfsprogs");
        add(std::move(d));
    }
    {
        D d;
        d.type = FsType::Jfs;
        d.mkfsProgram = QStringLiteral("mkfs.jfs");
        d.mkfsArgs = [](const Partition &p) {
            return QStringList{ QStringLiteral("-q"), QStringLiteral("-L"), p.fsLabel, p.path };
        };
        d.usagePrograms = { QStringLiteral("jfs_debugfs") };
        d.readUsage = jfsUsage;
        d.limits.minSize = 16 * MiB;
        d.software = QStringLiteral("jfsutils");
        add(std::move(d));
    }
    {
        D d;
        d.type = FsType::LinuxSwap;
        d.mkfsProgram = QStringLiteral("mkswap");
        d.mkfsArgs = [](const Partition &p) {
            return QStringList{ QStringLiteral("-L"), p.fsLabel, p.path };
        };
        d.readUsage = swapUsage;
        d.software = QStringLiteral("util-linux");
        add(std::move(d));
    }
    {
        D d;
        d.type = FsType::Lvm2Pv;
        d.mkfsProgram = QStringLiteral("lvm");
        d.mkfsArgs = [](const Partition &p) {
            return QStringList{ QStringLiteral("pvcreate"), QStringLiteral("-M"), QStringLiteral("2"), p.path };
        };
        d.labelAtCreate = false;
        d.usagePrograms = { QStringLiteral("lvm") };
        d.readUsage = lvm2PvUsage;
        d.software = QStringLiteral("lvm2");
        add(std::move(d));
    }
    {
        D d;
        d.type = FsType::Minix;
        d.mkfsProgram = QStringLiteral("mkfs.minix");
        d.mkfsArgs = [](const Partition &p) { return QStringList{ QStringLiteral("-3"), p.path }; };
        d.labelAtCreate = false;
        d.software = QStringLiteral("util-linux");
        add(std::move(d));
    }
    {
        D d;
        d.type = FsType::Nilfs2;
        d.mkfsProgram = QStringLiteral("mkfs.nilfs2");
        d.mkfsArgs = [](const Partition &p) {
            return QStringList{ QStringLiteral("-L"), p.fsLabel, p.path };
        };
        d.usagePrograms = { QStringLiteral("nilfs-tune") };
        d.readUsage = nilfs2Usage;
        d.limits.minSize = 128 * MiB + 4 * KiB;
        d.software = QStringLiteral("nilfs-utils");
        add(std::move(d));
    }
    {
        D d;
        d.type = FsType::Reiser4;
        d.mkfsProgram = QStringLiteral("mkfs.reiser4");
        d.mkfsArgs = [](const Partition &p) {
            return QStringList{ QStringLiteral("--force"), QStringLiteral("--yes"), QStringLiteral("--label"),
                                p.fsLabel, p.path };
        };
        d.software = QStringLiteral("reiser4progs");
        add(std::move(d));
    }
    {
        D d;
        d.type = FsType::Reiserfs;
        d.mkfsProgram = QStringLiteral("mkreiserfs");
        d.mkfsArgs = [](const Partition &p) {
            return QStringList{ QStringLiteral("-f"), QStringLiteral("-f"), QStringLiteral("--label"),
                                p.fsLabel, p.path };
        };
        d.limits.minSize = 34 * MiB;
        d.software = QStringLiteral("reiserfsprogs");
        add(std::move(d));
    }
    {
        D d;
        d.type = FsType::Udf;
        d.mkfsProgram = QStringLiteral("mkudffs");
        d.mkfsArgs = [](const Partition &p) {
            QStringList a{ QStringLiteral("--utf8"), QStringLiteral("--media-type=hd"), QStringLiteral("--udfrev=0x201"),
                           QStringLiteral("--blocksize=%1").arg(p.sectorSize) };
            if (!p.fsLabel.isEmpty()) {
                // The volume identifier is limited to 30 Latin-1 or 15 other characters.
                QString vid = p.fsLabel.left(30);
                for (int i = 0; i < vid.size(); ++i)
                    if (vid[i].unicode() > 0xFF) {
                        vid = p.fsLabel.left(i < 15 ? 15 : i);
                        break;
                    }
                a << QStringLiteral("--lvid=") + p.fsLabel << QStringLiteral("--vid=") + vid;
            }
            a << p.path;
            return a;
        };
        d.usagePrograms = { QStringLiteral("udfinfo") };
        d.readUsage = udfUsage;
        d.limits.minSize = 300 * 512;
        d.software = QStringLiteral("udftools");
        add(std::move(d));
    }
    {
        D d;
        d.type = FsType::Bcachefs;
        d.mkfsProgram = QStringLiteral("bcachefs");
        d.mkfsArgs = [](const Partition &p) {
            QStringList a{ QStringLiteral("format") };
            a += labelArgs(QStringLiteral("-L"), p);
            a << p.path;
            return a;
        };
        d.limits.minSize = 32 * MiB;
        d.software = QStringLiteral("bcachefs-tools");
        add(std::move(d));
    }

    addResizeAndMaintenance();
    detect();
}

void FileSystemRegistry::detect()
{
    Tools::clearCache();
    for (auto &[type, fs] : m_fs)
        fs->detect();
}

FsSupport FileSystemRegistry::support(FsType type) const
{
    const FileSystem *fs = get(type);
    return fs ? fs->support() : FsSupport();
}

const FileSystem *FileSystemRegistry::get(FsType type) const
{
    auto it = m_fs.find(type);
    return it == m_fs.end() ? nullptr : it->second.get();
}

QList<const FileSystem *> FileSystemRegistry::all() const
{
    QList<const FileSystem *> list;
    for (FsType t : creatableFsTypes())
        if (const FileSystem *fs = get(t))
            list.append(fs);
    return list;
}

void FileSystemRegistry::readUsage(Partition &p) const
{
    if (!fsIsReal(p.fsType))
        return;
    if (p.busy) {
        if (p.fsType == FsType::LinuxSwap) {
            // Active swap: used size from /proc/swaps is shown by the kernel in KiB.
            QFile f(QStringLiteral("/proc/swaps"));
            if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                const QStringList lines = QString::fromLocal8Bit(f.readAll()).split(QLatin1Char('\n'));
                for (const QString &l : lines) {
                    const QStringList cols = l.simplified().split(QLatin1Char(' '));
                    if (cols.size() >= 4 && cols[0] == p.path) {
                        const Sector used = cols[3].toLongLong() * KiB / p.sectorSize;
                        p.setUsage(p.length(), p.length() - used);
                        break;
                    }
                }
            }
            return;
        }
        if (!p.mountPoints.isEmpty()) {
            ByteValue size = 0, freeB = 0;
            QString error;
            if (mountedUsage(p.mountPoints.first(), size, freeB, error))
                p.setUsage(size / p.sectorSize, freeB / p.sectorSize);
            else
                p.messages.append(error);
        }
        return;
    }
    if (const FileSystem *fs = get(p.fsType))
        fs->readUsage(p);
}

} // namespace QtParted

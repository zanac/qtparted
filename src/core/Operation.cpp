/*
 * QtParted - a Qt frontend to libparted for manipulating disk partitions
 * Copyright (C) 2002-2026 Vanni Brutto <zanac4ever@gmail.com>
 *
 * The pending operation model (visual re-application, merging) is ported from GParted
 * (https://gparted.org), Copyright (C) 2004 Bart Hakvoort and the GParted contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "Operation.h"

#include "Luks.h"

#include "FileSystem.h"
#include "Steps.h"

#include <QCoreApplication>

namespace QtParted {

static QString tr(const char *s)
{
    return QCoreApplication::translate("Operation", s);
}

Operation::Operation(OperationType type, const Device &device, const Partition &original,
                     const Partition &newPartition)
    : m_type(type)
    , m_device(device.withoutPartitions())
    , m_original(original)
    , m_new(newPartition)
    , m_detail(std::make_unique<OperationDetail>())
{
}

bool Operation::apply()
{
    OperationDetail::takeLibpartedMessages();
    m_detail->reset(m_description);
    m_detail->setStatus(OperationDetail::Status::Executing);
    const bool ok = execute(*m_detail);
    m_detail->setSuccessAndCaptureErrors(ok);
    return ok;
}

int Operation::findIndexOriginal(const PartitionVector &partitions) const
{
    for (size_t i = 0; i < partitions.size(); ++i)
        if (m_original.start >= partitions[i].start && m_original.end <= partitions[i].end)
            return int(i);
    return -1;
}

int Operation::findIndexNew(const PartitionVector &partitions) const
{
    for (size_t i = 0; i < partitions.size(); ++i)
        if (m_new.start >= partitions[i].start && m_new.end <= partitions[i].end)
            return int(i);
    return -1;
}

void Operation::substituteNew(PartitionVector &partitions) const
{
    if (m_original.insideExtended) {
        const int ext = findExtended(partitions);
        if (ext >= 0) {
            PartitionVector &logicals = partitions[size_t(ext)].logicals;
            const int index = findIndexOriginal(logicals);
            if (index >= 0)
                logicals[size_t(index)] = m_new;
        }
    } else {
        const int index = findIndexOriginal(partitions);
        if (index >= 0)
            partitions[size_t(index)] = m_new;
    }
}

Partition *Operation::findOriginal(PartitionVector &partitions) const
{
    PartitionVector *vector = &partitions;
    if (m_original.insideExtended) {
        const int ext = findExtended(partitions);
        if (ext < 0)
            return nullptr;
        vector = &partitions[size_t(ext)].logicals;
    }
    for (Partition &p : *vector)
        if (p == m_original)
            return &p;
    return nullptr;
}

void Operation::insertNew(PartitionVector &partitions) const
{
    // Creating applies to unallocated space: it only matters that the new partition fits.
    if (m_new.insideExtended) {
        const int ext = findExtended(partitions);
        if (ext >= 0) {
            Partition &extended = partitions[size_t(ext)];
            const int index = findIndexNew(extended.logicals);
            if (index >= 0) {
                extended.logicals[size_t(index)] = m_new;
                insertUnallocated(m_device.path, extended.logicals, extended.start, extended.end,
                                  m_device.sectorSize, true);
            }
        }
    } else {
        const int index = findIndexNew(partitions);
        if (index >= 0) {
            partitions[size_t(index)] = m_new;
            insertUnallocated(m_device.path, partitions, 0, m_device.length - 1, m_device.sectorSize, false);
        }
    }
}

// Right before changing a file system: the partition is where and as busy as when the
// operation was queued and, for a file system inside open LUKS encryption, the mapping is
// still the same and the file system inside is as busy as it was.
static bool verifyFileSystemTarget(Partition &p, OperationDetail &detail, bool online)
{
    if (!p.isOpenLuks())
        return Parted::verifyTarget(p, true, detail, online && p.busy);
    if (!Parted::verifyTarget(p, true, detail, true))
        return false;
    Luks::loadCache();
    const LuksMapping m = Luks::mapping(p.path);
    if (m.name != p.luks.name || m.offset != p.luks.offset || m.length != p.luks.length) {
        detail.addChild(QCoreApplication::translate("Operation",
                                                    "The encryption mapping of %1 changed since the operation was "
                                                    "queued. Refresh the devices and queue it again. Nothing was changed.")
                            .arg(p.path),
                        OperationDetail::Status::Error, OperationDetail::Font::Italic);
        return false;
    }
    Partition &f = *p.encrypted;
    return Parted::verifyTarget(f, false, detail, online && f.busy);
}

bool createFileSystem(const Partition &p, OperationDetail &detail)
{
    OperationDetail &od = detail.addChild(tr("create new %1 file system").arg(fsName(p.fsType)));
    const FileSystem *fs = FileSystemRegistry::instance().get(p.fsType);
    const bool ok = fs && fs->create(p, od);
    od.setSuccessAndCaptureErrors(ok);
    return ok;
}

// --- create partition table --------------------------------------------------------

OperationCreateTable::OperationCreateTable(const Device &device, const QString &diskType,
                                           const Parted::DiskTypeInfo &info)
    : Operation(OperationType::CreateTable, device,
                Partition::makeUnpartitioned(device.path, device.path, FsType::Unknown, device.length,
                                             device.sectorSize, false),
                Partition::makeUnallocated(device.path, 0, device.length - 1, device.sectorSize, false))
    , m_diskType(diskType)
    , m_info(info)
{
}

void OperationCreateTable::createDescription()
{
    m_description = tr("Create %1 partition table on %2").arg(m_diskType, m_device.path);
}

void OperationCreateTable::applyToVisual(Device &device)
{
    device.diskType = m_diskType;
    device.hasPartitionTable = true;
    device.maxPrimaries = m_info.maxPrimaries;
    device.maxNameLength = m_info.maxNameLength;
    device.supportsExtended = m_info.supportsExtended;
    device.highestBusy = 0;
    device.partitions.clear();
    insertUnallocated(device.path, device.partitions, 0, device.length - 1, device.sectorSize, false);
}

bool OperationCreateTable::execute(OperationDetail &detail)
{
    return Parted::verifyDiskUnused(m_device, detail) && Parted::createDiskLabel(m_device, m_diskType, detail);
}

// --- create partition ----------------------------------------------------------------

OperationCreate::OperationCreate(const Device &device, const Partition &unallocated, const Partition &newPartition)
    : Operation(OperationType::Create, device, unallocated, newPartition)
{
}

void OperationCreate::createDescription()
{
    QString kind;
    switch (m_new.type) {
    case PartitionType::Primary:  kind = tr("Primary Partition"); break;
    case PartitionType::Logical:  kind = tr("Logical Partition"); break;
    case PartitionType::Extended: kind = tr("Extended Partition"); break;
    default: break;
    }
    m_description = tr("Create %1 #%2 (%3, %4) on %5")
                        .arg(kind)
                        .arg(m_new.number)
                        .arg(fsName(m_new.fsType), formatSize(m_new.length(), m_new.sectorSize), m_device.path);
}

void OperationCreate::applyToVisual(Device &device)
{
    insertNew(device.partitions);
}

bool OperationCreate::mergeWith(const Operation &candidate)
{
    // Later changes to the not yet created partition are folded into its creation.
    if (candidate.original().status != PartitionStatus::New || !(m_new == candidate.original()))
        return false;
    switch (candidate.type()) {
    case OperationType::Format:
    case OperationType::SetFlags:
    case OperationType::Name:
    case OperationType::ResizeMove:
        m_new = candidate.partitionNew();
        if (m_new.type == PartitionType::Extended) {
            // Logical partitions are separate operations: only the space is ours.
            m_new.logicals.clear();
            m_new.logicals.push_back(Partition::makeUnallocated(m_new.devicePath, m_new.start, m_new.end,
                                                                m_new.sectorSize, true));
        }
        createDescription();
        return true;
    default:
        return false;
    }
}

bool OperationCreate::execute(OperationDetail &detail)
{
    Partition &np = m_new;
    Sector minSize = 0;
    if (np.type != PartitionType::Extended)
        if (const FileSystem *fs = FileSystemRegistry::instance().get(np.fsType))
            minSize = fs->limits().minSize / np.sectorSize;

    bool ok = Parted::createPartition(np, minSize, detail);
    if (ok && !np.name.isEmpty())
        ok = Parted::setName(np, detail);
    if (!ok)
        return false;

    if (np.type == PartitionType::Extended)
        ok = Parted::setPartitionType(np, detail);
    else if (np.fsType == FsType::Cleared)
        ok = Parted::eraseSignatures(np, detail);
    else if (np.fsType != FsType::Unformatted)
        ok = Parted::eraseSignatures(np, detail) && Parted::setPartitionType(np, detail)
          && createFileSystem(np, detail);

    // Flags chosen for the new partition beyond the ones its type already implies.
    if (ok && !np.flags.isEmpty()) {
        const QStringList onDisk = Parted::readFlags(np);
        bool missing = false;
        for (const QString &f : np.flags)
            missing = missing || !onDisk.contains(f);
        if (missing)
            ok = Parted::setFlags(np, onDisk, onDisk + np.flags, detail);
    }
    return ok;
}

// --- delete partition ------------------------------------------------------------------

OperationDelete::OperationDelete(const Device &device, const Partition &partition)
    : Operation(OperationType::Delete, device, partition, partition)
{
}

void OperationDelete::createDescription()
{
    const QString what = m_original.type == PartitionType::Logical ? tr("Logical Partition") : m_original.path;
    m_description = tr("Delete %1 (%2, %3) from %4")
                        .arg(what, fsName(m_original.fsType),
                             formatSize(m_original.length(), m_original.sectorSize), m_original.devicePath);
}

static void removeWithAdjacentUnallocated(PartitionVector &partitions, int index)
{
    if (index + 1 < int(partitions.size()) && partitions[size_t(index + 1)].type == PartitionType::Unallocated)
        partitions.erase(partitions.begin() + index + 1);
    if (index - 1 >= 0 && partitions[size_t(index - 1)].type == PartitionType::Unallocated)
        partitions.erase(partitions.begin() + --index);
    partitions.erase(partitions.begin() + index);
}

void OperationDelete::applyToVisual(Device &device)
{
    PartitionVector &partitions = device.partitions;
    if (m_original.insideExtended) {
        const int ext = findExtended(partitions);
        if (ext < 0)
            return;
        Partition &extended = partitions[size_t(ext)];
        const int index = findIndexOriginal(extended.logicals);
        if (index < 0)
            return;
        removeWithAdjacentUnallocated(extended.logicals, index);
        insertUnallocated(m_device.path, extended.logicals, extended.start, extended.end, m_device.sectorSize, true);
        // Logical partitions are always numbered consecutively from 5: renumber the following ones.
        if (m_original.status != PartitionStatus::New) {
            for (Partition &l : extended.logicals) {
                // Only existing partitions: new ones get their number when they are created.
                if (l.type == PartitionType::Logical && l.status == PartitionStatus::Real
                    && l.number > m_original.number) {
                    const int newNumber = l.number - 1;
                    if (l.status == PartitionStatus::Real && l.path.endsWith(QString::number(l.number)))
                        l.path = l.path.left(l.path.size() - QString::number(l.number).size())
                               + QString::number(newNumber);
                    l.number = newNumber;
                }
            }
        }
    } else {
        const int index = findIndexOriginal(partitions);
        if (index < 0)
            return;
        removeWithAdjacentUnallocated(partitions, index);
        insertUnallocated(m_device.path, partitions, 0, m_device.length - 1, m_device.sectorSize, false);
    }
}

bool OperationDelete::execute(OperationDetail &detail)
{
    return Parted::calibrate(m_original, detail) && Parted::verifyTarget(m_original, false, detail)
        && Parted::verifyFollowingLogicalsUnused(m_original, detail) && Parted::deletePartition(m_original, detail);
}

// --- format ------------------------------------------------------------------------------

OperationFormat::OperationFormat(const Device &device, const Partition &original, const Partition &newPartition)
    : Operation(OperationType::Format, device, original, newPartition)
{
}

void OperationFormat::createDescription()
{
    m_description = tr("Format %1 as %2").arg(m_original.path, fsName(m_new.filesystem().fsType));
}

void OperationFormat::applyToVisual(Device &device)
{
    if (m_original.type == PartitionType::Unpartitioned && m_new.fsType == FsType::Cleared) {
        // A cleared whole disk device shows as unallocated, as it will be after applying.
        device.partitions.clear();
        device.partitions.push_back(Partition::makeUnpartitioned(device.path, QString(), FsType::Unallocated,
                                                                 device.length, device.sectorSize, false));
        return;
    }
    substituteNew(device.partitions);
}

bool OperationFormat::mergeWith(const Operation &candidate)
{
    if (candidate.type() == OperationType::Format && m_new == candidate.original()) {
        m_new = candidate.partitionNew();
        createDescription();
        return true;
    }
    return false;
}

bool OperationFormat::execute(OperationDetail &detail)
{
    if (!Parted::calibrate(m_new, detail) || !verifyFileSystemTarget(m_new, detail, false))
        return false;
    m_original.path = m_new.path;
    if (m_new.isOpenLuks()) {
        // The file system inside the open encryption mapping (GParted: format within LUKS).
        const Partition &f = m_new.filesystem();
        if (f.fsType == FsType::Cleared)
            return Parted::eraseSignatures(f, detail);
        return Parted::eraseSignatures(f, detail) && createFileSystem(f, detail);
    }
    if (m_new.fsType == FsType::Cleared)
        return Parted::eraseSignatures(m_new, detail) && Parted::setPartitionType(m_new, detail);
    return Parted::eraseSignatures(m_new, detail) && Parted::setPartitionType(m_new, detail)
        && createFileSystem(m_new, detail);
}

// --- flags ---------------------------------------------------------------------------------

OperationSetFlags::OperationSetFlags(const Device &device, const Partition &original, const QStringList &flags)
    : Operation(OperationType::SetFlags, device, original, original)
{
    m_new.flags = flags;
}

void OperationSetFlags::createDescription()
{
    if (m_new.flags.isEmpty())
        m_description = tr("Clear all flags on %1").arg(m_original.path);
    else
        m_description = tr("Set flags on %1 to: %2").arg(m_original.path, m_new.flags.join(QStringLiteral(", ")));
}

void OperationSetFlags::applyToVisual(Device &device)
{
    // Only the flags change: keep everything else (e.g. the logicals of an extended
    // partition, which other operations may have changed) as it is now.
    if (Partition *p = findOriginal(device.partitions))
        p->flags = m_new.flags;
}

bool OperationSetFlags::mergeWith(const Operation &candidate)
{
    if (candidate.type() == OperationType::SetFlags && m_new == candidate.original()) {
        m_new.flags = candidate.partitionNew().flags;
        createDescription();
        return true;
    }
    return false;
}

bool OperationSetFlags::execute(OperationDetail &detail)
{
    return Parted::calibrate(m_new, detail) && Parted::setFlags(m_new, m_original.flags, m_new.flags, detail);
}

// --- name -----------------------------------------------------------------------------------

OperationName::OperationName(const Device &device, const Partition &original, const QString &name)
    : Operation(OperationType::Name, device, original, original)
{
    m_new.name = name;
}

void OperationName::createDescription()
{
    if (m_new.name.isEmpty())
        m_description = tr("Clear partition name on %1").arg(m_original.path);
    else
        m_description = tr("Set partition name \"%1\" on %2").arg(m_new.name, m_original.path);
}

void OperationName::applyToVisual(Device &device)
{
    if (Partition *p = findOriginal(device.partitions))
        p->name = m_new.name;
}

bool OperationName::mergeWith(const Operation &candidate)
{
    if (candidate.type() == OperationType::Name && m_new == candidate.original()) {
        m_new.name = candidate.partitionNew().name;
        createDescription();
        return true;
    }
    return false;
}

bool OperationName::execute(OperationDetail &detail)
{
    return Parted::calibrate(m_new, detail) && Parted::setName(m_new, detail);
}

// --- resize / move -------------------------------------------------------------------

OperationResizeMove::OperationResizeMove(const Device &device, const Partition &original, const Partition &newPartition)
    : Operation(OperationType::ResizeMove, device, original, newPartition)
{
}

void OperationResizeMove::createDescription()
{
    const QString path = m_original.path;
    const QString from = formatSize(m_original.length(), m_original.sectorSize);
    const QString to = formatSize(m_new.length(), m_new.sectorSize);
    const bool right = m_new.start > m_original.start;
    const bool left = m_new.start < m_original.start;
    if (m_new.length() > m_original.length())
        m_description = right ? tr("Move %1 to the right and grow it from %2 to %3").arg(path, from, to)
                      : left  ? tr("Move %1 to the left and grow it from %2 to %3").arg(path, from, to)
                              : tr("Grow %1 from %2 to %3").arg(path, from, to);
    else if (m_new.length() < m_original.length())
        m_description = right ? tr("Move %1 to the right and shrink it from %2 to %3").arg(path, from, to)
                      : left  ? tr("Move %1 to the left and shrink it from %2 to %3").arg(path, from, to)
                              : tr("Shrink %1 from %2 to %3").arg(path, from, to);
    else
        m_description = right ? tr("Move %1 to the right").arg(path)
                      : left  ? tr("Move %1 to the left").arg(path)
                              : tr("resize/move %1 (new and old partition have the same size and position.  "
                                   "Hence continuing anyway)").arg(path);
}

static void removeAdjacentUnallocated(PartitionVector &partitions, int index)
{
    if (index + 1 < int(partitions.size()) && partitions[size_t(index + 1)].type == PartitionType::Unallocated)
        partitions.erase(partitions.begin() + index + 1);
    if (index - 1 >= 0 && partitions[size_t(index - 1)].type == PartitionType::Unallocated)
        partitions.erase(partitions.begin() + (index - 1));
}

void OperationResizeMove::applyToVisual(Device &device)
{
    PartitionVector &partitions = device.partitions;
    if (m_original.type == PartitionType::Extended) {
        int ext = findExtended(partitions);
        if (ext < 0)
            return;
        removeAdjacentUnallocated(partitions, ext);
        ext = findExtended(partitions);
        Partition &x = partitions[size_t(ext)];
        x.start = m_new.start;
        x.end = m_new.end;
        insertUnallocated(m_device.path, partitions, 0, m_device.length - 1, m_device.sectorSize, false);
        ext = findExtended(partitions);
        PartitionVector &logicals = partitions[size_t(ext)].logicals;
        if (!logicals.empty() && logicals.front().type == PartitionType::Unallocated)
            logicals.erase(logicals.begin());
        if (!logicals.empty() && logicals.back().type == PartitionType::Unallocated)
            logicals.pop_back();
        insertUnallocated(m_device.path, logicals, partitions[size_t(ext)].start, partitions[size_t(ext)].end,
                          m_device.sectorSize, true);
        return;
    }
    if (m_original.insideExtended) {
        const int ext = findExtended(partitions);
        if (ext < 0)
            return;
        Partition &x = partitions[size_t(ext)];
        const int index = findIndexOriginal(x.logicals);
        if (index < 0)
            return;
        x.logicals[size_t(index)] = m_new;
        removeAdjacentUnallocated(x.logicals, index);
        insertUnallocated(m_device.path, x.logicals, x.start, x.end, m_device.sectorSize, true);
    } else {
        const int index = findIndexOriginal(partitions);
        if (index < 0)
            return;
        partitions[size_t(index)] = m_new;
        removeAdjacentUnallocated(partitions, index);
        insertUnallocated(m_device.path, partitions, 0, m_device.length - 1, m_device.sectorSize, false);
    }
}

bool OperationResizeMove::mergeWith(const Operation &candidate)
{
    if (candidate.type() == OperationType::ResizeMove && m_new == candidate.original()) {
        m_new = candidate.partitionNew();
        createDescription();
        return true;
    }
    return false;
}

bool OperationResizeMove::execute(OperationDetail &detail)
{
    if (!Parted::calibrate(m_original, detail))
        return false;
    // Online changes are expected to find the partition in use.
    if (!verifyFileSystemTarget(m_original, detail, true))
        return false;
    m_new.path = m_original.path;
    m_new.number = m_original.number;
    m_new.busy = m_original.busy;
    m_new.mountPoints = m_original.mountPoints;
    m_new.luks = m_original.luks;
    return Steps::resizeMove(m_original, m_new, detail);
}

// --- copy / paste ---------------------------------------------------------------------

OperationCopy::OperationCopy(const Device &device, const Partition &original, const Partition &newPartition,
                             const Partition &copied)
    : Operation(OperationType::Copy, device, original, newPartition)
    , m_copied(copied)
{
}

void OperationCopy::createDescription()
{
    if (m_original.type == PartitionType::Unallocated)
        m_description = tr("Copy %1 to %2 (start at %3)")
                            .arg(m_copied.path, m_device.path, formatSize(m_new.start, m_new.sectorSize));
    else
        m_description = tr("Copy %1 to %2").arg(m_copied.path, m_original.path);
}

void OperationCopy::applyToVisual(Device &device)
{
    if (m_original.type == PartitionType::Unallocated)
        insertNew(device.partitions);
    else
        substituteNew(device.partitions);
}

bool OperationCopy::execute(OperationDetail &detail)
{
    if (!Parted::calibrate(m_copied, detail) || !verifyFileSystemTarget(m_copied, detail, false))
        return false;
    if (m_original.type != PartitionType::Unallocated) {
        if (!Parted::calibrate(m_new, detail) || !Parted::verifyTarget(m_new, true, detail))
            return false;
        m_original.path = m_new.path;
    }
    // Never onto the source's own sectors (also when the source is inside encryption).
    if (m_copied.devicePath == m_new.devicePath && m_copied.start <= m_new.end && m_new.start <= m_copied.end) {
        detail.addChild(tr("the destination overlaps the source partition %1: nothing was changed").arg(m_copied.path),
                        OperationDetail::Status::Error, OperationDetail::Font::Italic);
        return false;
    }
    // The content copied is the file system, inside the encryption when open.
    return Steps::copy(m_copied.filesystem(), m_new, detail);
}

// --- check ----------------------------------------------------------------------------------

OperationCheck::OperationCheck(const Device &device, const Partition &partition)
    : Operation(OperationType::Check, device, partition, partition)
{
}

void OperationCheck::createDescription()
{
    m_description = tr("Check and repair file system (%1) on %2").arg(fsName(m_original.filesystem().fsType), m_original.path);
}

bool OperationCheck::mergeWith(const Operation &candidate)
{
    return candidate.type() == OperationType::Check && m_new == candidate.original();
}

bool OperationCheck::execute(OperationDetail &detail)
{
    return Parted::calibrate(m_new, detail) && verifyFileSystemTarget(m_new, detail, false)
        && Steps::checkRepair(m_new.filesystem(), detail) && Steps::checkRepairMaximize(m_new, detail);
}

// --- file system label --------------------------------------------------------------------

OperationLabel::OperationLabel(const Device &device, const Partition &original, const QString &label)
    : Operation(OperationType::Label, device, original, original)
{
    m_new.filesystem().fsLabel = label;
}

void OperationLabel::createDescription()
{
    if (m_new.filesystem().fsLabel.isEmpty())
        m_description = tr("Clear file system label on %1").arg(m_original.path);
    else
        m_description = tr("Set file system label \"%1\" on %2").arg(m_new.filesystem().fsLabel, m_original.path);
}

void OperationLabel::applyToVisual(Device &device)
{
    if (Partition *p = findOriginal(device.partitions))
        p->filesystem().fsLabel = m_new.filesystem().fsLabel;
}

bool OperationLabel::mergeWith(const Operation &candidate)
{
    if (candidate.type() == OperationType::Label && m_new == candidate.original()) {
        m_new.filesystem().fsLabel = candidate.partitionNew().filesystem().fsLabel;
        createDescription();
        return true;
    }
    return false;
}

bool OperationLabel::execute(OperationDetail &detail)
{
    return Parted::calibrate(m_new, detail) && verifyFileSystemTarget(m_new, detail, true)
        && Steps::labelFileSystem(m_new.filesystem(), detail);
}

// --- new UUID -------------------------------------------------------------------------------

OperationChangeUuid::OperationChangeUuid(const Device &device, const Partition &original)
    : Operation(OperationType::ChangeUuid, device, original, original)
{
    m_new.filesystem().uuid = tr("(new random UUID)");
}

void OperationChangeUuid::createDescription()
{
    m_description = tr("Set a new random UUID on %1 file system on %2").arg(fsName(m_original.filesystem().fsType), m_original.path);
}

void OperationChangeUuid::applyToVisual(Device &device)
{
    if (Partition *p = findOriginal(device.partitions))
        p->filesystem().uuid = m_new.filesystem().uuid;
}

bool OperationChangeUuid::mergeWith(const Operation &candidate)
{
    return candidate.type() == OperationType::ChangeUuid && m_new == candidate.original();
}

bool OperationChangeUuid::execute(OperationDetail &detail)
{
    return Parted::calibrate(m_new, detail) && verifyFileSystemTarget(m_new, detail, false)
        && Steps::changeUuid(m_new.filesystem(), detail);
}

} // namespace QtParted

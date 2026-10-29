/*
 * QtParted - a Qt frontend to libparted for manipulating disk partitions
 * Copyright (C) 2002-2026 Vanni Brutto <zanac4ever@gmail.com>
 *
 * New partition geometry and alignment rules are ported from GParted's
 * Dialog_Base_Partition / Dialog_Partition_New (https://gparted.org),
 * Copyright (C) 2004 Bart Hakvoort and the GParted contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "Layout.h"

#include "Parted.h"

#include <QCoreApplication>

#include <algorithm>
#include <cmath>

namespace QtParted {
namespace Layout {

int mibNeededForBootRecord(const Partition &p)
{
    return (p.insideExtended || p.start < MiB / p.sectorSize) ? 1 : 0;
}

static void snapToCylinder(const Device &device, Partition &p)
{
    const Sector cyl = device.cylinderSize;
    const bool lessThanHalf = p.end - p.start < cyl / 2;
    Sector diff;
    if (p.type == PartitionType::Logical || p.start == device.sectorsPerTrack)
        diff = (p.start - device.sectorsPerTrack) % cyl;  // EBR / MBR to next track offset
    else if (p.start == 34)
        diff = (p.start - 34) % cyl;                       // GPT data start
    else
        diff = p.start % cyl;
    if (diff && !p.strictStart) {
        if (diff < cyl / 2 || lessThanHalf)
            p.start -= diff;
        else
            p.start += cyl - diff;
    }
    diff = (p.end + 1) % cyl;
    if (diff) {
        if (diff < cyl / 2 && !lessThanHalf)
            p.end -= diff;
        else
            p.end += cyl - diff;
    }
}

static void snapToMebibyte(const Device &device, Partition &p)
{
    const Sector mib = MiB / p.sectorSize;
    if (p.start < 2 || p.type == PartitionType::Logical) {
        // MBR before the first partition, EBR before a logical one: the kernel
        // considers an EBR two sectors long.
        if (!p.strictStart && p.freeSpaceBefore == 0)
            p.start += 2;
    }

    Sector diff = p.start % mib;
    if (diff && (!p.strictStart || p.status == PartitionStatus::New || p.status == PartitionStatus::Copy)) {
        p.start += mib - diff;
        if (p.type == PartitionType::Extended) {
            const int ext = findExtended(device.partitions);
            if (ext >= 0)
                for (const Partition &l : device.partitions[size_t(ext)].logicals)
                    if (l.type == PartitionType::Logical && l.start - p.start < 2)
                        p.start -= mib;
        }
    }

    diff = (p.end + 1) % mib;
    if (diff)
        p.end -= diff;

    if (p.type == PartitionType::Logical) {
        const int ext = findExtended(device.partitions);
        if (ext >= 0) {
            for (const Partition &l : device.partitions[size_t(ext)].logicals)
                if (l.type == PartitionType::Logical && l.start > p.end && l.start - p.end < 2)
                    p.end -= mib;  // room for the following EBR
            if (p.end > device.partitions[size_t(ext)].end)
                p.end -= mib;
        }
    }

    if (p.type == PartitionType::Primary || p.type == PartitionType::Extended) {
        for (const Partition &o : device.partitions)
            if ((o.type == PartitionType::Primary || o.type == PartitionType::Extended)
                && p.status == PartitionStatus::Real && p.number != o.number && o.start > p.start
                && o.start <= p.end)
                p.end -= mib;
    }

    if (p.type == PartitionType::Extended)
        for (const Partition &l : p.logicals)
            if (l.type == PartitionType::Logical && l.end > p.end)
                p.end = l.end;

    // Backup GPT at the end of the disk.
    if (device.diskType == QLatin1String("gpt") && device.length - p.end < 34)
        p.end -= mib;
}

void snapToAlignment(const Device &device, Partition &p)
{
    if (p.alignment == Alignment::Cylinder)
        snapToCylinder(device, p);
    else if (p.alignment == Alignment::MiB)
        snapToMebibyte(device, p);
}

Partition makeNewPartition(const Device &device, const Partition &un, const NewPartitionRequest &req)
{
    const Sector mib = MiB / un.sectorSize;
    const Sector START = un.start;
    const Sector totalLength = un.length();

    Sector newStart = START + req.beforeMiB * mib;
    Sector newEnd = newStart + req.sizeMiB * mib - 1;
    if (newStart < un.start)
        newStart = un.start;
    if (newEnd > un.end)
        newEnd = un.end;
    // Grow a bit into free space smaller than 1 MiB.
    if (newStart - un.start < mib)
        newStart = un.start;
    if (un.end - newEnd < mib)
        newEnd = un.end;

    Partition p;
    p.devicePath = un.devicePath;
    p.path = QCoreApplication::translate("Layout", "New Partition #%1").arg(req.number);
    p.number = req.number;
    p.type = req.type;
    p.status = PartitionStatus::New;
    p.fsType = req.type == PartitionType::Extended ? FsType::Extended : req.fsType;
    p.start = newStart;
    p.end = newEnd;
    p.sectorSize = un.sectorSize;
    p.insideExtended = un.insideExtended;
    p.name = req.name.trimmed();
    if (const FileSystem *fs = FileSystemRegistry::instance().get(p.fsType))
        p.fsLabel = fs->sanitizeLabel(req.label.trimmed());
    else
        p.fsLabel = req.label.trimmed();
    p.alignment = req.alignment;

    if (p.alignment == Alignment::MiB) {
        // Keep the requested size when the start is not MiB aligned and there is room.
        const Sector diff = mib - (p.end + 1) % mib;
        if (diff && p.start % mib > 0 && p.end - START + 1 + diff < totalLength)
            p.end += diff;
    }

    Parted::composePartitionFlags(p, device.diskType);
    p.freeSpaceBefore = req.beforeMiB * mib;

    snapToAlignment(device, p);

    // Space for logical partitions, from the final (aligned) boundaries.
    if (p.type == PartitionType::Extended) {
        p.logicals.clear();
        p.logicals.push_back(Partition::makeUnallocated(p.devicePath, p.start, p.end, p.sectorSize, true));
    }
    return p;
}

FsLimits newPartitionLimits(FsType fsType, const Partition &un)
{
    FsLimits limits;
    if (const FileSystem *fs = FileSystemRegistry::instance().get(fsType))
        limits = fs->limits();
    const ByteValue total = un.byteLength();
    const ByteValue usable = total - mibNeededForBootRecord(un) * MiB;
    if (limits.minSize < MiB)
        limits.minSize = MiB;
    if (limits.minSize > total)
        limits.minSize = total;
    if (!limits.maxSize || limits.maxSize > usable)
        limits.maxSize = usable;
    return limits;
}

bool canCreate(FsType fsType, const Device &device, ByteValue spaceBytes)
{
    if (fsType == FsType::Unformatted)
        return true;
    if (device.readonly)
        return false;  // only unformatted partitions when the kernel can't be told (GParted)
    if (fsType == FsType::Cleared)
        return true;
    if (device.isImageFile)
        return false;  // file system tools need a device node
    const FileSystem *fs = FileSystemRegistry::instance().get(fsType);
    if (!fs || !fs->support().create)
        return false;
    return spaceBytes >= fs->limits().minSize;
}

// --- Resize/Move and Paste --------------------------------------------------------

static qint64 roundMiB(Sector sectors, ByteValue sectorSize)
{
    return qRound64(double(sectors) * double(sectorSize) / double(MiB));
}

static qint64 ceilMiB(ByteValue bytes)
{
    return qint64(std::ceil(double(bytes) / double(MiB)));
}

static void finishRanges(SpaceEdit &s, ByteValue minBytes, ByteValue maxBytes)
{
    s.minSizeMiB = std::max<qint64>(1, ceilMiB(minBytes));
    s.maxSizeMiB = std::max(s.minSizeMiB, ceilMiB(maxBytes));
    s.maxBeforeMiB = std::max(s.minBeforeMiB, s.totalMiB - s.minSizeMiB);
    s.maxAfterMiB = std::max<qint64>(0, s.totalMiB - s.minBeforeMiB - s.minSizeMiB);
}

SpaceEdit resizeMoveSpace(const Device &device, const Partition &p)
{
    SpaceEdit s;
    const Sector mib = MiB / p.sectorSize;
    const bool isNew = p.status == PartitionStatus::New || p.status == PartitionStatus::Copy;
    const PartitionVector *siblings = &device.partitions;
    if (p.type == PartitionType::Logical) {
        const int ext = findExtended(device.partitions);
        if (ext >= 0)
            siblings = &device.partitions[size_t(ext)].logicals;
    }
    size_t t = 0;
    while (t < siblings->size() && !((*siblings)[t] == p))
        ++t;
    if (t == siblings->size())
        return s;

    if (p.type == PartitionType::Extended) {
        // Port of GParted's Resize_Move_Extended.
        Sector previous = 0, next = 0;
        const Partition *prevUnalloc = nullptr;
        if (t > 0 && (*siblings)[t - 1].type == PartitionType::Unallocated) {
            previous = (*siblings)[t - 1].length();
            s.start = (*siblings)[t - 1].start;
            prevUnalloc = &(*siblings)[t - 1];
        } else {
            s.start = p.start;
        }
        if (t + 1 < siblings->size() && (*siblings)[t + 1].type == PartitionType::Unallocated)
            next = (*siblings)[t + 1].length();
        s.minBeforeMiB = prevUnalloc ? mibNeededForBootRecord(*prevUnalloc) : 0;
        s.totalSectors = previous + p.length() + next;
        s.totalMiB = roundMiB(s.totalSectors, p.sectorSize);

        // Used space: from (one MiB before) the first logical to the end of the last one.
        Sector first = 0, last = 0, used = 0;
        bool anyLogical = false;
        for (const Partition &l : p.logicals)
            anyLogical = anyLogical || l.type == PartitionType::Logical;
        if (anyLogical) {
            first = p.end;
            last = p.start;
            for (const Partition &l : p.logicals) {
                if (l.type != PartitionType::Logical)
                    continue;
                if (l.start < first)
                    first = std::max<Sector>(0, l.start - mib);
                if (l.end > last)
                    last = l.end;
            }
            used = last - first;
        }
        const ByteValue minBytes = used == 0 ? MiB : used * p.sectorSize;
        const ByteValue maxBytes = (s.totalMiB - s.minBeforeMiB) * MiB;
        finishRanges(s, minBytes, maxBytes);
        if (first != 0)
            s.maxBeforeMiB = roundMiB(first - s.start, p.sectorSize);
        s.beforeMiB = roundMiB(previous, p.sectorSize);
        s.sizeMiB = roundMiB(p.length(), p.sectorSize);
        s.afterMiB = roundMiB(next, p.sectorSize);
        if (first != 0)
            s.maxAfterMiB = roundMiB(s.totalSectors + s.start - first - used, p.sectorSize);
        s.usedMiB = roundMiB(used, p.sectorSize);
        s.usedStartMiB = first ? roundMiB(first - s.start, p.sectorSize) : 0;
        return s;
    }

    // Port of GParted's Resize_Move_Normal.
    const FsSupport fs = FileSystemRegistry::instance().support(p.fsType);
    FsLimits limits;
    if (const FileSystem *f = FileSystemRegistry::instance().get(p.fsType))
        limits = f->limits();
    bool canShrink, canGrow, canMove;
    if (isNew) {
        // Nothing written yet: any geometry is fine.
        canShrink = canGrow = canMove = true;
    } else {
        canShrink = (p.busy ? fs.onlineShrink : fs.shrink) != FsMethod::None
                 && (p.usageKnown() || p.fsType == FsType::LinuxSwap);
        canGrow = (p.busy ? fs.onlineGrow : fs.grow) != FsMethod::None;
        canMove = fs.move != FsMethod::None && !p.busy && p.type != PartitionType::Unpartitioned;
    }
    s.fixedStart = !canMove;

    Sector previous = 0, next = 0;
    const Partition *prevUnalloc = nullptr;
    if (t >= 1 && (*siblings)[t - 1].type == PartitionType::Unallocated && !s.fixedStart) {
        previous = (*siblings)[t - 1].length();
        s.start = (*siblings)[t - 1].start;
        prevUnalloc = &(*siblings)[t - 1];
    } else {
        s.start = p.start;
    }
    if (t + 1 < siblings->size() && (*siblings)[t + 1].type == PartitionType::Unallocated) {
        next = (*siblings)[t + 1].length();
        // A logical partition keeps 1 MiB after it for a following logical's EBR.
        if (p.type == PartitionType::Logical && next >= mib) {
            Sector maxEnd = 0;
            for (const Partition &x : *siblings)
                maxEnd = std::max(maxEnd, x.end);
            if (maxEnd - (*siblings)[t + 1].end > mib)
                next -= mib;
        }
    }
    s.minBeforeMiB = prevUnalloc ? mibNeededForBootRecord(*prevUnalloc) : 0;
    s.totalSectors = previous + p.length() + next;
    s.totalMiB = roundMiB(s.totalSectors, p.sectorSize);

    const Sector minResize = p.estimatedMinSize();
    ByteValue minBytes, maxBytes;
    if (canShrink) {
        minBytes = std::max(limits.minSize, isNew ? 0 : std::max<Sector>(0, minResize) * p.sectorSize);
        if (minBytes < MiB)
            minBytes = MiB;
    } else {
        minBytes = p.byteLength();
    }
    if (canGrow) {
        maxBytes = (s.totalMiB - s.minBeforeMiB) * MiB;
        if (limits.maxSize && maxBytes > limits.maxSize && !isNew)
            maxBytes = std::max(limits.maxSize, p.byteLength());
    } else {
        maxBytes = p.byteLength();
    }
    finishRanges(s, minBytes, maxBytes);
    s.beforeMiB = s.fixedStart ? 0 : roundMiB(previous, p.sectorSize);
    if (s.fixedStart)
        s.maxBeforeMiB = s.minBeforeMiB = 0;
    s.sizeMiB = roundMiB(p.length(), p.sectorSize);
    s.afterMiB = roundMiB(next, p.sectorSize);
    s.minAfterMiB = (!canGrow && !canMove) ? s.afterMiB : 0;
    s.usedMiB = minResize > 0 ? roundMiB(minResize, p.sectorSize) : 0;
    return s;
}

// Port of GParted's Dialog_Base_Partition::prepare_new_partition().
static void prepare(const Device &device, Partition &n, const SpaceEdit &s, qint64 before, qint64 size,
                    qint64 after, Alignment alignment)
{
    const Sector mib = MiB / n.sectorSize;
    const Sector oldSize = n.length();
    const bool beforeChanged = s.alwaysRecompute || before != s.beforeMiB;
    const bool afterChanged = s.alwaysRecompute || after != s.afterMiB;
    if (beforeChanged)
        n.start = s.start + before * mib;
    if (afterChanged)
        n.end = n.start + size * mib - 1;

    const Sector last = s.start + s.totalSectors - 1;
    if (n.start < s.start)
        n.start = s.start;
    if (n.end > last)
        n.end = last;
    // Grow a bit into free space smaller than 1 MiB.
    if (n.start - s.start < mib)
        n.start = s.start;
    if (last - n.end < mib)
        n.end = last;

    n.alignment = alignment;
    if (alignment == Alignment::MiB) {
        // Keep the requested size after snapping when start or end are not aligned.
        if ((n.start % mib > 0 || (n.end + 1) % mib > 0) && n.length() + mib < s.totalSectors)
            n.end += mib;
    }
    n.freeSpaceBefore = before * mib;
    n.strictStart = !beforeChanged;
    snapToAlignment(device, n);

    if (n.usageKnown()) {
        const Sector newSize = n.length();
        if (oldSize == newSize)
            n.setUsage(n.sectorsUsed + n.sectorsUnused, n.sectorsUnused);
        else
            n.setUsage(newSize, newSize - n.sectorsUsed);
    }
}

Partition applyResizeMove(const Device &device, const Partition &p, const SpaceEdit &s, qint64 before,
                          qint64 size, qint64 after, Alignment alignment)
{
    Partition n = p;
    prepare(device, n, s, before, size, after, alignment);
    return n;
}

SpaceEdit pasteSpace(const Partition &un, const Partition &copied)
{
    // Port of GParted's Dialog_Partition_Copy::set_data().
    SpaceEdit s;
    s.alwaysRecompute = true;
    s.start = un.start;
    s.totalSectors = un.length();
    s.totalMiB = roundMiB(un.length(), un.sectorSize);
    s.minBeforeMiB = mibNeededForBootRecord(un);
    const Sector copiedMinSectors = (copied.byteLength() + un.sectorSize - 1) / un.sectorSize;
    const qint64 copiedMiB = ceilMiB(copiedMinSectors * un.sectorSize);

    const FsSupport fs = FileSystemRegistry::instance().support(copied.fsType);
    FsLimits limits;
    if (const FileSystem *f = FileSystemRegistry::instance().get(copied.fsType))
        limits = f->limits();
    ByteValue maxBytes;
    if (fs.grow != FsMethod::None) {
        maxBytes = (s.totalMiB - s.minBeforeMiB) * MiB;
        if (limits.maxSize && limits.maxSize < maxBytes)
            maxBytes = limits.maxSize;
    } else {
        maxBytes = copied.byteLength();
    }
    finishRanges(s, copiedMiB * MiB, std::max(maxBytes, copiedMiB * MiB));
    s.beforeMiB = s.minBeforeMiB;
    s.sizeMiB = copiedMiB;
    s.afterMiB = std::max<qint64>(0, s.totalMiB - s.minBeforeMiB - copiedMiB);
    const Sector minResize = copied.estimatedMinSize();
    s.usedMiB = minResize > 0 ? roundMiB(minResize, copied.sectorSize) : 0;
    return s;
}

Partition pasteIntoExisting(const Partition &dst, const Partition &copied)
{
    // Port of GParted's Win_GParted::activate_paste() for an existing partition:
    // the destination keeps its geometry and table attributes, the file system
    // (including label and UUID: it is a block copy) comes from the source.
    Partition n = dst;
    n.fsType = copied.fsType;
    n.fsLabel = copied.fsLabel;
    n.uuid = copied.uuid;
    n.mountPoints.clear();
    n.messages.clear();
    n.busy = false;
    n.clearUsage();
    if (copied.usageKnown()) {
        // The copied file system is grown to fill the destination afterwards.
        const Sector used = (copied.sectorsUsed * copied.sectorSize + dst.sectorSize - 1) / dst.sectorSize;
        n.setUsage(dst.length(), dst.length() - used);
    }
    return n;
}

Partition makePastedPartition(const Device &device, const Partition &un, const Partition &copied,
                              const SpaceEdit &s, qint64 before, qint64 size, qint64 after, Alignment alignment,
                              int number)
{
    Partition n = copied;
    n.devicePath = un.devicePath;
    n.insideExtended = un.insideExtended;
    n.type = un.insideExtended ? PartitionType::Logical : PartitionType::Primary;
    n.sectorSize = un.sectorSize;
    n.busy = false;
    n.fsReadonly = false;
    n.mountPoints.clear();
    n.messages.clear();
    n.name.clear();
    n.availableFlags.clear();
    n.logicals.clear();
    n.number = number;
    n.path = QCoreApplication::translate("Layout", "Copy of %1").arg(copied.path);
    if (copied.usageKnown()) {
        const ByteValue fsBytes = (copied.sectorsUsed + copied.sectorsUnused) * copied.sectorSize;
        const ByteValue unusedBytes = copied.sectorsUnused * copied.sectorSize;
        n.setUsage((fsBytes + un.sectorSize - 1) / un.sectorSize, (unusedBytes + un.sectorSize - 1) / un.sectorSize);
    } else {
        n.clearUsage();
    }
    // `n` still has the source geometry: give it the free space's before preparing.
    n.start = un.start;
    n.end = un.end;
    prepare(device, n, s, before, size, after, alignment);
    n.status = PartitionStatus::Copy;
    // Only the flags the partition type implies are written for a new partition.
    n.flags.clear();
    Parted::composePartitionFlags(n, device.diskType);
    return n;
}

ByteValue pasteRequiredBytes(const Device &device, const Partition &un, const Partition &copied)
{
    ByteValue required = copied.byteLength();
    if (un.insideExtended || un.start < MiB / un.sectorSize)
        required += MiB;  // partition table or EBR
    if (((un.insideExtended && un.type == PartitionType::Unallocated) || un.type == PartitionType::Logical)
        && un.end < device.length - 2 * MiB / device.sectorSize)
        required += MiB;  // EBR of a following logical partition
    if (device.diskType == QLatin1String("gpt") && device.length - un.end < MiB / device.sectorSize)
        required += MiB;  // backup GPT
    return required;
}

} // namespace Layout
} // namespace QtParted

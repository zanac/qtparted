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

} // namespace Layout
} // namespace QtParted

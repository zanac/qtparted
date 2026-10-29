/*
 * QtParted - a Qt frontend to libparted for manipulating disk partitions
 * Copyright (C) 2002-2026 Vanni Brutto <zanac4ever@gmail.com>
 *
 * insertUnallocated() and the usage accounting are ported from GParted
 * (https://gparted.org), Copyright (C) 2004 Bart Hakvoort and the GParted contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "Partition.h"

#include <QCoreApplication>

#include <algorithm>

namespace QtParted {

void Partition::setUsage(Sector fsSize, Sector fsUnused)
{
    const Sector len = length();
    if (fsSize >= 0 && fsSize <= len && fsUnused >= 0 && fsUnused <= fsSize) {
        sectorsUsed = fsSize - fsUnused;
        sectorsUnused = fsUnused;
        sectorsUnallocated = len - fsSize;
    } else if (fsSize == -1 && fsUnused >= 0 && fsUnused <= len) {
        sectorsUsed = len - fsUnused;
        sectorsUnused = fsUnused;
        sectorsUnallocated = 0;
    } else {
        clearUsage();
    }
}

void Partition::clearUsage()
{
    sectorsUsed = -1;
    sectorsUnused = -1;
    sectorsUnallocated = 0;
    fsBlockSize = -1;
}

Sector Partition::significantUnallocatedThreshold() const
{
    constexpr double higher = 0.05;
    constexpr double lower = 0.02;
    const Sector len = length();
    const ByteValue bytes = len * sectorSize;
    Sector significant;
    if (bytes <= 0)
        significant = 1;
    else if (bytes <= 100 * MiB)
        significant = qRound64(len * higher);
    else if (bytes <= 1 * GiB) {
        const double fraction = (higher - lower) - double(bytes - 100 * MiB) * (higher - lower) / double(1 * GiB - 100 * MiB)
                              + lower;
        significant = qRound64(len * fraction);
    } else
        significant = qRound64(len * lower);
    return significant <= 1 ? 1 : significant;
}

Sector Partition::estimatedMinSize() const
{
    if (isOpenLuks()) {
        if (sectorsUsed < 0 || encrypted->sectorsUsed < 0)
            return -1;
        const Sector totalUsed = luksHeader + encrypted->sectorsUsed;
        const Sector totalUnallocated = sectorsUnallocated + encrypted->sectorsUnallocated;
        return totalUsed + std::min(totalUnallocated, significantUnallocatedThreshold());
    }
    if (sectorsUsed < 0)
        return -1;
    return sectorsUsed + std::min(sectorsUnallocated, significantUnallocatedThreshold());
}

bool Partition::usageKnown() const
{
    const bool own = sectorsUsed >= 0 && sectorsUnused >= 0;
    if (isOpenLuks())
        return own && encrypted->usageKnown();
    return own;
}

Sector Partition::usedSectors() const
{
    if (isOpenLuks()) {
        if (sectorsUsed < 0 || encrypted->sectorsUsed < 0)
            return -1;
        const Sector totalUsed = luksHeader + encrypted->sectorsUsed;
        const Sector totalUnallocated = sectorsUnallocated + encrypted->sectorsUnallocated;
        return totalUnallocated < significantUnallocatedThreshold() ? totalUsed + totalUnallocated : totalUsed;
    }
    if (sectorsUsed < 0)
        return -1;
    return sectorsUnallocated < significantUnallocatedThreshold() ? sectorsUsed + sectorsUnallocated : sectorsUsed;
}

Sector Partition::unusedSectors() const
{
    return isOpenLuks() ? encrypted->sectorsUnused : sectorsUnused;
}

Sector Partition::unallocatedSectors() const
{
    const Sector total = isOpenLuks() ? sectorsUnallocated + encrypted->sectorsUnallocated : sectorsUnallocated;
    return total < significantUnallocatedThreshold() ? 0 : total;
}

void Partition::luksFollowResize()
{
    if (fsType != FsType::Luks)
        return;
    setUsage(length(), 0);
    if (luks.isOpen())
        luks.length = (length() - luksHeader) * sectorSize;  // the mapping fills the partition
    if (encrypted) {
        const Sector mapping = length() - luksHeader;
        encrypted->end = encrypted->start + mapping - 1;
        if (encrypted->sectorsUsed >= 0)
            encrypted->setUsage(mapping, mapping - encrypted->sectorsUsed);
    }
}

void Partition::setOnlyFlag(const QString &flag)
{
    flags = QStringList{ flag };
}

QString Partition::displayPath() const
{
    if (fsType == FsType::Unallocated)
        return QCoreApplication::translate("Partition", "unallocated");
    return path;
}

Partition Partition::makeUnallocated(const QString &devicePath, Sector start, Sector end,
                                     ByteValue sectorSize, bool insideExtended)
{
    Partition p;
    p.devicePath = devicePath;
    p.path = fsName(FsType::Unallocated);
    p.number = -1;
    p.type = PartitionType::Unallocated;
    p.fsType = FsType::Unallocated;
    p.start = start;
    p.end = end;
    p.sectorSize = sectorSize;
    p.insideExtended = insideExtended;
    return p;
}

Partition Partition::makeUnpartitioned(const QString &devicePath, const QString &path, FsType fsType,
                                       Sector length, ByteValue sectorSize, bool busy)
{
    Partition p;
    p.devicePath = devicePath;
    p.path = path.isEmpty() ? fsName(FsType::Unallocated) : path;
    p.number = 1;
    p.type = PartitionType::Unpartitioned;
    p.fsType = fsType;
    p.start = 0;
    p.end = length - 1;
    p.sectorSize = sectorSize;
    p.busy = busy;
    return p;
}

int findExtended(const PartitionVector &partitions)
{
    for (size_t i = 0; i < partitions.size(); ++i)
        if (partitions[i].type == PartitionType::Extended)
            return int(i);
    return -1;
}

void insertUnallocated(const QString &devicePath, PartitionVector &partitions, Sector start, Sector end,
                       ByteValue sectorSize, bool insideExtended)
{
    const Sector mib = MiB / sectorSize;

    if (partitions.empty()) {
        partitions.push_back(Partition::makeUnallocated(devicePath, start, end, sectorSize, insideExtended));
        return;
    }

    // start <---> first partition start
    if (partitions.front().start - start > mib) {
        partitions.insert(partitions.begin(),
                          Partition::makeUnallocated(devicePath, start, partitions.front().start - 1, sectorSize,
                                                     insideExtended));
    }

    // gaps in between
    for (size_t t = 0; t + 1 < partitions.size(); ++t) {
        const Sector gap = partitions[t + 1].start - partitions[t].end - 1;
        // Only show exactly 1 MiB if the following partition is not logical (EBR space).
        if (gap > mib || (partitions[t + 1].type != PartitionType::Logical && gap == mib)) {
            Partition un = Partition::makeUnallocated(devicePath, partitions[t].end + 1,
                                                      partitions[t + 1].start - 1, sectorSize, insideExtended);
            partitions.insert(partitions.begin() + Sector(++t), un);
        }
    }

    // last partition end <---> end
    if (end - partitions.back().end >= mib) {
        partitions.push_back(
            Partition::makeUnallocated(devicePath, partitions.back().end + 1, end, sectorSize, insideExtended));
    }
}

} // namespace QtParted

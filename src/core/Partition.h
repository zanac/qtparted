/*
 * QtParted - a Qt frontend to libparted for manipulating disk partitions
 * Copyright (C) 2002-2026 Vanni Brutto <zanac4ever@gmail.com>
 *
 * The partition model follows GParted (https://gparted.org),
 * Copyright (C) 2004 Bart Hakvoort and the GParted contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#pragma once

#include "FsType.h"
#include "Units.h"

#include <QStringList>

#include <vector>

namespace QtParted {

enum class PartitionType {
    Primary,
    Logical,
    Extended,
    Unallocated,    // free space inside a partition table
    Unpartitioned,  // whole disk device without a partition table
};

enum class PartitionStatus {
    Real,       // exists on disk
    New,        // will be created by a pending operation
    Copy,       // will be created by a pending paste
    Formatted,  // will be reformatted by a pending operation
};

enum class Alignment {
    Cylinder,
    MiB,
    None,  // exact sectors
};

struct Partition;
using PartitionVector = std::vector<Partition>;

struct Partition {
    QString devicePath;
    QString path;           // /dev/sda1, or "New Partition #1" for pending ones
    int number = -1;
    PartitionType type = PartitionType::Unallocated;
    PartitionStatus status = PartitionStatus::Real;
    Alignment alignment = Alignment::MiB;
    FsType fsType = FsType::Unallocated;

    QString fsLabel;
    QString uuid;
    QString name;           // partition name (GPT and other tables supporting names)
    QStringList flags;      // libparted flag names currently set
    QStringList availableFlags;  // libparted flag names this partition supports

    Sector start = -1;
    Sector end = -1;
    ByteValue sectorSize = 512;
    bool insideExtended = false;
    bool busy = false;
    bool fsReadonly = false;

    QStringList mountPoints;
    QStringList messages;

    // File system usage; -1 when unknown.
    Sector sectorsUsed = -1;
    Sector sectorsUnused = -1;
    Sector sectorsUnallocated = 0;  // partition space not used by the file system
    ByteValue fsBlockSize = -1;

    // Used by the dialogs creating or moving partitions.
    Sector freeSpaceBefore = -1;
    bool strictStart = false;

    PartitionVector logicals;  // only for Extended

    // --- helpers -----------------------------------------------------------
    Sector length() const { return end - start + 1; }
    ByteValue byteLength() const { return length() * sectorSize; }
    Sector middleSector() const { return (start + end) / 2; }

    bool usageKnown() const { return sectorsUsed >= 0 && sectorsUnused >= 0; }
    // fsSize/fsUnused in sectors as reported by the file system tools (-1 = unknown size).
    void setUsage(Sector fsSize, Sector fsUnused);
    void clearUsage();

    bool isFlagSet(const QString &flag) const { return flags.contains(flag); }
    void setOnlyFlag(const QString &flag);

    bool isUnallocated() const { return type == PartitionType::Unallocated || fsType == FsType::Unallocated; }
    bool isPartitionInTable() const
    {
        return type == PartitionType::Primary || type == PartitionType::Logical || type == PartitionType::Extended;
    }

    QString displayPath() const;   // localized "unallocated" for free space
    QString mountPointsText() const { return mountPoints.join(QStringLiteral(", ")); }

    static Partition makeUnallocated(const QString &devicePath, Sector start, Sector end,
                                     ByteValue sectorSize, bool insideExtended);
    static Partition makeUnpartitioned(const QString &devicePath, const QString &path, FsType fsType,
                                       Sector length, ByteValue sectorSize, bool busy);

    // Same identity as GParted: device, number, start sector and type.
    bool operator==(const Partition &other) const
    {
        return devicePath == other.devicePath && number == other.number && start == other.start
            && type == other.type;
    }
    bool operator!=(const Partition &other) const { return !(*this == other); }
};

// Index of the extended partition in a vector, or -1.
int findExtended(const PartitionVector &partitions);

// Insert unallocated partitions for every gap larger than 1 MiB between [start, end].
void insertUnallocated(const QString &devicePath, PartitionVector &partitions, Sector start, Sector end,
                       ByteValue sectorSize, bool insideExtended);

} // namespace QtParted

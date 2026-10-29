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
#pragma once

#include "Device.h"
#include "FileSystem.h"

namespace QtParted {
namespace Layout {

// 1 when the first MiB of the unallocated space is needed for a partition table / EBR.
int mibNeededForBootRecord(const Partition &unallocated);

struct NewPartitionRequest {
    PartitionType type = PartitionType::Primary;
    FsType fsType = FsType::Ext4;
    qint64 beforeMiB = 0;   // free space preceding
    qint64 sizeMiB = 0;     // new size
    Alignment alignment = Alignment::MiB;
    QString label;
    QString name;
    int number = 1;         // "New Partition #number"
};

// Build the partition to create inside `unallocated`, aligned as requested.
Partition makeNewPartition(const Device &device, const Partition &unallocated, const NewPartitionRequest &request);

void snapToAlignment(const Device &device, Partition &partition);

// Size limits for a new file system in the given space (min at least 1 MiB).
FsLimits newPartitionLimits(FsType fsType, const Partition &unallocated);

// Which file system types a new partition / format may use on this device.
bool canCreate(FsType fsType, const Device &device, ByteValue spaceBytes);

} // namespace Layout
} // namespace QtParted

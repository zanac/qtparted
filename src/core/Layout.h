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

// --- Resize/Move and Paste dialogs ------------------------------------------------
// The editable space and the limits, all sizes in MiB like the dialogs show them.
struct SpaceEdit {
    Sector start = 0;           // first sector of the editable space
    Sector totalSectors = 0;
    qint64 totalMiB = 0;
    qint64 minBeforeMiB = 0;    // reserved for the partition table / EBR
    qint64 beforeMiB = 0, sizeMiB = 0, afterMiB = 0;  // current values
    qint64 maxBeforeMiB = 0;
    qint64 minSizeMiB = 1, maxSizeMiB = 1;
    qint64 minAfterMiB = 0, maxAfterMiB = 0;
    bool fixedStart = false;    // the start can't move (file system can't be moved)
    bool alwaysRecompute = false;  // paste: geometry always comes from the values
    qint64 usedMiB = 0;         // drawn as used space in the resizer
    qint64 usedStartMiB = 0;    // extended: where the logicals start
};

// Space around `partition` (a partition of `device`, as displayed) for a resize/move.
SpaceEdit resizeMoveSpace(const Device &device, const Partition &partition);
// The partition after a resize/move to the given values.
Partition applyResizeMove(const Device &device, const Partition &partition, const SpaceEdit &space,
                          qint64 beforeMiB, qint64 sizeMiB, qint64 afterMiB, Alignment alignment);

// Space for pasting `copied` into `unallocated`.
SpaceEdit pasteSpace(const Partition &unallocated, const Partition &copied);
// The new partition that will receive the copy.
Partition makePastedPartition(const Device &device, const Partition &unallocated, const Partition &copied,
                              const SpaceEdit &space, qint64 beforeMiB, qint64 sizeMiB, qint64 afterMiB,
                              Alignment alignment, int number);

// Bytes needed in `unallocated` to paste `copied` (partition table / EBR / backup GPT included).
ByteValue pasteRequiredBytes(const Device &device, const Partition &unallocated, const Partition &copied);

} // namespace Layout
} // namespace QtParted

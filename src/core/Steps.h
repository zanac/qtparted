/*
 * QtParted - a Qt frontend to libparted for manipulating disk partitions
 * Copyright (C) 2002-2026 Vanni Brutto <zanac4ever@gmail.com>
 *
 * The resize, move and copy sequences are ported from GParted_Core
 * (https://gparted.org), Copyright (C) 2004 Bart Hakvoort and the GParted contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#pragma once

#include "Partition.h"

namespace QtParted {

class OperationDetail;

// Building blocks of the operations that change file systems.  All run in the worker
// thread and record what they do in the detail tree.
namespace Steps {

bool checkRepair(const Partition &partition, OperationDetail &detail);
bool maximizeFileSystem(const Partition &partition, OperationDetail &detail);
bool recreateSwap(const Partition &partition, OperationDetail &detail);
// Check operation's final step: maximize the encryption and/or the file system.
bool checkRepairMaximize(const Partition &partition, OperationDetail &detail);
// LUKS: "cryptsetup resize" to fill the partition; to the size of `newPartition`.
bool maximizeEncryption(const Partition &partition, OperationDetail &detail);
bool shrinkEncryption(const Partition &oldPartition, const Partition &newPartition, OperationDetail &detail);
bool resizeEncryption(const Partition &oldPartition, Partition &newPartition, OperationDetail &detail);

// Move and/or resize `oldPartition` to the geometry of `newPartition` (updated with the
// final boundaries).
bool resizeMove(const Partition &oldPartition, Partition &newPartition, OperationDetail &detail);

// Copy the file system of `source` into `destination`, creating the destination
// partition first when its status is Copy.
bool copy(const Partition &source, Partition &destination, OperationDetail &detail);

bool labelFileSystem(const Partition &partition, OperationDetail &detail);
bool changeUuid(const Partition &partition, OperationDetail &detail);

} // namespace Steps
} // namespace QtParted

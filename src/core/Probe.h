/*
 * QtParted - a Qt frontend to libparted for manipulating disk partitions
 * Copyright (C) 2002-2026 Vanni Brutto <zanac4ever@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#pragma once

#include "FsType.h"
#include "Units.h"

#include <QString>

namespace QtParted {

struct ProbeResult {
    bool ok = false;          // probing ran
    bool ambivalent = false;  // more than one signature found
    QString type;             // blkid TYPE
    QString version;
    QString label;
    QString uuid;
    QString ptType;           // partition table type found (whole disk probing)
    FsType fsType = FsType::Unknown;
};

namespace Probe {
// Probe a block device or file.  When `withPartitionTables` is set libblkid also
// looks for partition tables, so a partitioned disk reports ptType instead of a
// false file system.
ProbeResult path(const QString &path, bool withPartitionTables = false);
// Probe a byte range of a device or image file (used for partitions of image files
// which have no device node).
ProbeResult range(const QString &devicePath, ByteValue offset, ByteValue size);
} // namespace Probe

} // namespace QtParted

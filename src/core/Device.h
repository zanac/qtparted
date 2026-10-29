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

#include "Partition.h"

namespace QtParted {

struct Device {
    QString path;
    QString model;
    QString serial;
    Sector length = 0;          // in sectors
    ByteValue sectorSize = 512;
    int heads = 0;
    int sectorsPerTrack = 0;
    qint64 cylinders = 0;
    Sector cylinderSize = 0;    // sectors, at least 1 MiB
    QString diskType;           // libparted label name ("gpt", "msdos", ...), "none" or localized "unrecognized"
    bool hasPartitionTable = false;
    int maxPrimaries = 1;
    int maxNameLength = 0;      // 0 => partition names not supported
    bool supportsExtended = false;
    bool readonly = false;      // partition table cannot be changed (busy and kernel not re-readable)
    bool isImageFile = false;   // a regular file opened as a disk image
    int highestBusy = 0;        // highest partition number in use
    PartitionVector partitions;
    QStringList messages;

    ByteValue byteLength() const { return length * sectorSize; }
    bool partitionNamingSupported() const { return maxNameLength > 0; }

    Device withoutPartitions() const
    {
        Device d = *this;
        d.partitions.clear();
        return d;
    }

    bool operator==(const Device &other) const { return path == other.path; }
    bool operator!=(const Device &other) const { return path != other.path; }
};

} // namespace QtParted

/*
 * QtParted - a Qt frontend to libparted for manipulating disk partitions
 * Copyright (C) 2002-2026 Vanni Brutto <zanac4ever@gmail.com>
 *
 * Mount, unmount, swapon/swapoff and Volume Group (de)activation are ported from
 * GParted's Win_GParted (https://gparted.org),
 * Copyright (C) 2004 Bart Hakvoort and the GParted contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#pragma once

#include "Partition.h"

namespace QtParted {

// Actions applied immediately, not queued: they change whether a file system is in use.
// `partition` is the one holding the file system (Partition::filesystem()).  On failure
// `error` holds the commands that failed and their output.
namespace BusyActions {

bool mount(const Partition &partition, const QString &mountPoint, QString &error);
bool unmount(const Partition &partition, QString &error);
bool swapOn(const Partition &partition, QString &error);
bool swapOff(const Partition &partition, QString &error);
// The Volume Group name is the PV's displayed mount point.
bool setVolumeGroupActive(const Partition &partition, bool active, QString &error);

// Kernel file system type for "mount -t", empty when unknown.
QString kernelFsName(FsType type);

} // namespace BusyActions
} // namespace QtParted

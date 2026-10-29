/*
 * QtParted - a Qt frontend to libparted for manipulating disk partitions
 * Copyright (C) 2002-2026 Vanni Brutto <zanac4ever@gmail.com>
 *
 * Device scanning and the partition table changes are ported from GParted_Core
 * (https://gparted.org), Copyright (C) 2004 Bart Hakvoort and the GParted contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#pragma once

#include "Device.h"

#include <QStringList>

#include <functional>
#include <optional>

namespace QtParted {

class OperationDetail;
class SystemInfo;

namespace Parted {

// libparted exception types and options are passed through as plain ints
// (PedExceptionType / PedExceptionOption bit masks).
using Prompter = std::function<int(int type, const QString &message, int options)>;

void initialize();
void setPrompter(Prompter prompter);
QString version();

// Names of the exception options for a mask, in libparted order.
QList<QPair<int, QString>> exceptionOptions(int options);
QString exceptionTypeName(int type);

QStringList diskLabelTypes();  // alphabetic, "loop" last, as libparted orders them

struct DiskTypeInfo {
    int maxPrimaries = 4;
    int maxNameLength = 0;
    bool supportsExtended = false;
};
DiskTypeInfo diskTypeInfo(const QString &devicePath, const QString &diskType);

QStringList probeDevicePaths();
std::optional<Device> readDevice(const QString &path, const SystemInfo &sys);

// --- changes, used while applying operations --------------------------------
bool createDiskLabel(const Device &device, const QString &diskType, OperationDetail &detail);
bool calibrate(Partition &partition, OperationDetail &detail);
bool createPartition(Partition &partition, Sector minSize, OperationDetail &detail);
bool deletePartition(const Partition &partition, OperationDetail &detail);
bool setPartitionType(const Partition &partition, OperationDetail &detail);
// Change only the flags that differ between `from` and `to` (clears first, then sets, so
// aliased flags such as boot/esp on GPT end up as requested).
bool setFlags(const Partition &partition, const QStringList &from, const QStringList &to, OperationDetail &detail);
QStringList readFlags(const Partition &partition);
bool setName(const Partition &partition, OperationDetail &detail);
bool eraseSignatures(const Partition &partition, OperationDetail &detail);

// GParted's compose_partition_flags(): flags implied by file system / table type.
void composePartitionFlags(Partition &partition, const QString &diskType);

// Wait for udev / devtmpfs to (re)create device nodes after a table change.
void settle();

} // namespace Parted
} // namespace QtParted

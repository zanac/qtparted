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

#include <QString>
#include <QtGlobal>

namespace QtParted {

// All disk geometry is expressed in device sectors (64-bit, never floating point).
using Sector    = qint64;
using ByteValue = qint64;

inline constexpr ByteValue KiB = 1024LL;
inline constexpr ByteValue MiB = 1024LL * KiB;
inline constexpr ByteValue GiB = 1024LL * MiB;
inline constexpr ByteValue TiB = 1024LL * GiB;
inline constexpr ByteValue PiB = 1024LL * TiB;

inline constexpr ByteValue floorSize(ByteValue value, ByteValue rounding)
{
    return value / rounding * rounding;
}

inline constexpr ByteValue ceilSize(ByteValue value, ByteValue rounding)
{
    return (value + rounding - 1) / rounding * rounding;
}

// Human readable size using binary units, e.g. "1.50 GiB".
QString formatBytes(ByteValue bytes);
QString formatSize(Sector sectors, ByteValue sectorSize);

} // namespace QtParted

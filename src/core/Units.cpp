/*
 * QtParted - a Qt frontend to libparted for manipulating disk partitions
 * Copyright (C) 2002-2026 Vanni Brutto <zanac4ever@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "Units.h"

#include <QCoreApplication>
#include <QLocale>

namespace QtParted {

QString formatBytes(ByteValue bytes)
{
    struct Unit { ByteValue size; const char *name; };
    static const Unit units[] = {
        { PiB, QT_TRANSLATE_NOOP("Units", "PiB") },
        { TiB, QT_TRANSLATE_NOOP("Units", "TiB") },
        { GiB, QT_TRANSLATE_NOOP("Units", "GiB") },
        { MiB, QT_TRANSLATE_NOOP("Units", "MiB") },
        { KiB, QT_TRANSLATE_NOOP("Units", "KiB") },
    };
    const QLocale locale;
    for (const Unit &u : units) {
        if (bytes >= u.size || bytes <= -u.size)
            return locale.toString(double(bytes) / double(u.size), 'f', 2) + QLatin1Char(' ')
                 + QCoreApplication::translate("Units", u.name);
    }
    return locale.toString(bytes) + QLatin1Char(' ') + QCoreApplication::translate("Units", "B");
}

QString formatSize(Sector sectors, ByteValue sectorSize)
{
    return formatBytes(sectors * sectorSize);
}

} // namespace QtParted

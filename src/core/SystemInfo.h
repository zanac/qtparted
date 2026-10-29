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

#include <QHash>
#include <QSet>
#include <QStringList>

namespace QtParted {

// Snapshot of what the kernel says about block devices: mounts, active swap,
// devices held by device-mapper/RAID/LVM, plus /etc/fstab.
class SystemInfo
{
public:
    void load();

    QStringList mountPoints(const QString &devPath) const;
    bool isMounted(const QString &devPath) const;
    bool isMountedReadonly(const QString &devPath) const;
    bool isSwapActive(const QString &devPath) const;
    // Activity for which the device has holders in sysfs (dm-crypt, LVM, md RAID ...)
    static bool hasHolders(const QString &devPath);
    QStringList fstabMountPoints(const QString &devPath, const QString &uuid, const QString &label) const;

    // Whole disk devices worth showing, like GParted's /proc/partitions filter plus
    // loop devices that have a backing file attached.
    static QStringList wholeDiskPaths();
    static QString serialNumber(const QString &devPath);
    static bool isRegularFile(const QString &path);

    static quint64 devNumber(const QString &path);  // st_rdev of a block device, 0 otherwise

private:
    struct Mount { QString mountPoint; bool readonly = false; };
    QHash<quint64, QList<Mount>> m_mountsByDev;
    QHash<QString, QList<Mount>> m_mountsBySource;
    QSet<quint64> m_swapsByDev;
    QSet<QString> m_swapsByPath;
    struct FstabEntry { QString spec; QString mountPoint; };
    QList<FstabEntry> m_fstab;
};

} // namespace QtParted

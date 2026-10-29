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
#include "BusyActions.h"

#include "Command.h"
#include "SystemInfo.h"

#include <QCoreApplication>

namespace QtParted {
namespace BusyActions {

namespace {

QString tr(const char *s)
{
    return QCoreApplication::translate("BusyActions", s);
}

QString failed(const QString &program, const QStringList &args, const CommandResult &r)
{
    return QStringLiteral("# %1 %2\n%3").arg(program, args.join(QLatin1Char(' ')), (r.error + r.output).trimmed());
}

bool run(const QString &program, const QStringList &args, QString &error)
{
    const CommandResult r = runCommand(program, args);
    if (r.ok())
        return true;
    if (!error.isEmpty())
        error += QStringLiteral("\n\n");
    error += failed(program, args, r);
    return false;
}

} // namespace

QString kernelFsName(FsType type)
{
    switch (type) {
    case FsType::Bcachefs: return QStringLiteral("bcachefs");
    case FsType::Btrfs:    return QStringLiteral("btrfs");
    case FsType::Exfat:    return QStringLiteral("exfat");
    case FsType::Ext2:     return QStringLiteral("ext2");
    case FsType::Ext3:     return QStringLiteral("ext3");
    case FsType::Ext4:     return QStringLiteral("ext4");
    case FsType::F2fs:     return QStringLiteral("f2fs");
    case FsType::Fat16:
    case FsType::Fat32:    return QStringLiteral("vfat");
    case FsType::Hfs:      return QStringLiteral("hfs");
    case FsType::HfsPlus:  return QStringLiteral("hfsplus");
    case FsType::Jfs:      return QStringLiteral("jfs");
    case FsType::Minix:    return QStringLiteral("minix");
    case FsType::Nilfs2:   return QStringLiteral("nilfs2");
    case FsType::Ntfs:     return QStringLiteral("ntfs");
    case FsType::Reiser4:  return QStringLiteral("reiser4");
    case FsType::Reiserfs: return QStringLiteral("reiserfs");
    case FsType::Udf:      return QStringLiteral("udf");
    case FsType::Xfs:      return QStringLiteral("xfs");
    default:               return QString();
    }
}

bool mount(const Partition &p, const QString &mountPoint, QString &error)
{
    // First let mount (libblkid) find the type, then name the one QtParted detected.
    error.clear();
    if (run(QStringLiteral("mount"), { QStringLiteral("-v"), p.path, mountPoint }, error))
        return true;
    const QString type = kernelFsName(p.fsType);
    if (type.isEmpty())
        return false;
    if (run(QStringLiteral("mount"), { QStringLiteral("-v"), QStringLiteral("-t"), type, p.path, mountPoint }, error)) {
        error.clear();
        return true;
    }
    return false;
}

bool unmount(const Partition &p, QString &error)
{
    error.clear();
    SystemInfo sys;
    sys.load();
    const QStringList all = sys.allMountPoints();
    QStringList skipped;
    bool ok = true;
    for (const QString &mp : sys.mountPoints(p.path)) {
        if (all.count(mp) >= 2) {
            // Other file systems are mounted on the same directory: unmounting would
            // need them all unmounted and remounted.  Too rare to automate (GParted).
            skipped.append(mp);
            continue;
        }
        // Unmounting below a duplicating bind mount removes all copies at once.
        SystemInfo now;
        now.load();
        if (!now.isMountedAt(p.path, mp))
            continue;
        ok = run(QStringLiteral("umount"), { QStringLiteral("-v"), mp }, error) && ok;
    }
    if (!ok)
        return false;
    if (!skipped.isEmpty()) {
        error = tr("The partition could not be unmounted from the following mount points:") + QStringLiteral("\n\n")
              + skipped.join(QLatin1Char('\n')) + QStringLiteral("\n\n")
              + tr("This is because other partitions are also mounted on these mount points.  You are advised to "
                   "unmount them manually.");
        return false;
    }
    return true;
}

bool swapOn(const Partition &p, QString &error)
{
    error.clear();
    return run(QStringLiteral("swapon"), { QStringLiteral("-v"), p.path }, error);
}

bool swapOff(const Partition &p, QString &error)
{
    error.clear();
    return run(QStringLiteral("swapoff"), { QStringLiteral("-v"), p.path }, error);
}

bool setVolumeGroupActive(const Partition &p, bool active, QString &error)
{
    error.clear();
    if (p.mountPoints.isEmpty()) {
        error = tr("%1 is not a member of a Volume Group").arg(p.path);
        return false;
    }
    return run(QStringLiteral("lvm"),
               { QStringLiteral("vgchange"), QStringLiteral("-a"), active ? QStringLiteral("y") : QStringLiteral("n"),
                 p.mountPoints.front() },
               error);
}

} // namespace BusyActions
} // namespace QtParted

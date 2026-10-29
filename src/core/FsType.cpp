/*
 * QtParted - a Qt frontend to libparted for manipulating disk partitions
 * Copyright (C) 2002-2026 Vanni Brutto <zanac4ever@gmail.com>
 *
 * File system naming and blkid mapping follow GParted (https://gparted.org),
 * Copyright (C) 2004 Bart Hakvoort and the GParted contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "FsType.h"

#include <QCoreApplication>

namespace QtParted {

QString fsName(FsType type)
{
    switch (type) {
    case FsType::Unknown:        return QCoreApplication::translate("FsType", "unknown");
    case FsType::Unallocated:    return QCoreApplication::translate("FsType", "unallocated");
    case FsType::Unformatted:    return QCoreApplication::translate("FsType", "unformatted");
    case FsType::Cleared:        return QCoreApplication::translate("FsType", "cleared");
    case FsType::Extended:       return QStringLiteral("extended");
    case FsType::Bcachefs:       return QStringLiteral("bcachefs");
    case FsType::Btrfs:          return QStringLiteral("btrfs");
    case FsType::Exfat:          return QStringLiteral("exfat");
    case FsType::Ext2:           return QStringLiteral("ext2");
    case FsType::Ext3:           return QStringLiteral("ext3");
    case FsType::Ext4:           return QStringLiteral("ext4");
    case FsType::F2fs:           return QStringLiteral("f2fs");
    case FsType::Fat16:          return QStringLiteral("fat16");
    case FsType::Fat32:          return QStringLiteral("fat32");
    case FsType::Hfs:            return QStringLiteral("hfs");
    case FsType::HfsPlus:        return QStringLiteral("hfs+");
    case FsType::Jfs:            return QStringLiteral("jfs");
    case FsType::LinuxSwap:      return QStringLiteral("linux-swap");
    case FsType::Luks:           return QStringLiteral("luks");
    case FsType::Lvm2Pv:         return QStringLiteral("lvm2 pv");
    case FsType::Minix:          return QStringLiteral("minix");
    case FsType::Nilfs2:         return QStringLiteral("nilfs2");
    case FsType::Ntfs:           return QStringLiteral("ntfs");
    case FsType::Reiser4:        return QStringLiteral("reiser4");
    case FsType::Reiserfs:       return QStringLiteral("reiserfs");
    case FsType::Udf:            return QStringLiteral("udf");
    case FsType::Xfs:            return QStringLiteral("xfs");
    case FsType::Apfs:           return QStringLiteral("apfs");
    case FsType::Ataraid:        return QStringLiteral("ataraid");
    case FsType::Bcache:         return QStringLiteral("bcache");
    case FsType::Bitlocker:      return QStringLiteral("bitlocker");
    case FsType::Grub2CoreImg:   return QStringLiteral("grub2 core.img");
    case FsType::Iso9660:        return QStringLiteral("iso9660");
    case FsType::Jbd:            return QStringLiteral("jbd");
    case FsType::LinuxSwraid:    return QStringLiteral("linux-raid");
    case FsType::LinuxSwsuspend: return QStringLiteral("linux-suspend");
    case FsType::Refs:           return QStringLiteral("refs");
    case FsType::Ufs:            return QStringLiteral("ufs");
    case FsType::Zfs:            return QStringLiteral("zfs");
    }
    return QStringLiteral("?");
}

FsType fsTypeFromBlkid(const QString &t, const QString &version)
{
    if (t.isEmpty())
        return FsType::Unknown;
    if (t == QLatin1String("vfat")) {
        // FAT12 is presented as fat16 like GParted does.
        return version.compare(QLatin1String("FAT32"), Qt::CaseInsensitive) == 0 ? FsType::Fat32
                                                                                  : FsType::Fat16;
    }
    struct Map { const char *name; FsType type; };
    static const Map map[] = {
        { "bcachefs", FsType::Bcachefs },
        { "btrfs", FsType::Btrfs },
        { "exfat", FsType::Exfat },
        { "ext2", FsType::Ext2 },
        { "ext3", FsType::Ext3 },
        { "ext4", FsType::Ext4 },
        { "ext4dev", FsType::Ext4 },
        { "f2fs", FsType::F2fs },
        { "hfs", FsType::Hfs },
        { "hfsplus", FsType::HfsPlus },
        { "hfsx", FsType::HfsPlus },
        { "jfs", FsType::Jfs },
        { "swap", FsType::LinuxSwap },
        { "crypto_LUKS", FsType::Luks },
        { "LVM2_member", FsType::Lvm2Pv },
        { "minix", FsType::Minix },
        { "nilfs2", FsType::Nilfs2 },
        { "ntfs", FsType::Ntfs },
        { "reiser4", FsType::Reiser4 },
        { "reiserfs", FsType::Reiserfs },
        { "udf", FsType::Udf },
        { "xfs", FsType::Xfs },
        { "apfs", FsType::Apfs },
        { "bcache", FsType::Bcache },
        { "BitLocker", FsType::Bitlocker },
        { "iso9660", FsType::Iso9660 },
        { "jbd", FsType::Jbd },
        { "linux_raid_member", FsType::LinuxSwraid },
        { "swsuspend", FsType::LinuxSwsuspend },
        { "swsusp", FsType::LinuxSwsuspend },
        { "ReFS", FsType::Refs },
        { "ufs", FsType::Ufs },
        { "zfs_member", FsType::Zfs },
    };
    for (const Map &m : map)
        if (t == QLatin1String(m.name))
            return m.type;
    if (t.endsWith(QLatin1String("_raid_member")))
        return FsType::Ataraid;
    return FsType::Unknown;
}

int fsLabelMaxLength(FsType type)
{
    switch (type) {
    case FsType::Bcachefs:  return 32;
    case FsType::Btrfs:     return 255;
    case FsType::Exfat:     return 11;
    case FsType::Ext2:
    case FsType::Ext3:
    case FsType::Ext4:      return 16;
    case FsType::F2fs:      return 127;
    case FsType::Fat16:
    case FsType::Fat32:     return 11;
    case FsType::Hfs:       return 27;
    case FsType::HfsPlus:   return 63;
    case FsType::Jfs:       return 11;
    case FsType::LinuxSwap: return 15;
    case FsType::Nilfs2:    return 80;
    case FsType::Ntfs:      return 128;
    case FsType::Reiser4:
    case FsType::Reiserfs:  return 16;
    case FsType::Udf:       return 126;
    case FsType::Xfs:       return 12;
    default:                return 30;
    }
}

bool fsIsReal(FsType type)
{
    switch (type) {
    case FsType::Unknown:
    case FsType::Unallocated:
    case FsType::Unformatted:
    case FsType::Cleared:
    case FsType::Extended:
        return false;
    default:
        return true;
    }
}

QList<FsType> creatableFsTypes()
{
    return { FsType::Bcachefs, FsType::Btrfs,  FsType::Exfat,    FsType::Ext2,     FsType::Ext3,
             FsType::Ext4,     FsType::F2fs,   FsType::Fat16,    FsType::Fat32,    FsType::Hfs,
             FsType::HfsPlus,  FsType::Jfs,    FsType::LinuxSwap, FsType::Lvm2Pv,  FsType::Minix,
             FsType::Nilfs2,   FsType::Ntfs,   FsType::Reiser4,  FsType::Reiserfs, FsType::Udf,
             FsType::Xfs };
}

} // namespace QtParted

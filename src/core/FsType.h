/*
 * QtParted - a Qt frontend to libparted for manipulating disk partitions
 * Copyright (C) 2002-2026 Vanni Brutto <zanac4ever@gmail.com>
 *
 * File system type list and naming follow GParted (https://gparted.org),
 * Copyright (C) 2004 Bart Hakvoort and the GParted contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#pragma once

#include <QList>
#include <QString>

namespace QtParted {

enum class FsType {
    // Pseudo types
    Unknown,
    Unallocated,
    Unformatted,   // New partition without a file system (existing content left untouched)
    Cleared,       // New partition / format with old signatures erased
    Extended,

    // File systems QtParted can manage
    Bcachefs,
    Btrfs,
    Exfat,
    Ext2,
    Ext3,
    Ext4,
    F2fs,
    Fat16,
    Fat32,
    Hfs,
    HfsPlus,
    Jfs,
    LinuxSwap,
    Luks,
    Lvm2Pv,
    Minix,
    Nilfs2,
    Ntfs,
    Reiser4,
    Reiserfs,
    Udf,
    Xfs,

    // Recognised only
    Apfs,
    Ataraid,
    Bcache,
    Bitlocker,
    Grub2CoreImg,
    Iso9660,
    Jbd,
    LinuxSwraid,
    LinuxSwsuspend,
    Refs,
    Ufs,
    Zfs,
};

// Short display / libparted compatible name, e.g. "ext4", "linux-swap", "fat32".
QString fsName(FsType type);

// Map a libblkid TYPE (and VERSION, used for FAT) to a file system type.
FsType fsTypeFromBlkid(const QString &type, const QString &version);

// Maximum length of a file system label (characters).
int fsLabelMaxLength(FsType type);

// Whether the type represents an actual file system / content (not a pseudo type).
bool fsIsReal(FsType type);

// The file systems QtParted knows how to create, in menu order.
QList<FsType> creatableFsTypes();

} // namespace QtParted

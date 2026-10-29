/*
 * QtParted - a Qt frontend to libparted for manipulating disk partitions
 * Copyright (C) 2002-2026 Vanni Brutto <zanac4ever@gmail.com>
 *
 * File system colours are GParted's (https://gparted.org).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "UiHelpers.h"

#include <QApplication>
#include <QCoreApplication>
#include <QPainter>
#include <QPalette>
#include <QPixmap>

namespace QtParted {
namespace Ui {

QColor fsColor(FsType type)
{
    switch (type) {
    case FsType::Unallocated:    return QColor(QRgb(0xA9A9A9));
    case FsType::Unknown:
    case FsType::Unformatted:
    case FsType::Cleared:        return QColor(QRgb(0x000000));
    case FsType::Extended:       return QColor(QRgb(0x95E3E5));
    case FsType::Bcachefs:       return QColor(QRgb(0xC26825));
    case FsType::Btrfs:          return QColor(QRgb(0xE58749));
    case FsType::Exfat:          return QColor(QRgb(0x267726));
    case FsType::Ext2:           return QColor(QRgb(0x7590AE));
    case FsType::Ext3:           return QColor(QRgb(0x4B6983));
    case FsType::Ext4:           return QColor(QRgb(0x314E6C));
    case FsType::F2fs:           return QColor(QRgb(0xDF421E));
    case FsType::Fat16:          return QColor(QRgb(0x60C160));
    case FsType::Fat32:          return QColor(QRgb(0x46A046));
    case FsType::Hfs:            return QColor(QRgb(0xD59FD4));
    case FsType::HfsPlus:        return QColor(QRgb(0xB173B0));
    case FsType::Jfs:            return QColor(QRgb(0xD6B129));
    case FsType::LinuxSwap:      return QColor(QRgb(0xC1665A));
    case FsType::Luks:           return QColor(QRgb(0x625B81));
    case FsType::Lvm2Pv:         return QColor(QRgb(0xB39169));
    case FsType::Minix:          return QColor(QRgb(0x9DB8D2));
    case FsType::Nilfs2:         return QColor(QRgb(0x990000));
    case FsType::Ntfs:           return QColor(QRgb(0x70D2B1));
    case FsType::Reiser4:        return QColor(QRgb(0x887FA3));
    case FsType::Reiserfs:       return QColor(QRgb(0xADA7C8));
    case FsType::Udf:            return QColor(QRgb(0x105210));
    case FsType::Xfs:            return QColor(QRgb(0xEED680));
    case FsType::Apfs:           return QColor(QRgb(0x874986));
    case FsType::Ataraid:        return QColor(QRgb(0x5A4733));
    case FsType::Bcache:         return QColor(QRgb(0xE0C39E));
    case FsType::Bitlocker:      return QColor(QRgb(0x494066));
    case FsType::Grub2CoreImg:   return QColor(QRgb(0x666666));
    case FsType::Iso9660:        return QColor(QRgb(0xD3D3D3));
    case FsType::Jbd:            return QColor(QRgb(0x314E6C));
    case FsType::LinuxSwraid:    return QColor(QRgb(0x826647));
    case FsType::LinuxSwsuspend: return QColor(QRgb(0x884631));
    case FsType::Refs:           return QColor(QRgb(0x3EA281));
    case FsType::Ufs:            return QColor(QRgb(0xAA8F2C));
    case FsType::Zfs:            return QColor(QRgb(0x984F18));
    }
    return QColor(Qt::black);
}

bool isDarkPalette(const QPalette &palette)
{
    return palette.color(QPalette::Window).lightness() < 128;
}

QColor usedColor(const QPalette &palette)
{
    return isDarkPalette(palette) ? QColor(0x8A, 0x86, 0x4E) : QColor(0xF8, 0xF8, 0xBA);
}

QColor unusedColor(const QPalette &palette)
{
    return palette.color(QPalette::Base);
}

QIcon fsIcon(FsType type, int size)
{
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const QColor c = fsColor(type);
    p.setPen(QPen(c.darker(140), 1));
    p.setBrush(c);
    p.drawRoundedRect(QRectF(1.5, 1.5, size - 3, size - 3), 2, 2);
    return QIcon(pm);
}

QIcon icon(const char *themeName, QStyle::StandardPixmap fallback)
{
    const QString name = QString::fromLatin1(themeName);
    if (QIcon::hasThemeIcon(name))
        return QIcon::fromTheme(name);
    return QApplication::style()->standardIcon(fallback);
}

QString partitionTypeText(PartitionType type)
{
    switch (type) {
    case PartitionType::Primary:       return QCoreApplication::translate("Ui", "Primary Partition");
    case PartitionType::Logical:       return QCoreApplication::translate("Ui", "Logical Partition");
    case PartitionType::Extended:      return QCoreApplication::translate("Ui", "Extended Partition");
    case PartitionType::Unallocated:   return QCoreApplication::translate("Ui", "Unallocated");
    case PartitionType::Unpartitioned: return QCoreApplication::translate("Ui", "Whole disk device");
    }
    return QString();
}

QString statusText(const Partition &p)
{
    if (p.status == PartitionStatus::New)
        return QCoreApplication::translate("Ui", "Not yet created");
    if (p.status == PartitionStatus::Formatted)
        return QCoreApplication::translate("Ui", "To be formatted");
    if (p.fsType == FsType::LinuxSwap)
        return p.busy ? QCoreApplication::translate("Ui", "Active") : QCoreApplication::translate("Ui", "Not active");
    if (p.busy)
        return p.mountPoints.isEmpty() ? QCoreApplication::translate("Ui", "Busy (in use)")
                                       : QCoreApplication::translate("Ui", "Mounted on %1").arg(p.mountPointsText());
    return QCoreApplication::translate("Ui", "Not mounted");
}

QString partitionKey(const Partition &p)
{
    return QStringLiteral("%1|%2|%3|%4").arg(p.path).arg(p.start).arg(int(p.type)).arg(p.number);
}

} // namespace Ui
} // namespace QtParted

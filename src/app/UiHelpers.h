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

#include <QColor>
#include <QIcon>
#include <QStyle>

class QPalette;

namespace QtParted {
namespace Ui {

// File system colours (GParted palette, readable on light and dark themes).
QColor fsColor(FsType type);
QColor usedColor(const QPalette &palette);
QColor unusedColor(const QPalette &palette);
bool isDarkPalette(const QPalette &palette);

// Small swatch used in lists and combo boxes.
QIcon fsIcon(FsType type, int size = 16);

// Theme icon (Breeze under KDE) with a Qt standard icon as fallback.
QIcon icon(const char *themeName, QStyle::StandardPixmap fallback);

QString partitionTypeText(PartitionType type);
QString statusText(const Partition &p);
// A stable identity for selecting the same partition after a refresh.
QString partitionKey(const Partition &p);

} // namespace Ui
} // namespace QtParted

/*
 * QtParted - a Qt frontend to libparted for manipulating disk partitions
 * Copyright (C) 2002-2026 Vanni Brutto <zanac4ever@gmail.com>
 *
 * Behaviour follows GParted's Dialog_Partition_Resize_Move and
 * Dialog_Partition_Copy (https://gparted.org).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#pragma once

#include "Layout.h"

#include <QDialog>

class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;

namespace QtParted {

class PartitionResizer;

// Resize/move an existing partition, or choose where to paste a copied one.
class DialogResizeMove : public QDialog
{
    Q_OBJECT
public:
    // Resize/move `partition` (as displayed on `device`).
    DialogResizeMove(const Device &device, const Partition &partition, QWidget *parent = nullptr);
    // Paste `copied` into `unallocated`; `number` names the new partition.
    DialogResizeMove(const Device &device, const Partition &unallocated, const Partition &copied, int number,
                     QWidget *parent = nullptr);

    // The partition after the change (resize/move) or the one to create (paste).
    Partition result() const;
    // Resize/move: whether the values differ from the current geometry.
    bool changed() const;

private:
    void build(const QString &title, const QString &okText, const char *okIcon, const Partition &shown);
    void setValues(qint64 before, qint64 size, QSpinBox *source);

    Device m_device;
    Partition m_partition;  // resize/move: the partition; paste: the unallocated space
    Partition m_copied;
    bool m_paste = false;
    int m_number = 0;
    Layout::SpaceEdit m_space;
    bool m_updating = false;

    PartitionResizer *m_resizer = nullptr;
    QSpinBox *m_before = nullptr;
    QSpinBox *m_size = nullptr;
    QSpinBox *m_after = nullptr;
    QComboBox *m_alignment = nullptr;
    QPushButton *m_ok = nullptr;
};

} // namespace QtParted

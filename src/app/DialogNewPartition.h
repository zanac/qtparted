/*
 * QtParted - a Qt frontend to libparted for manipulating disk partitions
 * Copyright (C) 2002-2026 Vanni Brutto <zanac4ever@gmail.com>
 *
 * Behaviour follows GParted's Dialog_Partition_New (https://gparted.org).
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
class QLineEdit;
class QSpinBox;
class QStandardItemModel;

namespace QtParted {

class PartitionResizer;

class DialogNewPartition : public QDialog
{
    Q_OBJECT
public:
    DialogNewPartition(const Device &device, const Partition &unallocated, bool anyExtended, int number,
                       QWidget *parent = nullptr);

    Layout::NewPartitionRequest request() const;

private:
    void onTypeChanged();
    void onFileSystemChanged();
    void updateLimits();
    void setValues(qint64 before, qint64 size, QSpinBox *source);
    FsType currentFsType() const;

    Device m_device;
    Partition m_unallocated;
    int m_number;
    qint64 m_totalMiB = 0;
    qint64 m_minBeforeMiB = 0;
    qint64 m_minSizeMiB = 1;
    qint64 m_maxSizeMiB = 1;
    bool m_updating = false;
    int m_extendedRow = -1;
    int m_defaultFsRow = 0;

    PartitionResizer *m_resizer;
    QSpinBox *m_before;
    QSpinBox *m_size;
    QSpinBox *m_after;
    QComboBox *m_alignment;
    QComboBox *m_type;
    QLineEdit *m_name;
    QComboBox *m_fs;
    QStandardItemModel *m_fsModel;
    QLineEdit *m_label;
    QLabel *m_minMax;
};

} // namespace QtParted

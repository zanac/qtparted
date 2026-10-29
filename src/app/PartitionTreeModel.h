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

#include "Device.h"

#include <QAbstractItemModel>

namespace QtParted {

// Partitions of the displayed device as a tree: logical partitions are children of
// the extended partition.
class PartitionTreeModel : public QAbstractItemModel
{
    Q_OBJECT
public:
    enum Column { ColPartition, ColName, ColFileSystem, ColMountPoint, ColLabel, ColSize, ColUsed, ColUnused,
                  ColFlags, ColumnCount };
    static constexpr int KeyRole = Qt::UserRole + 1;

    explicit PartitionTreeModel(QObject *parent = nullptr);

    void setDevice(const Device &device);
    const Partition *partition(const QModelIndex &index) const;
    QModelIndex indexForKey(const QString &key) const;

    QModelIndex index(int row, int column, const QModelIndex &parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

private:
    Device m_device;
};

} // namespace QtParted

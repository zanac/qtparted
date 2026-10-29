/*
 * QtParted - a Qt frontend to libparted for manipulating disk partitions
 * Copyright (C) 2002-2026 Vanni Brutto <zanac4ever@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "PartitionTreeModel.h"

#include "UiHelpers.h"

#include <QFont>

namespace QtParted {

// internalId: 0 = top level row; n > 0 = logical inside top level row n-1.

PartitionTreeModel::PartitionTreeModel(QObject *parent)
    : QAbstractItemModel(parent)
{
}

void PartitionTreeModel::setDevice(const Device &device)
{
    beginResetModel();
    m_device = device;
    endResetModel();
}

const Partition *PartitionTreeModel::partition(const QModelIndex &index) const
{
    if (!index.isValid())
        return nullptr;
    const quintptr id = index.internalId();
    if (id == 0) {
        if (index.row() < int(m_device.partitions.size()))
            return &m_device.partitions[size_t(index.row())];
        return nullptr;
    }
    const size_t top = size_t(id - 1);
    if (top >= m_device.partitions.size())
        return nullptr;
    const PartitionVector &logicals = m_device.partitions[top].logicals;
    return index.row() < int(logicals.size()) ? &logicals[size_t(index.row())] : nullptr;
}

QModelIndex PartitionTreeModel::indexForKey(const QString &key) const
{
    for (size_t i = 0; i < m_device.partitions.size(); ++i) {
        const Partition &p = m_device.partitions[i];
        if (Ui::partitionKey(p) == key)
            return createIndex(int(i), 0, quintptr(0));
        for (size_t j = 0; j < p.logicals.size(); ++j)
            if (Ui::partitionKey(p.logicals[j]) == key)
                return createIndex(int(j), 0, quintptr(i + 1));
    }
    return QModelIndex();
}

QModelIndex PartitionTreeModel::index(int row, int column, const QModelIndex &parent) const
{
    if (row < 0 || column < 0 || column >= ColumnCount)
        return QModelIndex();
    if (!parent.isValid())
        return row < int(m_device.partitions.size()) ? createIndex(row, column, quintptr(0)) : QModelIndex();
    if (parent.internalId() != 0)
        return QModelIndex();  // logicals have no children
    const Partition *p = partition(parent);
    if (!p || row >= int(p->logicals.size()))
        return QModelIndex();
    return createIndex(row, column, quintptr(parent.row() + 1));
}

QModelIndex PartitionTreeModel::parent(const QModelIndex &child) const
{
    if (!child.isValid() || child.internalId() == 0)
        return QModelIndex();
    return createIndex(int(child.internalId() - 1), 0, quintptr(0));
}

int PartitionTreeModel::rowCount(const QModelIndex &parent) const
{
    if (!parent.isValid())
        return int(m_device.partitions.size());
    if (parent.column() != 0 || parent.internalId() != 0)
        return 0;
    const Partition *p = partition(parent);
    return p ? int(p->logicals.size()) : 0;
}

int PartitionTreeModel::columnCount(const QModelIndex &) const
{
    return ColumnCount;
}

QVariant PartitionTreeModel::data(const QModelIndex &index, int role) const
{
    const Partition *p = partition(index);
    if (!p)
        return QVariant();

    if (role == KeyRole)
        return Ui::partitionKey(*p);

    const bool sizeKnown = p->usageKnown() && fsIsReal(p->fsType);
    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case ColPartition:  return p->displayPath();
        case ColName:       return p->name;
        case ColFileSystem: return p->fsType == FsType::Unallocated ? QString() : fsName(p->fsType);
        case ColMountPoint: return p->mountPointsText();
        case ColLabel:      return p->fsLabel;
        case ColSize:       return formatSize(p->length(), p->sectorSize);
        case ColUsed:       return sizeKnown ? formatSize(p->sectorsUsed, p->sectorSize) : QStringLiteral("---");
        case ColUnused:     return sizeKnown ? formatSize(p->sectorsUnused, p->sectorSize) : QStringLiteral("---");
        case ColFlags:      return p->flags.join(QStringLiteral(", "));
        }
    } else if (role == Qt::DecorationRole) {
        if (index.column() == ColFileSystem && p->fsType != FsType::Unallocated)
            return Ui::fsIcon(p->fsType);
        if (index.column() == ColPartition) {
            if (p->busy)
                return Ui::icon("object-locked", QStyle::SP_VistaShield);
            if (!p->messages.isEmpty())
                return Ui::icon("dialog-warning", QStyle::SP_MessageBoxWarning);
            if (p->status == PartitionStatus::New)
                return Ui::icon("list-add", QStyle::SP_FileIcon);
        }
    } else if (role == Qt::ToolTipRole) {
        if (index.column() == ColPartition) {
            QString tip = Ui::partitionTypeText(p->type) + QLatin1String(" — ") + Ui::statusText(*p);
            if (!p->messages.isEmpty())
                tip += QLatin1String("\n\n") + p->messages.join(QLatin1String("\n\n"));
            return tip;
        }
    } else if (role == Qt::TextAlignmentRole) {
        if (index.column() == ColSize || index.column() == ColUsed || index.column() == ColUnused)
            return QVariant::fromValue(Qt::AlignRight | Qt::AlignVCenter);
    } else if (role == Qt::FontRole) {
        if (index.column() == ColPartition && p->status != PartitionStatus::Real) {
            QFont f;
            f.setItalic(true);
            return f;
        }
    }
    return QVariant();
}

QVariant PartitionTreeModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return QVariant();
    switch (section) {
    case ColPartition:  return tr("Partition");
    case ColName:       return tr("Name");
    case ColFileSystem: return tr("File System");
    case ColMountPoint: return tr("Mount Point");
    case ColLabel:      return tr("Label");
    case ColSize:       return tr("Size");
    case ColUsed:       return tr("Used");
    case ColUnused:     return tr("Unused");
    case ColFlags:      return tr("Flags");
    }
    return QVariant();
}

} // namespace QtParted

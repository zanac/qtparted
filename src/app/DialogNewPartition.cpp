/*
 * QtParted - a Qt frontend to libparted for manipulating disk partitions
 * Copyright (C) 2002-2026 Vanni Brutto <zanac4ever@gmail.com>
 *
 * Behaviour follows GParted's Dialog_Partition_New (https://gparted.org),
 * Copyright (C) 2004 Bart Hakvoort and the GParted contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "DialogNewPartition.h"

#include "PartitionResizer.h"
#include "UiHelpers.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>

namespace QtParted {

static constexpr int FsTypeRole = Qt::UserRole + 1;

static bool labelAtCreate(FsType t)
{
    if (!fsIsReal(t))
        return false;
    return t != FsType::Lvm2Pv && t != FsType::Minix;
}

DialogNewPartition::DialogNewPartition(const Device &device, const Partition &unallocated, bool anyExtended,
                                       int number, QWidget *parent)
    : QDialog(parent)
    , m_device(device)
    , m_unallocated(unallocated)
    , m_number(number)
{
    setWindowTitle(tr("Create new Partition"));

    m_totalMiB = qRound64(double(unallocated.byteLength()) / double(MiB));
    m_minBeforeMiB = Layout::mibNeededForBootRecord(unallocated);

    m_resizer = new PartitionResizer(this);

    auto spin = [this]() {
        auto *s = new QSpinBox(this);
        s->setSuffix(tr(" MiB"));
        s->setRange(0, int(std::min<qint64>(m_totalMiB, std::numeric_limits<int>::max())));
        s->setAccelerated(true);
        s->setAlignment(Qt::AlignRight);
        return s;
    };
    m_before = spin();
    m_size = spin();
    m_after = spin();
    m_alignment = new QComboBox(this);
    m_alignment->addItem(tr("MiB"), int(Alignment::MiB));
    m_alignment->addItem(tr("Cylinder"), int(Alignment::Cylinder));
    m_alignment->addItem(tr("None"), int(Alignment::None));

    m_type = new QComboBox(this);
    m_type->addItem(tr("Primary Partition"), int(PartitionType::Primary));
    m_type->addItem(tr("Logical Partition"), int(PartitionType::Logical));
    m_type->addItem(tr("Extended Partition"), int(PartitionType::Extended));
    auto *typeModel = qobject_cast<QStandardItemModel *>(m_type->model());
    const bool msdosLike = device.supportsExtended;
    if (!msdosLike) {
        typeModel->item(1)->setEnabled(false);
        typeModel->item(2)->setEnabled(false);
        m_type->setCurrentIndex(0);
    } else if (unallocated.insideExtended) {
        typeModel->item(0)->setEnabled(false);
        typeModel->item(2)->setEnabled(false);
        m_type->setCurrentIndex(1);
    } else {
        typeModel->item(1)->setEnabled(false);
        if (anyExtended)
            typeModel->item(2)->setEnabled(false);
        m_type->setCurrentIndex(0);
    }

    m_name = new QLineEdit(this);
    m_name->setEnabled(device.partitionNamingSupported());
    if (device.partitionNamingSupported())
        m_name->setMaxLength(device.maxNameLength);
    else
        m_name->setPlaceholderText(tr("not supported by %1 tables").arg(device.diskType));

    m_fs = new QComboBox(this);
    m_fsModel = new QStandardItemModel(this);
    m_defaultFsRow = -1;
    auto addFs = [&](FsType t, bool enabled, const QString &tip) {
        auto *item = new QStandardItem(Ui::fsIcon(t), fsName(t));
        item->setData(int(t), FsTypeRole);
        item->setEnabled(enabled);
        if (!tip.isEmpty())
            item->setToolTip(tip);
        m_fsModel->appendRow(item);
        return m_fsModel->rowCount() - 1;
    };
    for (FsType t : creatableFsTypes()) {
        const bool ok = Layout::canCreate(t, device, unallocated.byteLength());
        QString tip;
        const FileSystem *fs = FileSystemRegistry::instance().get(t);
        if (!ok && fs) {
            if (device.isImageFile)
                tip = tr("File systems can only be created on block devices. Attach the image with "
                         "\"losetup -P\" and open the loop device.");
            else if (!fs->support().create)
                tip = tr("Requires %1 (%2)").arg(fs->mkfsProgram(), fs->software());
            else
                tip = tr("A %1 file system requires at least %2").arg(fsName(t), formatBytes(fs->limits().minSize));
        }
        const int row = addFs(t, ok, tip);
        if (ok && (t == FsType::Ext4 || (m_defaultFsRow < 0 && (t == FsType::Ext3 || t == FsType::Ext2))))
            m_defaultFsRow = row;
    }
    addFs(FsType::Cleared, true, tr("Erase old file system signatures, no new file system"));
    const int unformattedRow = addFs(FsType::Unformatted, true, tr("Create the partition only"));
    m_extendedRow = addFs(FsType::Extended, false, QString());
    if (m_defaultFsRow < 0) {
        for (int r = 0; r < m_fsModel->rowCount(); ++r)
            if (m_fsModel->item(r)->isEnabled()) {
                m_defaultFsRow = r;
                break;
            }
        if (m_defaultFsRow < 0)
            m_defaultFsRow = unformattedRow;
    }
    m_fs->setModel(m_fsModel);
    if (auto *view = qobject_cast<QListView *>(m_fs->view()))
        view->setRowHidden(m_extendedRow, true);
    m_fs->setCurrentIndex(m_defaultFsRow);

    m_label = new QLineEdit(this);
    m_minMax = new QLabel(this);
    m_minMax->setForegroundRole(QPalette::PlaceholderText);

    // --- layout ---------------------------------------------------------------
    auto *sizes = new QFormLayout;
    sizes->addRow(tr("Free space &preceding:"), m_before);
    sizes->addRow(tr("New si&ze:"), m_size);
    sizes->addRow(tr("Free space &following:"), m_after);
    sizes->addRow(tr("&Align to:"), m_alignment);

    auto *props = new QFormLayout;
    props->addRow(tr("Create &as:"), m_type);
    props->addRow(tr("Partition &name:"), m_name);
    props->addRow(tr("File &system:"), m_fs);
    props->addRow(tr("&Label:"), m_label);

    auto *grid = new QHBoxLayout;
    grid->addLayout(sizes, 1);
    grid->addSpacing(18);
    grid->addLayout(props, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("&Add"));
    buttons->button(QDialogButtonBox::Ok)->setIcon(Ui::icon("list-add", QStyle::SP_DialogApplyButton));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *top = new QVBoxLayout(this);
    auto *where = new QLabel(tr("Free space on %1: %2").arg(device.path, formatSize(unallocated.length(), unallocated.sectorSize)), this);
    top->addWidget(where);
    top->addWidget(m_resizer);
    top->addWidget(m_minMax);
    top->addSpacing(6);
    top->addLayout(grid);
    top->addStretch(1);
    top->addWidget(buttons);

    // --- behaviour ------------------------------------------------------------
    connect(m_type, &QComboBox::currentIndexChanged, this, &DialogNewPartition::onTypeChanged);
    connect(m_fs, &QComboBox::currentIndexChanged, this, &DialogNewPartition::onFileSystemChanged);
    connect(m_before, &QSpinBox::valueChanged, this, [this](int v) {
        setValues(v, m_size->value(), m_before);
    });
    connect(m_size, &QSpinBox::valueChanged, this, [this](int v) {
        setValues(m_before->value(), v, m_size);
    });
    connect(m_after, &QSpinBox::valueChanged, this, [this](int v) {
        setValues(m_before->value(), m_totalMiB - m_before->value() - v, m_after);
    });
    connect(m_resizer, &PartitionResizer::valuesChanged, this, [this](qint64 b, qint64 s) {
        setValues(b, s, nullptr);
    });

    m_resizer->setTotal(m_totalMiB);
    onTypeChanged();
    onFileSystemChanged();
    setValues(m_minBeforeMiB, m_maxSizeMiB, nullptr);
    m_resizer->setEnabled(m_totalMiB >= 2);

    m_size->setFocus();
    resize(sizeHint().expandedTo(QSize(640, 0)));
}

FsType DialogNewPartition::currentFsType() const
{
    return FsType(m_fs->currentData(FsTypeRole).toInt());
}

void DialogNewPartition::onTypeChanged()
{
    const bool extended = PartitionType(m_type->currentData().toInt()) == PartitionType::Extended;
    auto *view = qobject_cast<QListView *>(m_fs->view());
    if (extended) {
        if (view)
            view->setRowHidden(m_extendedRow, false);
        m_fs->setCurrentIndex(m_extendedRow);
        m_fs->setEnabled(false);
    } else if (m_fs->currentIndex() == m_extendedRow) {
        m_fs->setCurrentIndex(m_defaultFsRow);
        m_fs->setEnabled(true);
        if (view)
            view->setRowHidden(m_extendedRow, true);
    }
    m_resizer->setExtendedBackground(extended);
    onFileSystemChanged();
}

void DialogNewPartition::onFileSystemChanged()
{
    const FsType t = currentFsType();
    m_resizer->setPartitionColor(Ui::fsColor(t));
    const bool label = labelAtCreate(t);
    m_label->setEnabled(label);
    m_label->setMaxLength(fsLabelMaxLength(t));
    m_label->setPlaceholderText(label ? QString() : tr("not supported"));
    updateLimits();
}

void DialogNewPartition::updateLimits()
{
    const FsType t = currentFsType();
    const FsLimits limits = Layout::newPartitionLimits(t == FsType::Extended ? FsType::Unformatted : t, m_unallocated);
    m_minSizeMiB = qint64(std::ceil(double(limits.minSize) / double(MiB)));
    m_maxSizeMiB = qint64(std::ceil(double(limits.maxSize) / double(MiB)));
    if (m_maxSizeMiB > m_totalMiB - m_minBeforeMiB)
        m_maxSizeMiB = m_totalMiB - m_minBeforeMiB;
    if (m_minSizeMiB > m_maxSizeMiB)
        m_minSizeMiB = m_maxSizeMiB;

    m_updating = true;
    m_before->setRange(int(m_minBeforeMiB), int(m_totalMiB - m_minSizeMiB));
    m_size->setRange(int(m_minSizeMiB), int(m_maxSizeMiB));
    m_after->setRange(0, int(m_totalMiB - m_minBeforeMiB - m_minSizeMiB));
    m_updating = false;
    m_resizer->setLimits(m_minBeforeMiB, m_minSizeMiB, m_maxSizeMiB);
    m_minMax->setText(tr("Minimum size: %1 MiB        Maximum size: %2 MiB").arg(m_minSizeMiB).arg(m_maxSizeMiB));
    setValues(m_before->value(), m_size->value(), nullptr);
}

void DialogNewPartition::setValues(qint64 before, qint64 size, QSpinBox *source)
{
    if (m_updating)
        return;
    m_updating = true;
    size = std::clamp(size, m_minSizeMiB, m_maxSizeMiB);
    before = std::max(before, m_minBeforeMiB);
    if (before + size > m_totalMiB) {
        // Keep what the user is editing, adjust the other value.
        if (source == m_before)
            size = std::max(m_minSizeMiB, m_totalMiB - before);
        else
            before = std::max(m_minBeforeMiB, m_totalMiB - size);
    }
    if (before + size > m_totalMiB)
        size = m_totalMiB - before;
    const qint64 after = std::max<qint64>(0, m_totalMiB - before - size);

    if (source != m_before)
        m_before->setValue(int(before));
    if (source != m_size)
        m_size->setValue(int(size));
    if (source != m_after)
        m_after->setValue(int(after));
    m_resizer->setValues(before, size);
    m_updating = false;
}

Layout::NewPartitionRequest DialogNewPartition::request() const
{
    Layout::NewPartitionRequest r;
    r.type = PartitionType(m_type->currentData().toInt());
    r.fsType = currentFsType();
    r.beforeMiB = m_before->value();
    r.sizeMiB = m_size->value();
    r.alignment = Alignment(m_alignment->currentData().toInt());
    r.label = m_label->isEnabled() ? m_label->text() : QString();
    r.name = m_name->isEnabled() ? m_name->text() : QString();
    r.number = m_number;
    return r;
}

} // namespace QtParted

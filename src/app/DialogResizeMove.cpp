/*
 * QtParted - a Qt frontend to libparted for manipulating disk partitions
 * Copyright (C) 2002-2026 Vanni Brutto <zanac4ever@gmail.com>
 *
 * Behaviour follows GParted's Dialog_Partition_Resize_Move and
 * Dialog_Partition_Copy (https://gparted.org),
 * Copyright (C) 2004 Bart Hakvoort and the GParted contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "DialogResizeMove.h"

#include "PartitionResizer.h"
#include "UiHelpers.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>

namespace QtParted {

DialogResizeMove::DialogResizeMove(const Device &device, const Partition &partition, QWidget *parent)
    : QDialog(parent)
    , m_device(device)
    , m_partition(partition)
{
    m_space = Layout::resizeMoveSpace(device, partition);
    const bool canMove = !m_space.fixedStart;
    const QString title = canMove ? tr("Resize/Move %1").arg(partition.displayPath())
                                  : tr("Resize %1").arg(partition.displayPath());
    build(title, canMove ? tr("&Resize/Move") : tr("&Resize"), "transform-scale", partition);
}

DialogResizeMove::DialogResizeMove(const Device &device, const Partition &unallocated, const Partition &copied,
                                   int number, QWidget *parent)
    : QDialog(parent)
    , m_device(device)
    , m_partition(unallocated)
    , m_copied(copied)
    , m_paste(true)
    , m_number(number)
{
    m_space = Layout::pasteSpace(unallocated, copied);
    build(tr("Paste %1").arg(copied.displayPath()), tr("&Paste"), "edit-paste", copied);
}

void DialogResizeMove::build(const QString &title, const QString &okText, const char *okIcon, const Partition &shown)
{
    setWindowTitle(title);
    const Layout::SpaceEdit &s = m_space;

    m_resizer = new PartitionResizer(this);
    m_resizer->setTotal(s.totalMiB);
    m_resizer->setLimits(s.minBeforeMiB, s.minSizeMiB, s.maxSizeMiB);
    m_resizer->setMaxBefore(s.maxBeforeMiB);
    m_resizer->setEndRange(s.totalMiB - s.maxAfterMiB, s.totalMiB - s.minAfterMiB);
    m_resizer->setFixedStart(s.fixedStart);
    const bool extended = shown.type == PartitionType::Extended;
    m_resizer->setExtendedBackground(m_partition.insideExtended && !m_paste);
    m_resizer->setPartitionColor(Ui::fsColor(extended ? FsType::Extended : Ui::displayFsType(shown)));
    m_resizer->setUsed(s.usedMiB, extended ? s.usedStartMiB : -1);

    auto spin = [this](qint64 min, qint64 max) {
        auto *sp = new QSpinBox(this);
        sp->setSuffix(tr(" MiB"));
        const qint64 cap = std::numeric_limits<int>::max();
        sp->setRange(int(std::min(min, cap)), int(std::min(std::max(min, max), cap)));
        sp->setAccelerated(true);
        sp->setAlignment(Qt::AlignRight);
        return sp;
    };
    m_before = spin(s.minBeforeMiB, s.maxBeforeMiB);
    m_size = spin(s.minSizeMiB, s.maxSizeMiB);
    m_after = spin(s.minAfterMiB, s.maxAfterMiB);
    m_before->setEnabled(!s.fixedStart);
    m_alignment = new QComboBox(this);
    m_alignment->addItem(tr("MiB"), int(Alignment::MiB));
    m_alignment->addItem(tr("Cylinder"), int(Alignment::Cylinder));
    m_alignment->addItem(tr("None"), int(Alignment::None));

    auto *form = new QFormLayout;
    form->addRow(tr("Free space &preceding:"), m_before);
    form->addRow(tr("New si&ze:"), m_size);
    form->addRow(tr("Free space &following:"), m_after);
    form->addRow(tr("&Align to:"), m_alignment);

    auto *minMax = new QLabel(tr("Minimum size: %1 MiB        Maximum size: %2 MiB").arg(s.minSizeMiB).arg(s.maxSizeMiB), this);
    minMax->setForegroundRole(QPalette::PlaceholderText);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_ok = buttons->button(QDialogButtonBox::Ok);
    m_ok->setText(okText);
    m_ok->setIcon(Ui::icon(okIcon, QStyle::SP_DialogApplyButton));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *top = new QVBoxLayout(this);
    QString what = m_paste ? tr("Paste %1 (%2, %3) into free space on %4")
                                 .arg(shown.displayPath(), Ui::fsDisplayName(shown),
                                      formatSize(shown.length(), shown.sectorSize), m_device.path)
                           : tr("%1 (%2, %3)").arg(shown.displayPath(), extended ? tr("extended") : Ui::fsDisplayName(shown),
                                                   formatSize(shown.length(), shown.sectorSize));
    top->addWidget(new QLabel(what, this));
    if (!m_paste && shown.isOpenLuks())
        top->addWidget(new QLabel(tr("<i>The encryption is open: it is resized together with the file system inside, "
                                     "its start can't move.</i>"), this));
    else if (!m_paste && shown.busy)
        top->addWidget(new QLabel(tr("<i>The partition is in use: it can only be resized online, its start can't move.</i>"), this));
    else if (!m_paste && s.fixedStart && !extended)
        top->addWidget(new QLabel(tr("<i>The %1 file system can't be moved: only its end can change.</i>").arg(fsName(shown.fsType)), this));
    top->addWidget(m_resizer);
    top->addWidget(minMax);
    top->addSpacing(6);
    top->addLayout(form);
    top->addStretch(1);
    top->addWidget(buttons);

    connect(m_before, &QSpinBox::valueChanged, this, [this](int v) { setValues(v, m_size->value(), m_before); });
    connect(m_size, &QSpinBox::valueChanged, this, [this](int v) { setValues(m_before->value(), v, m_size); });
    connect(m_after, &QSpinBox::valueChanged, this, [this](int v) {
        // Changing the space after keeps the start: the size absorbs the change.
        setValues(m_before->value(), m_space.totalMiB - m_before->value() - v, m_after);
    });
    connect(m_resizer, &PartitionResizer::valuesChanged, this, [this](qint64 b, qint64 sz) { setValues(b, sz, nullptr); });
    connect(m_alignment, &QComboBox::currentIndexChanged, this, [this]() {
        if (!m_paste)
            m_ok->setEnabled(changed());
    });

    setValues(s.beforeMiB, s.sizeMiB, nullptr);
    if (!m_paste)
        m_ok->setEnabled(false);  // nothing changed yet
    m_size->setFocus();
    resize(sizeHint().expandedTo(QSize(640, 0)));
}

void DialogResizeMove::setValues(qint64 before, qint64 size, QSpinBox *source)
{
    if (m_updating)
        return;
    m_updating = true;
    const Layout::SpaceEdit &s = m_space;
    const qint64 maxEnd = s.totalMiB - s.minAfterMiB;
    const qint64 minEnd = s.totalMiB - s.maxAfterMiB;
    if (s.fixedStart)
        before = s.beforeMiB;
    if (source == m_before) {
        // Moving the start keeps the size where possible.
        before = std::clamp(before, s.minBeforeMiB, s.maxBeforeMiB);
        size = std::clamp(size, std::max(s.minSizeMiB, minEnd - before), std::min(s.maxSizeMiB, maxEnd - before));
    } else {
        size = std::clamp(size, s.minSizeMiB, s.maxSizeMiB);
        if (before + size > maxEnd)
            before = std::max(s.minBeforeMiB, maxEnd - size);
        if (before + size < minEnd)
            before = minEnd - size;
        before = std::clamp(before, s.minBeforeMiB, s.maxBeforeMiB);
        size = std::clamp(size, std::max(s.minSizeMiB, minEnd - before), std::min(s.maxSizeMiB, maxEnd - before));
    }
    const qint64 after = std::max<qint64>(0, s.totalMiB - before - size);
    m_before->setValue(int(before));
    m_size->setValue(int(size));
    m_after->setValue(int(after));
    m_resizer->setValues(before, size);
    m_updating = false;
    if (!m_paste)
        m_ok->setEnabled(changed());
}

bool DialogResizeMove::changed() const
{
    if (m_paste)
        return true;
    if (m_before->value() != m_space.beforeMiB || m_size->value() != m_space.sizeMiB
        || m_after->value() != m_space.afterMiB)
        return true;
    // Same values but a different alignment may still change the geometry.
    const Partition n = result();
    return n.start != m_partition.start || n.end != m_partition.end;
}

Partition DialogResizeMove::result() const
{
    const qint64 before = m_before->value();
    const qint64 size = m_size->value();
    const qint64 after = m_after->value();
    const Alignment alignment = Alignment(m_alignment->currentData().toInt());
    if (m_paste)
        return Layout::makePastedPartition(m_device, m_partition, m_copied, m_space, before, size, after, alignment,
                                           m_number);
    return Layout::applyResizeMove(m_device, m_partition, m_space, before, size, after, alignment);
}

} // namespace QtParted

/*
 * QtParted - a Qt frontend to libparted for manipulating disk partitions
 * Copyright (C) 2002-2026 Vanni Brutto <zanac4ever@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "DialogPartitionInfo.h"

#include "UiHelpers.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPainter>
#include <QPlainTextEdit>
#include <QVBoxLayout>

namespace QtParted {

namespace {

// Used / unused / unallocated bar like the one in the disk map.
class UsageBar : public QWidget
{
public:
    explicit UsageBar(const Partition &p, QWidget *parent)
        : QWidget(parent), m_p(p)
    {
        setMinimumHeight(40);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        const QRect r = rect().adjusted(0, 0, -1, -1);
        painter.fillRect(r, Ui::fsColor(m_p.fsType));
        const QRect inner = r.adjusted(4, 4, -4, -4);
        painter.fillRect(inner, Ui::unusedColor(palette()));
        if (m_p.usageKnown() && m_p.length() > 0) {
            const int usedW = int(double(inner.width()) * double(m_p.sectorsUsed) / double(m_p.length()));
            painter.fillRect(QRect(inner.left(), inner.top(), usedW, inner.height()), Ui::usedColor(palette()));
            const int unW = int(double(inner.width()) * double(m_p.sectorsUnallocated) / double(m_p.length()));
            if (unW > 0)
                painter.fillRect(QRect(inner.right() + 1 - unW, inner.top(), unW, inner.height()),
                                 QColor(0xA9, 0xA9, 0xA9, 110));
        }
    }

private:
    Partition m_p;
};

QLabel *value(const QString &text, QWidget *parent)
{
    auto *l = new QLabel(text.isEmpty() ? QStringLiteral("—") : text, parent);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    l->setWordWrap(true);
    return l;
}

QString percent(Sector part, Sector total)
{
    if (total <= 0)
        return QString();
    return QStringLiteral(" (%1%)").arg(QLocale().toString(100.0 * double(part) / double(total), 'f', 0));
}

} // namespace

DialogPartitionInfo::DialogPartitionInfo(const Device &device, const Partition &p, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Information about %1").arg(p.displayPath()));

    auto *top = new QVBoxLayout(this);

    if (fsIsReal(p.fsType) || p.fsType == FsType::Unknown) {
        top->addWidget(new UsageBar(p, this));
    }

    auto *fsBox = new QGroupBox(tr("File System"), this);
    auto *fsForm = new QFormLayout(fsBox);
    auto *fsRow = new QWidget(fsBox);
    auto *fsRowL = new QHBoxLayout(fsRow);
    fsRowL->setContentsMargins(0, 0, 0, 0);
    auto *sw = new QLabel(fsRow);
    sw->setPixmap(Ui::fsIcon(p.fsType).pixmap(16, 16));
    fsRowL->addWidget(sw);
    fsRowL->addWidget(value(fsName(p.fsType), fsRow), 1);
    fsForm->addRow(tr("File system:"), fsRow);
    fsForm->addRow(tr("Label:"), value(p.fsLabel, fsBox));
    fsForm->addRow(tr("UUID:"), value(p.uuid, fsBox));
    fsForm->addRow(tr("Status:"), value(Ui::statusText(p), fsBox));
    if (!p.busy && !p.mountPoints.isEmpty())
        fsForm->addRow(tr("Mount point (fstab):"), value(p.mountPointsText(), fsBox));
    fsForm->addRow(tr("Size:"), value(formatSize(p.length(), p.sectorSize), fsBox));
    if (p.usageKnown()) {
        fsForm->addRow(tr("Used:"), value(formatSize(p.sectorsUsed, p.sectorSize) + percent(p.sectorsUsed, p.length()), fsBox));
        fsForm->addRow(tr("Unused:"), value(formatSize(p.sectorsUnused, p.sectorSize) + percent(p.sectorsUnused, p.length()), fsBox));
        if (p.sectorsUnallocated > 0)
            fsForm->addRow(tr("Unallocated:"),
                           value(formatSize(p.sectorsUnallocated, p.sectorSize) + percent(p.sectorsUnallocated, p.length()), fsBox));
    }
    if (p.fsBlockSize > 0)
        fsForm->addRow(tr("Block size:"), value(formatBytes(p.fsBlockSize), fsBox));
    top->addWidget(fsBox);

    auto *ptBox = new QGroupBox(tr("Partition"), this);
    auto *ptForm = new QFormLayout(ptBox);
    ptForm->addRow(tr("Path:"), value(p.displayPath(), ptBox));
    if (device.partitionNamingSupported())
        ptForm->addRow(tr("Name:"), value(p.name, ptBox));
    ptForm->addRow(tr("Type:"), value(Ui::partitionTypeText(p.type), ptBox));
    ptForm->addRow(tr("Flags:"), value(p.flags.join(QStringLiteral(", ")), ptBox));
    ptForm->addRow(tr("First sector:"), value(QLocale().toString(p.start), ptBox));
    ptForm->addRow(tr("Last sector:"), value(QLocale().toString(p.end), ptBox));
    ptForm->addRow(tr("Total sectors:"), value(QLocale().toString(p.length()), ptBox));
    top->addWidget(ptBox);

    if (!p.messages.isEmpty()) {
        auto *msgBox = new QGroupBox(tr("Warnings"), this);
        auto *l = new QVBoxLayout(msgBox);
        auto *text = new QPlainTextEdit(p.messages.join(QStringLiteral("\n\n")), msgBox);
        text->setReadOnly(true);
        text->setMaximumHeight(140);
        l->addWidget(text);
        top->addWidget(msgBox);
    }

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    top->addWidget(buttons);
    resize(480, sizeHint().height());
}

} // namespace QtParted

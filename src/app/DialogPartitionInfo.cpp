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
        painter.fillRect(r, Ui::fsColor(Ui::displayFsType(m_p)));
        const QRect inner = r.adjusted(4, 4, -4, -4);
        painter.fillRect(inner, Ui::unusedColor(palette()));
        if (m_p.usageKnown() && m_p.length() > 0) {
            const int usedW = int(double(inner.width()) * double(m_p.usedSectors()) / double(m_p.length()));
            painter.fillRect(QRect(inner.left(), inner.top(), usedW, inner.height()), Ui::usedColor(palette()));
            const int unW = int(double(inner.width()) * double(m_p.unallocatedSectors()) / double(m_p.length()));
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

    const Partition &f = p.filesystem();
    if (fsIsReal(p.fsType) || p.fsType == FsType::Unknown || p.fsType == FsType::Luks)
        top->addWidget(new UsageBar(p, this));

    if (p.fsType == FsType::Luks) {
        // GParted's encryption section.
        auto *encBox = new QGroupBox(tr("Encryption"), this);
        auto *encForm = new QFormLayout(encBox);
        encForm->addRow(tr("Encryption:"), value(fsName(FsType::Luks), encBox));
        encForm->addRow(tr("UUID:"), value(p.uuid, encBox));
        encForm->addRow(tr("Status:"), value(p.isOpenLuks() ? tr("Open") : tr("Closed"), encBox));
        if (p.isOpenLuks()) {
            encForm->addRow(tr("Path:"), value(p.luks.path(), encBox));
            encForm->addRow(tr("Header:"), value(formatSize(p.luksHeader, p.sectorSize), encBox));
            encForm->addRow(tr("Volume key:"),
                            value(p.luks.keyLocation == LuksMapping::KeyLocation::KeyRing ? tr("kernel keyring")
                                                                                          : tr("dm-crypt table"),
                                  encBox));
        }
        top->addWidget(encBox);
    }

    if (p.fsType != FsType::Luks || p.isOpenLuks()) {
        auto *fsBox = new QGroupBox(tr("File System"), this);
        auto *fsForm = new QFormLayout(fsBox);
        auto *fsRow = new QWidget(fsBox);
        auto *fsRowL = new QHBoxLayout(fsRow);
        fsRowL->setContentsMargins(0, 0, 0, 0);
        auto *sw = new QLabel(fsRow);
        sw->setPixmap(Ui::fsIcon(f.fsType).pixmap(16, 16));
        fsRowL->addWidget(sw);
        fsRowL->addWidget(value(fsName(f.fsType), fsRow), 1);
        fsForm->addRow(tr("File system:"), fsRow);
        fsForm->addRow(tr("Label:"), value(f.fsLabel, fsBox));
        fsForm->addRow(tr("UUID:"), value(f.uuid, fsBox));
        fsForm->addRow(tr("Status:"), value(Ui::statusText(f), fsBox));
        if (!f.busy && !f.mountPoints.isEmpty())
            fsForm->addRow(tr("Mount point (fstab):"), value(f.mountPointsText(), fsBox));
        fsForm->addRow(tr("Size:"), value(formatSize(f.length(), f.sectorSize), fsBox));
        if (f.usageKnown()) {
            fsForm->addRow(tr("Used:"), value(formatSize(f.usedSectors(), f.sectorSize) + percent(f.usedSectors(), f.length()), fsBox));
            fsForm->addRow(tr("Unused:"), value(formatSize(f.unusedSectors(), f.sectorSize) + percent(f.unusedSectors(), f.length()), fsBox));
            if (f.unallocatedSectors() > 0)
                fsForm->addRow(tr("Unallocated:"),
                               value(formatSize(f.unallocatedSectors(), f.sectorSize)
                                         + percent(f.unallocatedSectors(), f.length()), fsBox));
        }
        if (f.fsBlockSize > 0)
            fsForm->addRow(tr("Block size:"), value(formatBytes(f.fsBlockSize), fsBox));
        top->addWidget(fsBox);
    }

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

    if (!p.allMessages().isEmpty()) {
        auto *msgBox = new QGroupBox(tr("Warnings"), this);
        auto *l = new QVBoxLayout(msgBox);
        auto *text = new QPlainTextEdit(p.allMessages().join(QStringLiteral("\n\n")), msgBox);
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

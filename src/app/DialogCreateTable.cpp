/*
 * QtParted - a Qt frontend to libparted for manipulating disk partitions
 * Copyright (C) 2002-2026 Vanni Brutto <zanac4ever@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "DialogCreateTable.h"

#include "UiHelpers.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace QtParted {

DialogCreateTable::DialogCreateTable(const Device &device, const QStringList &types, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Create partition table on %1").arg(device.path));

    auto *icon = new QLabel(this);
    icon->setPixmap(Ui::icon("dialog-warning", QStyle::SP_MessageBoxWarning).pixmap(48, 48));
    icon->setAlignment(Qt::AlignTop);

    auto *text = new QLabel(this);
    text->setWordWrap(true);
    text->setTextFormat(Qt::RichText);
    text->setText(tr("<p><b>WARNING: This will ERASE ALL DATA on the ENTIRE DISK %1</b></p>"
                     "<p>A new empty partition table replaces the current content of the device "
                     "(%2, %3) when the pending operations are applied.</p>")
                      .arg(device.path.toHtmlEscaped(), device.model.toHtmlEscaped(),
                           formatSize(device.length, device.sectorSize)));

    m_types = new QComboBox(this);
    for (const QString &t : types)
        m_types->addItem(t);
    // GPT is the sensible default for any modern system.
    const int gpt = m_types->findText(QStringLiteral("gpt"));
    if (gpt >= 0)
        m_types->setCurrentIndex(gpt);

    auto *hint = new QLabel(tr("Use <b>gpt</b> for UEFI systems and disks larger than 2 TiB, "
                               "<b>msdos</b> for legacy BIOS compatibility."),
                            this);
    hint->setWordWrap(true);
    hint->setForegroundRole(QPalette::PlaceholderText);

    auto *form = new QFormLayout;
    form->addRow(tr("Select new partition table &type:"), m_types);

    auto *body = new QVBoxLayout;
    body->addWidget(text);
    body->addLayout(form);
    body->addWidget(hint);

    auto *row = new QHBoxLayout;
    row->addWidget(icon);
    row->addLayout(body, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("&Add"));
    buttons->button(QDialogButtonBox::Cancel)->setDefault(true);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *top = new QVBoxLayout(this);
    top->addLayout(row);
    top->addWidget(buttons);
    resize(520, sizeHint().height());
}

QString DialogCreateTable::diskType() const
{
    return m_types->currentText();
}

} // namespace QtParted

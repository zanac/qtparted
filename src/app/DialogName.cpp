/*
 * QtParted - a Qt frontend to libparted for manipulating disk partitions
 * Copyright (C) 2002-2026 Vanni Brutto <zanac4ever@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "DialogName.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QVBoxLayout>

namespace QtParted {

DialogName::DialogName(const Partition &partition, int maxLength, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Set partition name on %1").arg(partition.path));
    m_edit = new QLineEdit(partition.name, this);
    m_edit->setMaxLength(maxLength);
    m_edit->setClearButtonEnabled(true);
    m_edit->selectAll();
    m_edit->setMinimumWidth(m_edit->fontMetrics().averageCharWidth() * 36);

    auto *form = new QFormLayout;
    form->addRow(tr("&Name:"), m_edit);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *top = new QVBoxLayout(this);
    top->addLayout(form);
    top->addWidget(buttons);
}

QString DialogName::name() const
{
    return m_edit->text().trimmed();
}

} // namespace QtParted

/*
 * QtParted - a Qt frontend to libparted for manipulating disk partitions
 * Copyright (C) 2002-2026 Vanni Brutto <zanac4ever@gmail.com>
 *
 * Behaviour follows GParted's DialogPasswordEntry (https://gparted.org).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "DialogPassword.h"

#include "UiHelpers.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace QtParted {

DialogPassword::DialogPassword(const QString &title, const QString &partitionPath, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(title);
    auto *heading = new QLabel(QStringLiteral("<b>%1</b>").arg(title.toHtmlEscaped()), this);
    m_edit = new QLineEdit(this);
    m_edit->setEchoMode(QLineEdit::Password);
    m_edit->setInputMethodHints(Qt::ImhHiddenText | Qt::ImhNoPredictiveText | Qt::ImhNoAutoUppercase);
    m_edit->setMinimumWidth(m_edit->fontMetrics().averageCharWidth() * 36);
    m_error = new QLabel(this);
    m_error->setWordWrap(true);
    QPalette pal = m_error->palette();
    pal.setColor(QPalette::WindowText, Ui::isDarkPalette(palette()) ? QColor(0xff, 0x8a, 0x80) : QColor(0xb0, 0x20, 0x20));
    m_error->setPalette(pal);
    m_error->hide();

    auto *form = new QFormLayout;
    form->addRow(tr("Passphrase for %1:").arg(partitionPath), m_edit);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("&Unlock"));
    buttons->button(QDialogButtonBox::Ok)->setIcon(Ui::icon("unlock", QStyle::SP_DialogOkButton));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *top = new QVBoxLayout(this);
    top->addWidget(heading);
    top->addLayout(form);
    top->addWidget(m_error);
    top->addWidget(buttons);
}

DialogPassword::~DialogPassword()
{
    clearPassword();
}

QByteArray DialogPassword::password() const
{
    return m_edit->text().toUtf8();
}

void DialogPassword::setError(const QString &message)
{
    m_error->setText(message);
    m_error->setVisible(!message.isEmpty());
    m_edit->selectAll();
    m_edit->setFocus();
}

void DialogPassword::clearPassword()
{
    // Overwrite before clearing so the old buffer doesn't keep the text.
    m_edit->setText(QString(m_edit->text().size(), QLatin1Char(' ')));
    m_edit->clear();
}

} // namespace QtParted

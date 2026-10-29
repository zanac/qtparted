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
#pragma once

#include <QDialog>

class QLabel;
class QLineEdit;

namespace QtParted {

// Asks for a LUKS passphrase.  The caller tries it and calls setError() to ask again.
class DialogPassword : public QDialog
{
    Q_OBJECT
public:
    DialogPassword(const QString &title, const QString &partitionPath, QWidget *parent = nullptr);
    ~DialogPassword() override;

    QByteArray password() const;
    void setError(const QString &message);
    // Wipe the entered text.
    void clearPassword();

private:
    QLineEdit *m_edit;
    QLabel *m_error;
};

} // namespace QtParted

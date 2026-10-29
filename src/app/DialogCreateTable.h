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

#include <QDialog>

class QComboBox;

namespace QtParted {

class DialogCreateTable : public QDialog
{
    Q_OBJECT
public:
    DialogCreateTable(const Device &device, const QStringList &types, QWidget *parent = nullptr);
    QString diskType() const;

private:
    QComboBox *m_types;
};

} // namespace QtParted

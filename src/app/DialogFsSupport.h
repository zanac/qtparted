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

#include <QDialog>

class QTableWidget;

namespace QtParted {

// Which file systems can be handled with the tools installed (GParted's "File
// System Support" dialog).
class DialogFsSupport : public QDialog
{
    Q_OBJECT
public:
    explicit DialogFsSupport(QWidget *parent = nullptr);

Q_SIGNALS:
    void rescanned();

private:
    void fill();
    QTableWidget *m_table;
};

} // namespace QtParted

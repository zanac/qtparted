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

class QLabel;
class QTextBrowser;

namespace QtParted {

// The user manual (doc/manual/manual.md, built into the program).  It is shown here
// rather than in a web browser because QtParted runs as root.
class DialogManual : public QDialog
{
    Q_OBJECT
public:
    explicit DialogManual(QWidget *parent = nullptr);

    // GitHub style anchor of a heading: "Mount, unmount" -> "mount-unmount".
    static QString headingAnchor(const QString &heading);

private:
    void addHeadingAnchors();
    void linkClicked(const QUrl &url);

    QTextBrowser *m_browser;
    QLabel *m_status;
};

} // namespace QtParted

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

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace QtParted {

class OperationDetail;

struct CommandResult {
    bool started = false;
    int exitCode = -1;
    QString output;
    QString error;

    bool ok() const { return started && exitCode == 0; }
};

namespace Tools {
// Locate an executable in PATH and the usual sbin directories.  Results are cached
// until clearCache() is called (e.g. when rescanning for supported file systems).
QString find(const QString &name);
inline bool has(const QString &name) { return !find(name).isEmpty(); }
void clearCache();
QString shellQuote(const QString &arg);
} // namespace Tools

// Run a program without a shell.  When `detail` is given the command line and its
// output are recorded as a child of it (streamed while the command runs).
// Commands run with LC_ALL=C so their output can be parsed.
CommandResult runCommand(const QString &program, const QStringList &args, OperationDetail *detail = nullptr,
                         const QByteArray &input = QByteArray(), int timeoutMs = -1);

} // namespace QtParted

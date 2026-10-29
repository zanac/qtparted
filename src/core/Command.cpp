/*
 * QtParted - a Qt frontend to libparted for manipulating disk partitions
 * Copyright (C) 2002-2026 Vanni Brutto <zanac4ever@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "Command.h"

#include "OperationDetail.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

namespace QtParted {

namespace {
QMutex g_toolsMutex;
QHash<QString, QString> g_toolsCache;
}

QString Tools::find(const QString &name)
{
    QMutexLocker lock(&g_toolsMutex);
    auto it = g_toolsCache.constFind(name);
    if (it != g_toolsCache.constEnd())
        return *it;
    QString path = QStandardPaths::findExecutable(name);
    if (path.isEmpty()) {
        static const QStringList extra = { QStringLiteral("/usr/local/sbin"), QStringLiteral("/usr/sbin"),
                                           QStringLiteral("/sbin"), QStringLiteral("/usr/bin"),
                                           QStringLiteral("/bin") };
        path = QStandardPaths::findExecutable(name, extra);
    }
    g_toolsCache.insert(name, path);
    return path;
}

void Tools::clearCache()
{
    QMutexLocker lock(&g_toolsMutex);
    g_toolsCache.clear();
}

QString Tools::shellQuote(const QString &arg)
{
    static const QRegularExpression safe(QStringLiteral("^[A-Za-z0-9_@%+=:,./-]+$"));
    if (!arg.isEmpty() && safe.match(arg).hasMatch())
        return arg;
    QString q = arg;
    q.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + q + QLatin1Char('\'');
}

CommandResult runCommand(const QString &program, const QStringList &args, OperationDetail *detail,
                         const QByteArray &input, int timeoutMs)
{
    CommandResult result;

    QString cmdline = Tools::shellQuote(program);
    for (const QString &a : args)
        cmdline += QLatin1Char(' ') + Tools::shellQuote(a);

    OperationDetail *cmdDetail = nullptr;
    OperationDetail *outDetail = nullptr;
    OperationDetail *errDetail = nullptr;
    if (detail)
        cmdDetail = &detail->addChild(cmdline, OperationDetail::Status::Executing, OperationDetail::Font::Monospace);

    const QString exe = Tools::find(program);
    if (exe.isEmpty()) {
        result.error = QCoreApplication::translate("Command", "Program %1 not found").arg(program);
        if (cmdDetail) {
            cmdDetail->addChild(result.error, OperationDetail::Status::None, OperationDetail::Font::Italic);
            cmdDetail->setStatus(OperationDetail::Status::Error);
        }
        return result;
    }

    QProcess proc;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    env.insert(QStringLiteral("LANG"), QStringLiteral("C"));
    env.insert(QStringLiteral("MTOOLS_SKIP_CHECK"), QStringLiteral("1"));
    proc.setProcessEnvironment(env);
    proc.start(exe, args);
    if (!proc.waitForStarted(10000)) {
        result.error = proc.errorString();
        if (cmdDetail) {
            cmdDetail->addChild(result.error, OperationDetail::Status::None, OperationDetail::Font::Italic);
            cmdDetail->setStatus(OperationDetail::Status::Error);
        }
        return result;
    }
    result.started = true;
    if (!input.isEmpty())
        proc.write(input);
    proc.closeWriteChannel();

    auto drain = [&]() {
        const QString out = QString::fromLocal8Bit(proc.readAllStandardOutput());
        const QString err = QString::fromLocal8Bit(proc.readAllStandardError());
        if (!out.isEmpty()) {
            result.output += out;
            if (cmdDetail) {
                if (!outDetail)
                    outDetail = &cmdDetail->addChild(QString(), OperationDetail::Status::None,
                                                     OperationDetail::Font::Monospace);
                outDetail->appendText(out);
            }
        }
        if (!err.isEmpty()) {
            result.error += err;
            if (cmdDetail) {
                if (!errDetail)
                    errDetail = &cmdDetail->addChild(QString(), OperationDetail::Status::None,
                                                     OperationDetail::Font::Italic);
                errDetail->appendText(err);
            }
        }
    };

    QElapsedTimer timer;
    timer.start();
    while (proc.state() != QProcess::NotRunning) {
        proc.waitForReadyRead(100);
        drain();
        if (timeoutMs > 0 && timer.elapsed() > timeoutMs) {
            proc.kill();
            proc.waitForFinished(2000);
            result.error += QCoreApplication::translate("Command", "\nCommand timed out");
            break;
        }
    }
    proc.waitForFinished(-1);
    drain();

    result.exitCode = proc.exitStatus() == QProcess::NormalExit ? proc.exitCode() : -1;
    if (cmdDetail)
        cmdDetail->setStatus(result.exitCode == 0 ? OperationDetail::Status::Success
                                                  : OperationDetail::Status::Error);
    return result;
}

} // namespace QtParted

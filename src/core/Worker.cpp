/*
 * QtParted - a Qt frontend to libparted for manipulating disk partitions
 * Copyright (C) 2002-2026 Vanni Brutto <zanac4ever@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "Worker.h"

#include "Operation.h"
#include "Parted.h"
#include "SystemInfo.h"

#include <QCoreApplication>
#include <QFileInfo>

#include <algorithm>

namespace QtParted {

std::vector<Device> scanDevices(const QStringList &userPaths, const std::function<void(const QString &)> &progress)
{
    SystemInfo sys;
    sys.load();

    QStringList paths;
    if (userPaths.isEmpty()) {
        paths = Parted::probeDevicePaths();
    } else {
        for (const QString &p : userPaths)
            paths.append(QFileInfo(p).absoluteFilePath());
        std::sort(paths.begin(), paths.end());
        paths.removeDuplicates();
    }

    std::vector<Device> devices;
    for (const QString &p : paths) {
        if (progress)
            progress(QCoreApplication::translate("Worker", "Searching %1 partitions").arg(p));
        if (auto d = Parted::readDevice(p, sys))
            devices.push_back(std::move(*d));
    }
    return devices;
}

OperationApplier::OperationApplier(std::vector<Operation *> operations, QObject *parent)
    : QObject(parent)
    , m_operations(std::move(operations))
{
}

void OperationApplier::run()
{
    int completed = 0;
    bool success = true;
    for (size_t i = 0; i < m_operations.size(); ++i) {
        if (m_stop)
            break;
        Q_EMIT operationStarted(int(i));
        const bool ok = m_operations[i]->apply();
        Q_EMIT operationFinished(int(i), ok);
        if (!ok) {
            success = false;
            break;
        }
        ++completed;
    }
    if (m_stop && completed < int(m_operations.size()))
        success = false;
    Q_EMIT finished(success, completed);
}

} // namespace QtParted

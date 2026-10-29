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

#include <QObject>
#include <QStringList>

#include <atomic>
#include <functional>
#include <vector>

namespace QtParted {

class Operation;

// Scan devices (blocking).  `userPaths` restricts scanning to the given devices or
// image files; progress receives "Scanning /dev/sda" style messages.
std::vector<Device> scanDevices(const QStringList &userPaths,
                                const std::function<void(const QString &)> &progress = {});

// Applies pending operations one after another; lives in a worker thread.
class OperationApplier : public QObject
{
    Q_OBJECT
public:
    explicit OperationApplier(std::vector<Operation *> operations, QObject *parent = nullptr);

    // Stop after the operation currently running.
    void requestStop() { m_stop = true; }

public Q_SLOTS:
    void run();

Q_SIGNALS:
    void operationStarted(int index);
    void operationFinished(int index, bool success);
    void finished(bool success, int completed);

private:
    std::vector<Operation *> m_operations;
    std::atomic<bool> m_stop{ false };
};

} // namespace QtParted

/*
 * QtParted - a Qt frontend to libparted for manipulating disk partitions
 * Copyright (C) 2002-2026 Vanni Brutto <zanac4ever@gmail.com>
 *
 * Operation queue management and the rules deciding which actions are allowed are
 * ported from GParted's Win_GParted (https://gparted.org),
 * Copyright (C) 2004 Bart Hakvoort and the GParted contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#pragma once

#include "Operation.h"

#include <memory>
#include <vector>

namespace QtParted {

struct AllowedActions {
    bool info = false;
    bool create = false;
    bool remove = false;
    bool format = false;
    bool name = false;
    bool flags = false;
    bool createTable = false;
};

// The devices as scanned plus the pending operations: the model behind the main window.
class Session
{
public:
    std::vector<Device> devices;
    std::vector<std::unique_ptr<Operation>> operations;

    int deviceIndex(const QString &path) const;
    // The device as it will look after the pending operations.
    Device displayDevice(int index) const;

    // Validates and queues the operation, merging it with an earlier one when possible.
    bool addOperation(std::unique_ptr<Operation> operation, QString *error = nullptr);
    void undoLast();
    void clear();
    // Deleting a not yet created partition removes every operation on it instead.
    void removeOperationsOnNew(const Partition &partition);
    int pendingCount(const QString &devicePath) const;

    int nextNewNumber() const;  // "New Partition #N"

    static bool validPartition(const Device &device, const Partition &partition, QString *error);
    static AllowedActions allowedActions(const Device &display, const Partition *selected,
                                         int pendingOnDevice);
};

} // namespace QtParted

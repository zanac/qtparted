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
#include "Session.h"

#include <QCoreApplication>

#include <algorithm>

namespace QtParted {

static QString tr(const char *s)
{
    return QCoreApplication::translate("Session", s);
}

int Session::deviceIndex(const QString &path) const
{
    for (size_t i = 0; i < devices.size(); ++i)
        if (devices[i].path == path)
            return int(i);
    return -1;
}

Device Session::displayDevice(int index) const
{
    if (index < 0 || index >= int(devices.size()))
        return Device();
    Device d = devices[size_t(index)];
    for (const auto &op : operations)
        if (op->device() == d)
            op->applyToVisual(d);
    return d;
}

bool Session::validPartition(const Device &device, const Partition &p, QString *error)
{
    auto fail = [&](const QString &m) {
        if (error)
            *error = m;
        return false;
    };
    if (p.start < 0)
        return fail(tr("A partition cannot start (%1) before the start of the device").arg(p.start));
    if (p.end >= device.length)
        return fail(tr("A partition cannot end (%1) after the end of the device (%2)").arg(p.end).arg(device.length - 1));
    if (p.length() <= 0)
        return fail(tr("A partition cannot have a length of %1 sectors").arg(p.length()));
    if (p.usageKnown() && p.length() < p.sectorsUsed)
        return fail(tr("A partition with used sectors (%1) greater than its length (%2) is not valid")
                        .arg(p.sectorsUsed).arg(p.length()));
    return true;
}

static bool affectSamePartition(const Operation &first, const Operation &second)
{
    if (first.type() == OperationType::Delete)
        return false;  // nothing left to merge into
    return first.partitionNew().path == second.original().path;
}

bool Session::addOperation(std::unique_ptr<Operation> op, QString *error)
{
    if (!op)
        return false;
    if (op->type() == OperationType::Create) {
        const int index = deviceIndex(op->device().path);
        if (index < 0 || !validPartition(devices[size_t(index)], op->partitionNew(), error))
            return false;
    }
    op->createDescription();

    for (int i = int(operations.size()) - 1; i >= 0; --i) {
        if (operations[size_t(i)]->device() == op->device() && affectSamePartition(*operations[size_t(i)], *op)) {
            if (operations[size_t(i)]->mergeWith(*op))
                return true;
            break;
        }
    }
    operations.push_back(std::move(op));
    return true;
}

void Session::undoLast()
{
    if (!operations.empty())
        operations.pop_back();
}

void Session::clear()
{
    operations.clear();
}

void Session::removeOperationsOnNew(const Partition &p)
{
    for (auto it = operations.begin(); it != operations.end();) {
        if ((*it)->type() != OperationType::Delete && (*it)->device().path == p.devicePath
            && (*it)->partitionNew().path == p.path)
            it = operations.erase(it);
        else
            ++it;
    }
}

int Session::pendingCount(const QString &devicePath) const
{
    int n = 0;
    for (const auto &op : operations)
        if (op->device().path == devicePath)
            ++n;
    return n;
}

int Session::nextNewNumber() const
{
    int highest = 0;
    for (const auto &op : operations)
        if (op->type() == OperationType::Create && op->partitionNew().status == PartitionStatus::New)
            highest = std::max(highest, op->partitionNew().number);
    return highest + 1;
}

AllowedActions Session::allowedActions(const Device &display, const Partition *sel, int pendingOnDevice)
{
    AllowedActions a;
    a.createTable = display.length > 0 && display.highestBusy == 0 && pendingOnDevice == 0;
    if (!sel)
        return a;
    a.info = true;

    const bool inTable = sel->isPartitionInTable();
    if (inTable && display.partitionNamingSupported() && !display.readonly
        && (sel->status == PartitionStatus::Real || sel->status == PartitionStatus::New))
        a.name = true;
    if (inTable && sel->status == PartitionStatus::Real && !display.readonly && !sel->availableFlags.isEmpty())
        a.flags = true;

    // Only informational actions while the file system is in use.
    if (sel->busy)
        return a;

    if (sel->fsType == FsType::Unallocated)
        a.create = true;
    if (sel->type == PartitionType::Unallocated)
        return a;

    if (sel->type == PartitionType::Extended) {
        a.remove = !display.readonly && sel->logicals.size() == 1
                && sel->logicals.front().type == PartitionType::Unallocated;
        return a;
    }

    if ((sel->type == PartitionType::Primary || sel->type == PartitionType::Logical
         || sel->type == PartitionType::Unpartitioned)
        && sel->fsType != FsType::Unallocated) {
        a.format = !display.isImageFile && !display.readonly;
        a.remove = sel->type != PartitionType::Unpartitioned && !display.readonly;
    }
    return a;
}

} // namespace QtParted

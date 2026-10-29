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

#include "FileSystem.h"
#include "Layout.h"

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
    if (op->type() == OperationType::Create || op->type() == OperationType::Copy
        || op->type() == OperationType::ResizeMove) {
        const int index = deviceIndex(op->device().path);
        if (index < 0 || !validPartition(devices[size_t(index)], op->partitionNew(), error))
            return false;
    }
    op->createDescription();

    for (int i = int(operations.size()) - 1; i >= 0; --i) {
        // A copy of this partition in between takes the state it had then: changes
        // queued after it must not be moved before it (GParted's merge rules).
        if (operations[size_t(i)]->type() == OperationType::Copy) {
            const auto &copy = static_cast<const OperationCopy &>(*operations[size_t(i)]);
            if (copy.copied().devicePath == op->original().devicePath && copy.copied().path == op->original().path)
                break;
        }
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
            && (*it)->partitionNew().path == p.path && (*it)->partitionNew().start == p.start)
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

int Session::pendingCountFor(const Partition &p) const
{
    int n = 0;
    for (const auto &op : operations)
        if (op->device().path == p.devicePath
            && (op->original().path == p.path || op->partitionNew().path == p.path))
            ++n;
    return n;
}

int Session::nextNewNumber() const
{
    int highest = 0;
    for (const auto &op : operations)
        if ((op->type() == OperationType::Create && op->partitionNew().status == PartitionStatus::New)
            || (op->type() == OperationType::Copy && op->partitionNew().status == PartitionStatus::Copy))
            highest = std::max(highest, op->partitionNew().number);
    return highest + 1;
}

AllowedActions Session::allowedActions(const Device &display, const Partition *sel, int pendingOnDevice,
                                      const Partition *clipboard)
{
    // Port of GParted's Win_GParted::set_valid_operations().
    AllowedActions a;
    a.createTable = display.length > 0 && display.highestBusy == 0 && pendingOnDevice == 0;
    if (!sel)
        return a;
    a.info = true;

    // The partition directly holding the file system: inside open LUKS encryption.
    const Partition &fsPart = sel->filesystem();
    const FsSupport fs = FileSystemRegistry::instance().support(fsPart.fsType);
    const FsSupport enc = FileSystemRegistry::instance().support(FsType::Luks);
    const bool real = sel->status == PartitionStatus::Real;
    const bool isNew = sel->status == PartitionStatus::New;
    const bool isLuks = sel->fsType == FsType::Luks;
    const bool openLuks = sel->isOpenLuks();
    // File system tools need a device node: no file system changes on image files.
    const bool fsTools = !display.isImageFile;
    const bool inTable = sel->isPartitionInTable();

    if (sel->status == PartitionStatus::Copy) {
        // A pending paste: it can only be removed again (GParted).
        a.remove = !display.readonly;
        return a;
    }

    // Open/close encryption, while the file system inside is not in use.
    if (real && isLuks && fsTools && !fsPart.busy && (sel->busy ? openLuks : true)) {
        a.openCrypt = true;
        a.cryptIsOpen = sel->busy;
    }

    // Mount/unmount and swapon/swapoff.
    if (real && fsTools && sel->type != PartitionType::Extended && fsPart.fsType != FsType::Lvm2Pv
        && fsPart.fsType != FsType::Luks && fsPart.fsType != FsType::LinuxSwraid
        && fsPart.fsType != FsType::Ataraid && fsPart.fsType != FsType::Bcache && fsPart.fsType != FsType::Jbd
        && fsPart.fsType != FsType::Unallocated && fsPart.fsType != FsType::Extended) {
        if (fsPart.fsType == FsType::LinuxSwap)
            a.toggle = fsPart.busy ? AllowedActions::Toggle::SwapOff : AllowedActions::Toggle::SwapOn;
        else if (fsPart.busy && !fsPart.mountPoints.isEmpty())
            a.toggle = AllowedActions::Toggle::Unmount;
        else if (!fsPart.busy && !fsPart.mountPoints.isEmpty())
            a.mountOn = fsPart.mountPoints;
    }
    // Volume Group activation, the VG name being shown as the mount point.
    if (real && fsTools && fsPart.fsType == FsType::Lvm2Pv && !fsPart.mountPoints.isEmpty())
        a.toggle = fsPart.busy ? AllowedActions::Toggle::DeactivateVg : AllowedActions::Toggle::ActivateVg;

    if (inTable && display.partitionNamingSupported() && !display.readonly && (real || isNew))
        a.name = true;
    if (inTable && real && !display.readonly && !sel->availableFlags.isEmpty())
        a.flags = true;

    if (fsPart.busy) {
        // Online changes need the kernel to accept a changed partition table.
        if (!display.readonly && fsTools && inTable) {
            if (!isLuks && fs.canResizeOnline())
                a.resize = true;
            if (openLuks && ((enc.onlineGrow != FsMethod::None && fs.onlineGrow != FsMethod::None)
                             || (enc.onlineShrink != FsMethod::None && fs.onlineShrink != FsMethod::None)))
                a.resize = true;
            if (sel->type == PartitionType::Extended)
                a.resize = true;
        }
        if (real && fsTools && fs.onlineWriteLabel)
            a.label = true;
        // Only unmount/swapoff/VG deactivation and online actions while in use.
        return a;
    }

    if (sel->fsType == FsType::Unallocated)
        a.create = true;
    if (sel->type == PartitionType::Unallocated) {
        // Encrypted content can't be pasted into a plain partition (GParted).
        if (clipboard && !display.readonly && clipboard->sectorSize == display.sectorSize
            && clipboard->filesystem().fsType != FsType::Luks
            && Layout::pasteRequiredBytes(display, *sel, clipboard->filesystem()) <= sel->byteLength())
            a.paste = true;
        return a;
    }

    if (sel->type == PartitionType::Extended) {
        a.remove = !display.readonly && sel->logicals.size() == 1
                && sel->logicals.front().type == PartitionType::Unallocated;
        a.resize = !display.readonly;
        return a;
    }

    if ((sel->type == PartitionType::Primary || sel->type == PartitionType::Logical
         || sel->type == PartitionType::Unpartitioned)
        && sel->fsType != FsType::Unallocated) {
        a.format = fsTools && !display.readonly;
        // Open encryption keeps the partition busy: close it before deleting.
        a.remove = sel->type != PartitionType::Unpartitioned && !display.readonly && !sel->busy;
        if (!display.readonly && inTable) {
            if (isNew)
                a.resize = true;
            else if (fsTools && !isLuks && fs.canResize())
                a.resize = true;
            else if (fsTools && isLuks && !sel->busy && (enc.grow != FsMethod::None || enc.move != FsMethod::None))
                a.resize = true;
            else if (fsTools && openLuks
                     && ((enc.onlineGrow != FsMethod::None && fs.grow != FsMethod::None)
                         || (enc.onlineShrink != FsMethod::None && fs.shrink != FsMethod::None)))
                a.resize = true;
        }
        // Closed encryption is only copied while open (its content).
        if (real && fsTools && fsPart.fsType != FsType::Luks && fs.copy != FsMethod::None)
            a.copy = true;
        if (real && fsTools && fs.writeLabel)
            a.label = true;
        if (real && fsTools && fs.writeUuid)
            a.uuid = true;
        if (real && fsTools && fs.check)
            a.check = true;
        if (clipboard && real && fsTools && !display.readonly && clipboard->sectorSize == display.sectorSize
            && clipboard->filesystem().byteLength() <= fsPart.byteLength() && !sel->busy
            && !(clipboard->devicePath == sel->devicePath && clipboard->start <= sel->end
                 && sel->start <= clipboard->end))
            a.paste = true;
    }
    return a;
}

} // namespace QtParted

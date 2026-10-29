/*
 * QtParted - a Qt frontend to libparted for manipulating disk partitions
 * Copyright (C) 2002-2026 Vanni Brutto <zanac4ever@gmail.com>
 *
 * The resize, move and copy sequences are ported from GParted_Core
 * (https://gparted.org), Copyright (C) 2004 Bart Hakvoort and the GParted contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "Steps.h"

#include "FileSystem.h"
#include "Luks.h"
#include "Operation.h"
#include "OperationDetail.h"
#include "Parted.h"

#include <QCoreApplication>

#include <algorithm>

namespace QtParted {
namespace Steps {

namespace {

QString tr(const char *s)
{
    return QCoreApplication::translate("Steps", s);
}

bool bug(OperationDetail &detail, const QString &what)
{
    detail.addChild(tr("QtParted bug") + QStringLiteral(": ") + what, OperationDetail::Status::Error,
                    OperationDetail::Font::Italic);
    return false;
}

const FileSystem *fsOf(const Partition &p)
{
    return FileSystemRegistry::instance().get(p.fsType);
}

// Resize the file system from the geometry of `o` to the one of `n`; growing always
// fills the partition.
bool resizeFileSystem(const Partition &o, const Partition &n, OperationDetail &detail)
{
    const FileSystem *fs = fsOf(n);
    if (!fs)
        return false;
    const bool grow = n.length() >= o.length();
    const FsSupport &s = fs->support();
    const FsMethod method = grow ? (o.busy ? s.onlineGrow : s.grow) : (o.busy ? s.onlineShrink : s.shrink);
    switch (method) {
    case FsMethod::Libparted:
        if (!Parted::libpartedFsResize(o, n, detail))
            return false;
        // libparted's FAT resizer can leave unconnected clusters behind: repair them.
        return !s.check || fs->check(n, detail);
    case FsMethod::External:
    case FsMethod::Recreate:
        return fs->resize(n, detail, grow);
    default:
        return false;
    }
}

bool shrinkFileSystem(const Partition &o, const Partition &n, OperationDetail &detail)
{
    if (n.length() >= o.length())
        return bug(detail, tr("the new partition size is larger or the same for a shrink only step"));
    OperationDetail &od = detail.addChild(tr("shrink file system"));
    const bool ok = resizeFileSystem(o, n, od);
    od.setSuccessAndCaptureErrors(ok);
    return ok;
}

void rollbackMoveFileSystem(const Partition &src, const Partition &dst, OperationDetail &detail, ByteValue done)
{
    if (done <= 0)
        return;
    // Work out which part was copied, and where, then copy it back.
    Partition tmpSrc = src;
    Partition tmpDst = dst;
    const Sector doneSectors = done / src.sectorSize;
    if (dst.start > src.start) {
        // Copied backwards: the last `done` bytes.
        const Sector distance = dst.start - src.start;
        tmpSrc.start = tmpSrc.end - (doneSectors - 1) + distance;
        tmpDst.start = tmpDst.end - (doneSectors - 1) + distance;
        if (tmpSrc.start > tmpSrc.end)
            return;  // nothing of the source was overwritten yet
    } else {
        const Sector distance = src.start - dst.start;
        tmpSrc.end = tmpSrc.start + (doneSectors - 1) - distance;
        tmpDst.end = tmpDst.start + (doneSectors - 1) - distance;
        if (tmpSrc.start > tmpSrc.end)
            return;
    }
    OperationDetail &od = detail.addChild(tr("rollback failed file system move"));
    ByteValue ignored = 0;
    const bool ok = Parted::copyBlocks(dst.devicePath, src.devicePath, tmpDst.start, tmpSrc.start,
                                       tmpSrc.byteLength(), od, ignored);
    od.setSuccessAndCaptureErrors(ok);
}

bool moveFileSystem(const Partition &o, const Partition &n, OperationDetail &detail)
{
    OperationDetail &od = detail.addChild(n.start < o.start ? tr("move file system to the left")
                                          : n.start > o.start ? tr("move file system to the right")
                                                              : tr("move file system"));
    if (n.start == o.start) {
        od.addChild(tr("new and old file system have the same position.  Hence skipping this operation"),
                    OperationDetail::Status::None, OperationDetail::Font::Italic);
        od.setSuccessAndCaptureErrors(true);
        return true;
    }
    const FileSystem *fs = fsOf(o);
    bool ok = false;
    switch (fs ? fs->support().move : FsMethod::None) {
    case FsMethod::Recreate:
        ok = true;  // linux-swap: recreated at the new position afterwards
        break;
    case FsMethod::Block: {
        ByteValue done = 0;
        ok = Parted::copyBlocks(o.devicePath, n.devicePath, o.start, n.start, o.byteLength(), od, done);
        const bool overlap = n.start <= o.end && o.start <= n.end;
        if (!ok && overlap)
            rollbackMoveFileSystem(o, n, od, done);
        break;
    }
    default:
        od.addChild(tr("moving is not available for this file system"), OperationDetail::Status::Error);
        break;
    }
    od.setSuccessAndCaptureErrors(ok);
    return ok;
}

bool move(const Partition &o, Partition &n, OperationDetail &detail)
{
    if (o.length() != n.length())
        return bug(detail, tr("size of the partition is changing for a move only step"));
    if (!checkRepair(o, detail))
        return false;

    // Logical partitions are preceded by their EBR: grow the partition first over all the
    // space involved so neither this EBR (moving left) nor the next one (moving right) is
    // overwritten by the data.
    Partition all = o;
    all.alignment = Alignment::None;
    all.start = std::min(o.start, n.start);
    all.end = std::max(o.end, n.end);

    bool ok = false;
    if (Parted::resizeMovePartition(o, all, detail, true)) {
        if (!moveFileSystem(o, n, detail)) {
            OperationDetail &rb = detail.addChild(tr("rollback last change to the partition"));
            Partition restore = o;
            restore.alignment = Alignment::None;
            if (Parted::resizeMovePartition(all, restore, rb, false)) {
                rb.setSuccessAndCaptureErrors(true);
                checkRepair(o, detail);
            } else {
                rb.setSuccessAndCaptureErrors(false);
            }
        } else {
            ok = true;
        }
    }
    if (ok)
        ok = Parted::resizeMovePartition(all, n, detail, false) && Parted::updateBootSector(n, detail);
    if (!ok)
        return false;
    if (n.fsType == FsType::LinuxSwap)
        return recreateSwap(n, detail);
    return true;
}

bool resize(const Partition &o, Partition &n, OperationDetail &detail)
{
    if (o.start != n.start)
        return bug(detail, tr("start of the partition is changing for a resize only step"));
    if (n.fsType == FsType::Luks)
        return resizeEncryption(o, n, detail);
    if (n.fsType == FsType::LinuxSwap)
        return Parted::resizeMovePartition(o, n, detail, true) && recreateSwap(n, detail);
    if (n.length() < o.length())
        return checkRepair(n, detail) && shrinkFileSystem(o, n, detail)
            && Parted::resizeMovePartition(o, n, detail, false);
    if (n.length() > o.length())
        return checkRepair(n, detail) && Parted::resizeMovePartition(o, n, detail, true)
            && maximizeFileSystem(n, detail);
    return true;
}

bool copyFileSystem(const Partition &src, const Partition &dst, OperationDetail &detail)
{
    OperationDetail &od = detail.addChild(tr("copy file system from %1 to %2").arg(src.path, dst.path));
    const FileSystem *fs = fsOf(src);
    bool ok = false;
    switch (fs ? fs->support().copy : FsMethod::None) {
    case FsMethod::Recreate:
        ok = true;  // linux-swap is recreated, not copied
        break;
    case FsMethod::Block: {
        ByteValue done = 0;
        ok = Parted::copyBlocks(src.devicePath, dst.devicePath, src.start, dst.start, src.byteLength(), od, done);
        break;
    }
    default:
        od.addChild(tr("copying is not available for this file system"), OperationDetail::Status::Error);
        break;
    }
    od.setSuccessAndCaptureErrors(ok);
    return ok;
}

} // namespace

bool checkRepairMaximize(const Partition &p, OperationDetail &detail)
{
    // Port of GParted's check_repair_maximize().
    if (p.fsType == FsType::Luks) {
        // Treat the encryption as closed so its offline grow capability decides; the
        // open state comes from the mapping itself.
        Partition offline = p;
        offline.busy = false;
        if (!maximizeEncryption(offline, detail))
            return false;
        if (p.isOpenLuks())
            return maximizeFileSystem(p.filesystem(), detail);
        return true;
    }
    return maximizeFileSystem(p, detail);
}

bool checkRepair(const Partition &p, OperationDetail &detail)
{
    if (p.busy)
        return true;  // checking an online file system is a successful non-operation
    OperationDetail &od = detail.addChild(
        tr("check file system on %1 for errors and (if possible) fix them").arg(p.path));
    const FileSystem *fs = fsOf(p);
    if (!fs || !fs->support().check) {
        od.addChild(tr("checking is not available for this file system"), OperationDetail::Status::None,
                    OperationDetail::Font::Italic);
        od.setStatus(OperationDetail::Status::Warning);
        return true;
    }
    const bool ok = fs->check(p, od);
    od.setSuccessAndCaptureErrors(ok);
    return ok;
}

bool maximizeFileSystem(const Partition &p, OperationDetail &detail)
{
    OperationDetail &od = detail.addChild(tr("grow file system to fill the partition"));
    const FsSupport s = FileSystemRegistry::instance().support(p.fsType);
    if ((p.busy ? s.onlineGrow : s.grow) == FsMethod::None) {
        od.addChild(tr("growing is not available for this file system"), OperationDetail::Status::None,
                    OperationDetail::Font::Italic);
        od.setStatus(OperationDetail::Status::Warning);
        return true;
    }
    const bool ok = resizeFileSystem(p, p, od);
    od.setSuccessAndCaptureErrors(ok);
    return ok;
}

bool recreateSwap(const Partition &p, OperationDetail &detail)
{
    if (p.fsType != FsType::LinuxSwap)
        return bug(detail, tr("not a linux-swap file system for a recreate linux-swap only step"));
    if (!Parted::eraseSignatures(p, detail))
        return false;
    OperationDetail &od = detail.addChild(tr("recreate %1 file system").arg(fsName(FsType::LinuxSwap)));
    const FileSystem *fs = fsOf(p);
    const bool ok = fs && fs->resize(p, od, true);
    od.setSuccessAndCaptureErrors(ok);
    return ok;
}

bool maximizeEncryption(const Partition &p, OperationDetail &detail)
{
    if (p.fsType != FsType::Luks)
        return bug(detail, tr("partition does not contain LUKS encryption for a maximize encryption only step"));
    OperationDetail &od = detail.addChild(tr("grow encryption volume to fill the partition"));
    const FsSupport s = FileSystemRegistry::instance().support(FsType::Luks);
    if ((p.busy ? s.onlineGrow : s.grow) == FsMethod::None) {
        od.addChild(tr("growing is not available for this encryption volume"), OperationDetail::Status::None,
                    OperationDetail::Font::Italic);
        od.setStatus(OperationDetail::Status::Warning);
        return true;
    }
    const bool ok = Luks::resize(p, true, od);
    od.setSuccessAndCaptureErrors(ok);
    return ok;
}

bool shrinkEncryption(const Partition &o, const Partition &n, OperationDetail &detail)
{
    if (!o.isOpenLuks())
        return bug(detail, tr("partition does not contain open LUKS encryption for a shrink encryption only step"));
    OperationDetail &od = detail.addChild(tr("shrink encryption volume"));
    const bool ok = Luks::resize(n, false, od);
    od.setSuccessAndCaptureErrors(ok);
    return ok;
}

bool resizeEncryption(const Partition &o, Partition &n, OperationDetail &detail)
{
    // Port of GParted's resize_encryption().
    const Sector delta = n.length() - o.length();
    if (!o.busy && delta < 0)
        return bug(detail, tr("impossible to shrink a closed LUKS encryption volume"));
    if (!o.busy)
        return Parted::resizeMovePartition(o, n, detail, true) && maximizeEncryption(n, detail);
    if (!o.isOpenLuks() || !n.isOpenLuks())
        return bug(detail, tr("open LUKS encryption without the file system inside it"));

    const Partition &fsOld = o.filesystem();
    const Partition &fsNew = n.filesystem();
    if (fsNew.fsType == FsType::LinuxSwap) {
        // The encryption is resized, linux-swap inside is recreated.
        if (delta < 0)
            return shrinkEncryption(o, n, detail) && Parted::resizeMovePartition(o, n, detail, false)
                && recreateSwap(fsNew, detail);
        if (delta > 0)
            return Parted::resizeMovePartition(o, n, detail, true) && maximizeEncryption(n, detail)
                && recreateSwap(fsNew, detail);
        return true;
    }
    if (delta < 0)
        return checkRepair(fsOld, detail) && shrinkFileSystem(fsOld, fsNew, detail)
            && shrinkEncryption(o, n, detail) && Parted::resizeMovePartition(o, n, detail, false);
    if (delta > 0)
        return checkRepair(fsOld, detail) && Parted::resizeMovePartition(o, n, detail, true)
            && maximizeEncryption(n, detail) && maximizeFileSystem(fsNew, detail);
    return true;
}

bool resizeMove(const Partition &o, Partition &n, OperationDetail &detail)
{
    if (n.alignment == Alignment::Cylinder && !n.strictStart && !Parted::calculateExactGeometry(o, n, detail))
        return false;

    if (o.type == PartitionType::Extended)
        return Parted::resizeMovePartition(o, n, detail, false);
    if (n.start == o.start)
        return resize(o, n, detail);
    if (n.length() == o.length())
        return move(o, n, detail);

    // Move and resize in two steps: grow after moving, shrink before moving.
    Partition temp;
    if (n.length() > o.length()) {
        temp = n;
        temp.end = temp.start + o.length() - 1;
    } else {
        temp = o;
        temp.end = o.start + n.length() - 1;
    }
    const Alignment wanted = temp.alignment;
    temp.alignment = Alignment::None;
    bool ok = resizeMove(o, temp, detail);
    temp.alignment = wanted;
    if (ok)
        ok = resizeMove(temp, n, detail);
    return ok;
}

bool copy(const Partition &src, Partition &dst, OperationDetail &detail)
{
    // Never onto itself: the destination is erased before the copy.
    if (src.devicePath == dst.devicePath && src.start <= dst.end && dst.start <= src.end) {
        detail.addChild(tr("the destination overlaps the source partition %1: nothing was changed").arg(src.path),
                        OperationDetail::Status::Error, OperationDetail::Font::Italic);
        return false;
    }
    if (dst.byteLength() < src.byteLength()) {
        detail.addChild(tr("the destination is smaller than the source partition"), OperationDetail::Status::Error,
                        OperationDetail::Font::Italic);
        return false;
    }
    if (dst.status == PartitionStatus::Copy) {
        const Sector minSize = (src.byteLength() + dst.sectorSize - 1) / dst.sectorSize;
        if (!Parted::createPartition(dst, minSize, detail))
            return false;
    }
    if (!(Parted::eraseSignatures(dst, detail) && Parted::setPartitionType(dst, detail)
          && copyFileSystem(src, dst, detail) && Parted::updateBootSector(dst, detail)))
        return false;
    if (dst.fsType == FsType::LinuxSwap)
        return recreateSwap(dst, detail);
    if (!checkRepair(dst, detail))
        return false;
    if (dst.byteLength() > src.byteLength())
        return maximizeFileSystem(dst, detail);
    return true;
}

bool labelFileSystem(const Partition &p, OperationDetail &detail)
{
    OperationDetail &od = detail.addChild(
        p.fsLabel.isEmpty() ? tr("Clear file system label on %1").arg(p.path)
                            : tr("Set file system label to \"%1\" on %2").arg(p.fsLabel, p.path));
    const FileSystem *fs = fsOf(p);
    const bool allowed = fs && (p.busy ? fs->support().onlineWriteLabel : fs->support().writeLabel);
    const bool ok = allowed && fs->writeLabel(p, od);
    od.setSuccessAndCaptureErrors(ok);
    return ok;
}

bool changeUuid(const Partition &p, OperationDetail &detail)
{
    OperationDetail &od = detail.addChild(tr("Set UUID on %1 to a new, random value").arg(p.path));
    const FileSystem *fs = fsOf(p);
    const bool ok = fs && fs->writeUuid(p, od);
    od.setSuccessAndCaptureErrors(ok);
    return ok;
}

} // namespace Steps
} // namespace QtParted

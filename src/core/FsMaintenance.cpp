/*
 * QtParted - a Qt frontend to libparted for manipulating disk partitions
 * Copyright (C) 2002-2026 Vanni Brutto <zanac4ever@gmail.com>
 *
 * Resize, check, label and UUID commands of each file system, ported from GParted
 * (https://gparted.org), Copyright (C) 2004 Bart Hakvoort and the GParted contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "FileSystem.h"

#include "Command.h"
#include "OperationDetail.h"
#include "Parted.h"

#include <QCoreApplication>
#include <QDir>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QUuid>

#include <cstdlib>
#include <unistd.h>

namespace QtParted {

static QString tr(const char *s)
{
    return QCoreApplication::translate("FileSystem", s);
}

QString randomUuid()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

bool withMountedFileSystem(const Partition &p, const QString &mountType, OperationDetail &detail,
                           const std::function<bool(const QString &)> &body)
{
    if (p.busy) {
        // Online: use a directory the file system is mounted on.
        for (const QString &mp : p.mountPoints)
            if (QDir(mp).exists())
                return body(mp);
        detail.addChild(tr("No directory mount point found in %1").arg(p.mountPointsText()),
                        OperationDetail::Status::Error);
        return false;
    }

    // Private temporary mount point (0700), like GParted's mk_temp_dir().
    QByteArray tmpl = QDir::tempPath().toLocal8Bit() + "/qtparted-XXXXXX";
    if (!::mkdtemp(tmpl.data())) {
        detail.addChild(tr("Failed to create temporary directory"), OperationDetail::Status::Error);
        return false;
    }
    const QString dir = QString::fromLocal8Bit(tmpl);
    detail.addChild(tr("Created directory %1").arg(dir), OperationDetail::Status::None, OperationDetail::Font::Italic);

    bool ok = runCommand(QStringLiteral("mount"), { QStringLiteral("-v"), QStringLiteral("-t"), mountType, p.path, dir },
                         &detail).ok();
    if (ok) {
        ok = body(dir);
        ok = runCommand(QStringLiteral("umount"), { QStringLiteral("-v"), dir }, &detail).ok() && ok;
    }
    if (::rmdir(tmpl.constData()) == 0)
        detail.addChild(tr("Removed directory %1").arg(dir), OperationDetail::Status::None, OperationDetail::Font::Italic);
    return ok;
}

namespace {

QString sizeK(const Partition &p)
{
    return QString::number(p.byteLength() / KiB) + QLatin1Char('K');
}

bool run(const QString &program, const QStringList &args, OperationDetail &d)
{
    return runCommand(program, args, &d).ok();
}

// --- ext2/3/4 ----------------------------------------------------------------------

bool extResize(const Partition &p, OperationDetail &d, bool fill)
{
    QStringList args{ QStringLiteral("-p") };
    if (p.fsType == FsType::Ext4 && p.byteLength() >= 16 * TiB) {
        // Over 16 TiB ext4 needs the 64bit feature: resize2fs -b converts.
        const CommandResult r = runCommand(QStringLiteral("tune2fs"), { QStringLiteral("-l"), p.path });
        static const QRegularExpression features(QStringLiteral("^Filesystem features:\\s*(.*)$"),
                                                 QRegularExpression::MultilineOption);
        const auto m = features.match(r.output);
        if (!m.hasMatch()) {
            d.addChild(tr("Failed to determine whether the ext4 file system has the 64bit feature enabled"),
                       OperationDetail::Status::Error);
            return false;
        }
        if (!m.captured(1).contains(QLatin1String("64bit")))
            args << QStringLiteral("-b");
    }
    args << p.path;
    if (!fill)
        args << sizeK(p);
    return run(QStringLiteral("resize2fs"), args, d);
}

// --- ntfs --------------------------------------------------------------------------

bool ntfsResize(const Partition &p, OperationDetail &d, bool fill)
{
    QStringList args{ QStringLiteral("--force"), QStringLiteral("--force") };
    if (!fill)
        args << QStringLiteral("-s") << QString::number(p.byteLength());
    OperationDetail &sim = d.addChild(tr("run simulation"));
    bool ok = run(QStringLiteral("ntfsresize"), args + QStringList{ QStringLiteral("--no-action"), p.path }, sim);
    sim.setSuccessAndCaptureErrors(ok);
    if (!ok)
        return false;
    OperationDetail &real = d.addChild(tr("real resize"));
    ok = run(QStringLiteral("ntfsresize"), args + QStringList{ p.path }, real);
    real.setSuccessAndCaptureErrors(ok);
    return ok;
}

// --- btrfs -------------------------------------------------------------------------

QString btrfsDevid(const QString &path)
{
    const CommandResult r = runCommand(QStringLiteral("btrfs"),
                                       { QStringLiteral("inspect-internal"), QStringLiteral("dump-super"), path });
    static const QRegularExpression re(QStringLiteral("^dev_item\\.devid\\s+(\\d+)"), QRegularExpression::MultilineOption);
    const auto m = re.match(r.output);
    return m.hasMatch() ? m.captured(1) : QString();
}

bool btrfsResize(const Partition &p, OperationDetail &d, bool fill)
{
    const QString devid = btrfsDevid(p.path);
    if (devid.isEmpty()) {
        d.addChild(tr("Failed to find devid for path %1").arg(p.path), OperationDetail::Status::Error);
        return false;
    }
    return withMountedFileSystem(p, QStringLiteral("btrfs"), d, [&](const QString &mp) {
        const QString size = fill ? QStringLiteral("max") : sizeK(p);
        return run(QStringLiteral("btrfs"),
                   { QStringLiteral("filesystem"), QStringLiteral("resize"), devid + QLatin1Char(':') + size, mp }, d);
    });
}

// --- swap --------------------------------------------------------------------------

bool swapRecreate(const Partition &p, OperationDetail &d, bool)
{
    QStringList args{ QStringLiteral("-L"), p.fsLabel };
    if (!p.uuid.isEmpty())
        args << QStringLiteral("-U") << p.uuid;
    args << p.path;
    return run(QStringLiteral("mkswap"), args, d);
}

QString randomExfatSerial()
{
    return QStringLiteral("0x%1").arg(QRandomGenerator::global()->generate(), 8, 16, QLatin1Char('0'));
}

} // namespace

void FileSystemRegistry::addResizeAndMaintenance()
{
    auto def = [this](FsType t) -> FileSystem::Definition * {
        auto it = m_fs.find(t);
        return it == m_fs.end() ? nullptr : &it->second->m_def;
    };
    auto pathArgs = [](std::initializer_list<const char *> before) {
        QStringList b;
        for (const char *a : before)
            b << QString::fromLatin1(a);
        return [b](const Partition &p) { return b + QStringList{ p.path }; };
    };

    // ext2/3/4
    for (FsType t : { FsType::Ext2, FsType::Ext3, FsType::Ext4 }) {
        FileSystem::Definition *d = def(t);
        d->resize = extResize;
        d->growPrograms = { QStringLiteral("resize2fs"), QStringLiteral("tune2fs") };
        d->shrinkPrograms = { QStringLiteral("resize2fs") };
        d->onlineGrow = t != FsType::Ext2;
        d->checkProgram = QStringLiteral("e2fsck");
        d->checkArgs = pathArgs({ "-f", "-y", "-v", "-C", "0" });
        d->checkSuccessCodes = { 0, 1, 2 };
        d->writeLabel = [](const Partition &p, OperationDetail &od) {
            return run(QStringLiteral("e2label"), { p.path, p.fsLabel }, od);
        };
        d->labelPrograms = { QStringLiteral("e2label") };
        d->onlineLabel = true;
        d->writeUuid = [](const Partition &p, OperationDetail &od) {
            return run(QStringLiteral("tune2fs"), { QStringLiteral("-U"), QStringLiteral("random"), p.path }, od);
        };
        d->uuidPrograms = { QStringLiteral("tune2fs") };
        d->moveNeedsCheck = true;
    }

    // fat16/32: resized through libparted-fs-resize
    for (FsType t : { FsType::Fat16, FsType::Fat32 }) {
        FileSystem::Definition *d = def(t);
        d->libpartedGrow = d->libpartedShrink = true;
        d->checkProgram = QStringLiteral("fsck.fat");
        d->checkArgs = pathArgs({ "-a", "-w", "-v" });
        d->checkSuccessCodes = { 0, 1 };
        d->writeLabel = [](const Partition &p, OperationDetail &od) {
            if (p.fsLabel.isEmpty())
                return run(QStringLiteral("mlabel"), { QStringLiteral("-c"), QStringLiteral("-i"), p.path, QStringLiteral("::") }, od);
            return run(QStringLiteral("fatlabel"), { p.path, p.fsLabel }, od);
        };
        d->labelPrograms = { QStringLiteral("fatlabel"), QStringLiteral("mlabel") };
        d->writeUuid = [](const Partition &p, OperationDetail &od) {
            return run(QStringLiteral("mlabel"), { QStringLiteral("-s"), QStringLiteral("-n"), QStringLiteral("-i"), p.path, QStringLiteral("::") }, od);
        };
        d->uuidPrograms = { QStringLiteral("mlabel") };
    }

    if (FileSystem::Definition *d = def(FsType::Ntfs)) {
        d->resize = ntfsResize;
        d->growPrograms = d->shrinkPrograms = { QStringLiteral("ntfsresize") };
        d->checkProgram = QStringLiteral("ntfsresize");
        d->checkArgs = pathArgs({ "-i", "-f", "-v" });
        d->writeLabel = [](const Partition &p, OperationDetail &od) {
            return run(QStringLiteral("ntfslabel"), { QStringLiteral("--force"), p.path, p.fsLabel }, od);
        };
        d->labelPrograms = { QStringLiteral("ntfslabel") };
        d->writeUuid = [](const Partition &p, OperationDetail &od) {
            return run(QStringLiteral("ntfslabel"), { QStringLiteral("--new-serial"), p.path }, od);
        };
        d->uuidPrograms = { QStringLiteral("ntfslabel") };
        d->moveNeedsCheck = true;
    }

    if (FileSystem::Definition *d = def(FsType::Btrfs)) {
        d->resize = btrfsResize;
        d->growPrograms = d->shrinkPrograms = { QStringLiteral("btrfs"), QStringLiteral("mount"), QStringLiteral("umount") };
        d->requireCheckForResize = true;
        d->onlineGrow = d->onlineShrink = true;
        d->checkProgram = QStringLiteral("btrfs");
        d->checkArgs = pathArgs({ "check" });
        d->writeLabel = [](const Partition &p, OperationDetail &od) {
            const QString target = p.busy && !p.mountPoints.isEmpty() ? p.mountPoints.first() : p.path;
            return run(QStringLiteral("btrfs"), { QStringLiteral("filesystem"), QStringLiteral("label"), target, p.fsLabel }, od);
        };
        d->labelPrograms = { QStringLiteral("btrfs") };
        d->onlineLabel = true;
        d->writeUuid = [](const Partition &p, OperationDetail &od) {
            return run(QStringLiteral("btrfstune"), { QStringLiteral("-f"), QStringLiteral("-u"), p.path }, od);
        };
        d->uuidPrograms = { QStringLiteral("btrfstune") };
        d->moveNeedsCheck = true;
    }

    if (FileSystem::Definition *d = def(FsType::Xfs)) {
        // xfs can only grow, while mounted.
        d->resize = [](const Partition &p, OperationDetail &od, bool) {
            return withMountedFileSystem(p, QStringLiteral("xfs"), od, [&](const QString &mp) {
                return run(QStringLiteral("xfs_growfs"), { mp }, od);
            });
        };
        d->growPrograms = { QStringLiteral("xfs_growfs"), QStringLiteral("mount"), QStringLiteral("umount") };
        d->requireCheckForResize = true;
        d->onlineGrow = true;
        d->checkProgram = QStringLiteral("xfs_repair");
        d->checkArgs = pathArgs({ "-v" });
        d->writeLabel = [](const Partition &p, OperationDetail &od) {
            if (p.fsLabel.isEmpty())
                return run(QStringLiteral("xfs_admin"), { QStringLiteral("-L"), QStringLiteral("--"), p.path }, od);
            return run(QStringLiteral("xfs_admin"), { QStringLiteral("-L"), p.fsLabel, p.path }, od);
        };
        d->labelPrograms = { QStringLiteral("xfs_admin") };
        d->writeUuid = [](const Partition &p, OperationDetail &od) {
            return run(QStringLiteral("xfs_admin"), { QStringLiteral("-U"), QStringLiteral("generate"), p.path }, od);
        };
        d->uuidPrograms = { QStringLiteral("xfs_admin") };
        d->moveNeedsCheck = true;
    }

    if (FileSystem::Definition *d = def(FsType::Exfat)) {
        d->checkProgram = QStringLiteral("fsck.exfat");
        d->checkArgs = pathArgs({ "-y", "-v" });
        d->writeLabel = [](const Partition &p, OperationDetail &od) {
            return run(QStringLiteral("tune.exfat"), { QStringLiteral("-L"), p.fsLabel, p.path }, od);
        };
        d->labelPrograms = { QStringLiteral("tune.exfat") };
        d->writeUuid = [](const Partition &p, OperationDetail &od) {
            return run(QStringLiteral("tune.exfat"), { QStringLiteral("-I"), randomExfatSerial(), p.path }, od);
        };
        d->uuidPrograms = { QStringLiteral("tune.exfat") };
    }

    if (FileSystem::Definition *d = def(FsType::F2fs)) {
        d->resize = [](const Partition &p, OperationDetail &od, bool) {
            return run(QStringLiteral("resize.f2fs"), { p.path }, od);  // grow to fill only
        };
        d->growPrograms = { QStringLiteral("resize.f2fs") };
        d->checkProgram = QStringLiteral("fsck.f2fs");
        d->checkArgs = pathArgs({ "-f", "-a" });
    }

    if (FileSystem::Definition *d = def(FsType::Jfs)) {
        d->resize = [](const Partition &p, OperationDetail &od, bool) {
            return withMountedFileSystem(p, QStringLiteral("jfs"), od, [&](const QString &mp) {
                return run(QStringLiteral("mount"), { QStringLiteral("-v"), QStringLiteral("-t"), QStringLiteral("jfs"),
                                                      QStringLiteral("-o"), QStringLiteral("remount,resize"), p.path, mp }, od);
            });
        };
        d->growPrograms = { QStringLiteral("mount"), QStringLiteral("umount") };
        d->requireCheckForResize = true;
        d->onlineGrow = true;
        d->checkProgram = QStringLiteral("jfs_fsck");
        d->checkArgs = pathArgs({ "-f" });
        d->checkSuccessCodes = { 0, 1 };
        d->writeLabel = [](const Partition &p, OperationDetail &od) {
            return run(QStringLiteral("jfs_tune"), { QStringLiteral("-L"), p.fsLabel, p.path }, od);
        };
        d->labelPrograms = { QStringLiteral("jfs_tune") };
        d->writeUuid = [](const Partition &p, OperationDetail &od) {
            return run(QStringLiteral("jfs_tune"), { QStringLiteral("-U"), QStringLiteral("random"), p.path }, od);
        };
        d->uuidPrograms = { QStringLiteral("jfs_tune") };
        d->moveNeedsCheck = true;
    }

    if (FileSystem::Definition *d = def(FsType::LinuxSwap)) {
        d->recreateResize = true;  // resized, moved and copied by recreating it
        d->resize = swapRecreate;
        d->writeLabel = [](const Partition &p, OperationDetail &od) {
            return run(QStringLiteral("swaplabel"), { QStringLiteral("-L"), p.fsLabel, p.path }, od);
        };
        d->labelPrograms = { QStringLiteral("swaplabel") };
        d->writeUuid = [](const Partition &p, OperationDetail &od) {
            return run(QStringLiteral("swaplabel"), { QStringLiteral("-U"), randomUuid(), p.path }, od);
        };
        d->uuidPrograms = { QStringLiteral("swaplabel") };
    }

    if (FileSystem::Definition *d = def(FsType::Nilfs2)) {
        d->resize = [](const Partition &p, OperationDetail &od, bool fill) {
            return withMountedFileSystem(p, QStringLiteral("nilfs2"), od, [&](const QString &) {
                QStringList args{ QStringLiteral("-v"), QStringLiteral("-y"), p.path };
                if (!fill)
                    args << sizeK(p);
                return run(QStringLiteral("nilfs-resize"), args, od);
            });
        };
        d->growPrograms = d->shrinkPrograms = { QStringLiteral("nilfs-resize"), QStringLiteral("mount"), QStringLiteral("umount") };
        d->onlineGrow = d->onlineShrink = true;
        d->writeLabel = [](const Partition &p, OperationDetail &od) {
            return run(QStringLiteral("nilfs-tune"), { QStringLiteral("-L"), p.fsLabel, p.path }, od);
        };
        d->labelPrograms = { QStringLiteral("nilfs-tune") };
        d->writeUuid = [](const Partition &p, OperationDetail &od) {
            return run(QStringLiteral("nilfs-tune"), { QStringLiteral("-U"), randomUuid(), p.path }, od);
        };
        d->uuidPrograms = { QStringLiteral("nilfs-tune") };
    }

    if (FileSystem::Definition *d = def(FsType::Reiserfs)) {
        d->resize = [](const Partition &p, OperationDetail &od, bool fill) {
            QStringList args;
            if (!fill)
                args << QStringLiteral("-s") << QString::number(p.byteLength());
            args << p.path;
            // resize_reiserfs asks for confirmation; it exits 1 after a successful resize too.
            const CommandResult r = runCommand(QStringLiteral("resize_reiserfs"), args, &od, QByteArrayLiteral("y\n"));
            return r.started && (r.exitCode == 0 || r.exitCode == 1);
        };
        d->growPrograms = d->shrinkPrograms = { QStringLiteral("resize_reiserfs") };
        d->requireCheckForResize = true;
        d->onlineGrow = true;
        d->checkProgram = QStringLiteral("reiserfsck");
        d->checkArgs = pathArgs({ "--yes", "--fix-fixable", "--quiet" });
        d->checkSuccessCodes = { 0, 1 };
        d->writeLabel = [](const Partition &p, OperationDetail &od) {
            return run(QStringLiteral("reiserfstune"), { QStringLiteral("--label"), p.fsLabel, p.path }, od);
        };
        d->labelPrograms = { QStringLiteral("reiserfstune") };
        d->writeUuid = [](const Partition &p, OperationDetail &od) {
            return run(QStringLiteral("reiserfstune"), { QStringLiteral("-u"), QStringLiteral("random"), p.path }, od);
        };
        d->uuidPrograms = { QStringLiteral("reiserfstune") };
        d->moveNeedsCheck = true;
    }

    if (FileSystem::Definition *d = def(FsType::Reiser4)) {
        d->checkProgram = QStringLiteral("fsck.reiser4");
        d->checkArgs = pathArgs({ "--yes", "--fix", "--quiet" });
        d->moveNeedsCheck = true;
    }

    if (FileSystem::Definition *d = def(FsType::Lvm2Pv)) {
        d->resize = [](const Partition &p, OperationDetail &od, bool fill) {
            QStringList args{ QStringLiteral("pvresize"), QStringLiteral("-v") };
            if (!fill)
                args << QStringLiteral("--yes") << QStringLiteral("--setphysicalvolumesize") << sizeK(p);
            args << p.path;
            return run(QStringLiteral("lvm"), args, od);
        };
        d->growPrograms = d->shrinkPrograms = { QStringLiteral("lvm") };
        d->onlineGrow = d->onlineShrink = true;
        d->checkProgram = QStringLiteral("lvm");
        d->checkArgs = pathArgs({ "pvck", "-v" });
        d->copyable = false;  // duplicate PV UUIDs would confuse LVM
    }

    if (FileSystem::Definition *d = def(FsType::Udf)) {
        d->writeLabel = [](const Partition &p, OperationDetail &od) {
            return run(QStringLiteral("udflabel"), { QStringLiteral("--utf8"), p.path, p.fsLabel }, od);
        };
        d->labelPrograms = { QStringLiteral("udflabel") };
        d->writeUuid = [](const Partition &p, OperationDetail &od) {
            return run(QStringLiteral("udflabel"), { QStringLiteral("--utf8"), QStringLiteral("--uuid=random"), p.path }, od);
        };
        d->uuidPrograms = { QStringLiteral("udflabel") };
    }

    for (FsType t : { FsType::Hfs, FsType::HfsPlus }) {
        FileSystem::Definition *d = def(t);
        d->libpartedShrink = true;
        d->readUsage = [](Partition &p) { Parted::libpartedFsUsage(p); };
        d->checkProgram = t == FsType::Hfs ? QStringLiteral("hfsck") : QStringLiteral("fsck.hfsplus");
        d->checkArgs = t == FsType::Hfs ? pathArgs({ "-v" }) : pathArgs({ "-f", "-y" });
    }

    if (FileSystem::Definition *d = def(FsType::Minix)) {
        d->checkProgram = QStringLiteral("fsck.minix");
        d->checkArgs = pathArgs({});
    }

    if (FileSystem::Definition *d = def(FsType::Bcachefs)) {
        d->resize = [](const Partition &p, OperationDetail &od, bool) {
            return run(QStringLiteral("bcachefs"), { QStringLiteral("device"), QStringLiteral("resize"), p.path }, od);
        };
        d->growPrograms = { QStringLiteral("bcachefs") };
        d->onlineGrow = true;
        d->checkProgram = QStringLiteral("bcachefs");
        d->checkArgs = pathArgs({ "fsck", "-f", "-y", "-v" });
    }
}

} // namespace QtParted

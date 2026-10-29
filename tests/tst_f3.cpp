/*
 * QtParted - tests for LUKS, mount/unmount, swapon/swapoff and Volume Group actions.
 * Copyright (C) 2026 Vanni Brutto <zanac4ever@gmail.com>
 *
 * The LUKS headers and passphrases are real.  Kernels without device-mapper (containers)
 * can't open them, so the end to end tests run with the fakecrypt test double first in
 * PATH: an "open" volume is a loop device at the LUKS data offset, and the real file
 * system tools work inside it.  Needs root; skipped otherwise.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "BusyActions.h"
#include "Command.h"
#include "Layout.h"
#include "Luks.h"
#include "OperationDetail.h"
#include "Parted.h"
#include "Session.h"
#include "SystemInfo.h"
#include "Worker.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <sys/sysmacros.h>
#include <unistd.h>

using namespace QtParted;

namespace {

const QByteArray kPass = "correct horse";

bool run(const QString &program, const QStringList &args, const QByteArray &input = QByteArray())
{
    const CommandResult r = runCommand(program, args, nullptr, input);
    if (!r.ok())
        qWarning().noquote() << program << args.join(QLatin1Char(' ')) << "->" << r.exitCode << r.error.trimmed()
                             << r.output.right(400);
    return r.ok();
}

Device scanOne(const QString &path)
{
    const std::vector<Device> d = scanDevices({ path });
    return d.size() == 1 ? d.front() : Device();
}

const Partition *byNumber(const Device &d, int number)
{
    for (const Partition &p : d.partitions) {
        if (p.number == number && p.isPartitionInTable())
            return &p;
        for (const Partition &l : p.logicals)
            if (l.number == number && l.type == PartitionType::Logical)
                return &l;
    }
    return nullptr;
}

bool applyAll(Session &s)
{
    std::vector<Operation *> ops;
    for (auto &op : s.operations)
        ops.push_back(op.get());
    OperationApplier applier(ops);
    bool result = false;
    QObject::connect(&applier, &OperationApplier::finished, [&](bool ok, int) { result = ok; });
    applier.run();
    if (!result)
        for (auto &op : s.operations)
            qWarning().noquote() << op->detail().toPlainText();
    s.clear();
    return result;
}

bool queueResize(Session &s, int number, qint64 deltaBeforeMiB, qint64 sizeMiB)
{
    const Device disp = s.displayDevice(0);
    const Partition *p = byNumber(disp, number);
    if (!p)
        return false;
    const Layout::SpaceEdit space = Layout::resizeMoveSpace(disp, *p);
    const qint64 before = space.beforeMiB + deltaBeforeMiB;
    const qint64 after = space.totalMiB - before - sizeMiB;
    if (before < space.minBeforeMiB || before > space.maxBeforeMiB || sizeMiB < space.minSizeMiB
        || sizeMiB > space.maxSizeMiB || after < 0) {
        qWarning() << "outside limits" << before << sizeMiB << after << "limits" << space.minBeforeMiB
                   << space.maxBeforeMiB << space.minSizeMiB << space.maxSizeMiB << space.totalMiB;
        return false;
    }
    const Partition n = Layout::applyResizeMove(disp, *p, space, before, sizeMiB, after, Alignment::MiB);
    QString error;
    const bool ok = s.addOperation(std::make_unique<OperationResizeMove>(s.devices[0], *p, n), &error);
    if (!ok)
        qWarning() << error;
    return ok;
}

QByteArray md5(const QString &file)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly))
        return "missing";
    return QCryptographicHash::hash(f.readAll(), QCryptographicHash::Md5).toHex();
}

} // namespace

class TestF3 : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void cleanup();
    void dmsetupTableParsing();
    void allowedActionsModel();
    void luksOpenScanAndClose();
    void openLuksResizeShrinkAndGrow();
    void openLuksCheckLabelUuidFormat();
    void closedLuksMoveAndGrow();
    void copyOpenLuksContent();
    void mountUnmountAndSwap();

private:
    bool attachLuks(qint64 imageMiB, qint64 partEndMiB);
    QString part(int n) const { return m_loop + QStringLiteral("p") + QString::number(n); }
    QByteArray writeData(const QString &dev, int mib);
    QByteArray readData(const QString &dev);
    bool openLuks();

    QTemporaryDir m_dir;
    QString m_loop;
    QString m_mnt;
    bool m_root = false;
};

void TestF3::initTestCase()
{
    QVERIFY(m_dir.isValid());
    m_mnt = m_dir.filePath(QStringLiteral("mnt"));
    QVERIFY(QDir().mkpath(m_mnt));
    m_root = geteuid() == 0 && Tools::has(QStringLiteral("losetup")) && Tools::has(QStringLiteral("parted"));
    // The fakecrypt double goes first in PATH unless the kernel has device-mapper.
    if (!QFileInfo::exists(QStringLiteral("/sys/module/dm_mod"))) {
        const QString fake = QStringLiteral(QTPARTED_FAKECRYPT_DIR);
        qputenv("PATH", (fake + QLatin1Char(':') + qEnvironmentVariable("PATH")).toLocal8Bit());
        qputenv("FAKECRYPT_STATE", m_dir.filePath(QStringLiteral("fakecrypt")).toLocal8Bit());
        Tools::clearCache();
    }
    Parted::initialize();
    FileSystemRegistry::instance().detect();
}

void TestF3::cleanup()
{
    runCommand(QStringLiteral("umount"), { m_mnt });
    if (!m_loop.isEmpty()) {
        Luks::loadCache();
        const LuksMapping m = Luks::mapping(part(1));
        if (m.isOpen())
            runCommand(QStringLiteral("cryptsetup"), { QStringLiteral("luksClose"), m.name });
        runCommand(QStringLiteral("swapoff"), { part(2) });
        runCommand(QStringLiteral("losetup"), { QStringLiteral("-d"), m_loop });
        m_loop.clear();
    }
    PasswordStore::clear();
}

bool TestF3::attachLuks(qint64 imageMiB, qint64 partEndMiB)
{
    if (!m_root || !Tools::has(QStringLiteral("cryptsetup")))
        return false;
    const QString image = m_dir.filePath(QStringLiteral("luks.img"));
    QFile::remove(image);
    QFile f(image);
    if (!f.open(QIODevice::WriteOnly) || !f.resize(imageMiB * MiB))
        return false;
    f.close();
    const CommandResult r = runCommand(QStringLiteral("losetup"), { QStringLiteral("-fP"), QStringLiteral("--show"), image });
    m_loop = r.output.trimmed();
    if (!r.ok() || m_loop.isEmpty())
        return false;
    if (!run(QStringLiteral("parted"), { QStringLiteral("-s"), m_loop, QStringLiteral("mklabel"), QStringLiteral("gpt"),
                                         QStringLiteral("mkpart"), QStringLiteral("secret"), QStringLiteral("1MiB"),
                                         QString::number(partEndMiB) + QStringLiteral("MiB") }))
        return false;
    Parted::settle();
    // Fast key derivation: the test opens the volume many times.
    return run(QStringLiteral("cryptsetup"),
               { QStringLiteral("luksFormat"), QStringLiteral("-q"), QStringLiteral("--type"), QStringLiteral("luks2"),
                 QStringLiteral("--pbkdf"), QStringLiteral("pbkdf2"), QStringLiteral("--pbkdf-force-iterations"),
                 QStringLiteral("1000"), part(1), QStringLiteral("-") },
               kPass);
}

bool TestF3::openLuks()
{
    Partition p;
    p.path = part(1);
    p.uuid = runCommand(QStringLiteral("blkid"), { QStringLiteral("-s"), QStringLiteral("UUID"), QStringLiteral("-o"),
                                                   QStringLiteral("value"), part(1) }).output.trimmed();
    QString error;
    const bool ok = Luks::open(p, kPass, error);
    if (!ok)
        qWarning() << error;
    return ok;
}

QByteArray TestF3::writeData(const QString &dev, int mib)
{
    if (!run(QStringLiteral("mount"), { dev, m_mnt }))
        return QByteArray();
    const QString file = m_mnt + QStringLiteral("/data.bin");
    run(QStringLiteral("dd"), { QStringLiteral("if=/dev/urandom"), QStringLiteral("of=") + file, QStringLiteral("bs=1M"),
                                QStringLiteral("count=") + QString::number(mib), QStringLiteral("status=none") });
    const QByteArray hash = md5(file);
    run(QStringLiteral("umount"), { m_mnt });
    return hash;
}

QByteArray TestF3::readData(const QString &dev)
{
    if (!run(QStringLiteral("mount"), { QStringLiteral("-o"), QStringLiteral("ro"), dev, m_mnt }))
        return "mount failed";
    const QByteArray hash = md5(m_mnt + QStringLiteral("/data.bin"));
    run(QStringLiteral("umount"), { m_mnt });
    return hash;
}

void TestF3::dmsetupTableParsing()
{
    const QString out = QStringLiteral(
        "sda2_crypt: 0 2093056 crypt aes-xts-plain64 :64:logon:cryptsetup:3f2e-d0 0 8:2 32768\n"
        "old_crypt: 0 409600 crypt aes-cbc-essiv:sha256 6ae1f14b0d7c 0 /dev/sdb1 4096 1 allow_discards\n"
        "linear: 0 100 linear 8:3 0\n"
        "broken: 0 0 crypt aes 00 0 8:4 0\n");
    const QList<Luks::CryptTarget> t = Luks::parseDmsetupTable(out);
    QCOMPARE(t.size(), 2);
    QCOMPARE(t[0].mapping.name, QStringLiteral("sda2_crypt"));
    QCOMPARE(t[0].containerDev, quint64(makedev(8, 2)));
    QCOMPARE(t[0].mapping.offset, ByteValue(32768) * 512);
    QCOMPARE(t[0].mapping.length, ByteValue(2093056) * 512);
    QCOMPARE(t[0].mapping.keyLocation, LuksMapping::KeyLocation::KeyRing);
    QCOMPARE(t[1].containerPath, QStringLiteral("/dev/sdb1"));
    QCOMPARE(t[1].mapping.keyLocation, LuksMapping::KeyLocation::DmCrypt);
    QCOMPARE(t[1].mapping.path(), QStringLiteral("/dev/mapper/old_crypt"));
    QVERIFY(Luks::parseDmsetupTable(QStringLiteral("No devices found\n")).isEmpty());
    QCOMPARE(Luks::mappingNameFor(QStringLiteral("/dev/nvme0n1p3")), QStringLiteral("nvme0n1p3_crypt"));
}

void TestF3::allowedActionsModel()
{
    // An open LUKS partition with an unmounted ext4 inside: resize, check, label, UUID,
    // format and copy of the content; no delete, no move.
    Device d;
    d.path = QStringLiteral("/dev/fake");
    d.length = 1024 * MiB / 512;
    d.diskType = QStringLiteral("gpt");
    d.maxPrimaries = 128;
    d.maxNameLength = 36;
    Partition p;
    p.devicePath = d.path;
    p.path = QStringLiteral("/dev/fake1");
    p.number = 1;
    p.type = PartitionType::Primary;
    p.fsType = FsType::Luks;
    p.start = 2048;
    p.end = 2048 + 400 * MiB / 512 - 1;
    p.busy = true;
    p.luks.name = QStringLiteral("fake1_crypt");
    p.luks.offset = 16 * MiB;
    p.luks.length = 384 * MiB;
    p.luksHeader = 16 * MiB / 512;
    p.setUsage(p.length(), 0);
    Partition in;
    in.devicePath = in.path = p.luks.path();
    in.type = PartitionType::Unpartitioned;
    in.fsType = FsType::Ext4;
    in.start = 0;
    in.end = 384 * MiB / 512 - 1;
    in.setUsage(in.length(), in.length() - 100 * MiB / 512);
    in.mountPoints = { QStringLiteral("/srv") };  // from fstab
    p.encrypted.emplace(in);
    d.partitions = { p };
    insertUnallocated(d.path, d.partitions, 0, d.length - 1, 512, false);
    const Partition &sel = d.partitions[0].type == PartitionType::Unallocated ? d.partitions[1] : d.partitions[0];

    const FsSupport ext4 = FileSystemRegistry::instance().support(FsType::Ext4);
    const bool cs = Tools::has(QStringLiteral("cryptsetup"));
    AllowedActions a = Session::allowedActions(d, &sel, 0);
    QVERIFY(a.openCrypt && a.cryptIsOpen);
    QVERIFY(!a.remove);
    QCOMPARE(a.check, ext4.check);
    QCOMPARE(a.label, ext4.writeLabel);
    QCOMPARE(a.uuid, ext4.writeUuid);
    QVERIFY(a.format);
    QCOMPARE(a.copy, true);
    QCOMPARE(a.resize, cs && ext4.canResize());
    QCOMPARE(a.mountOn, QStringList{ QStringLiteral("/srv") });
    QCOMPARE(sel.filesystem().fsType, FsType::Ext4);
    QCOMPARE(sel.usedSectors(), sel.luksHeader + 100 * MiB / 512);
    QCOMPARE(sel.unusedSectors(), in.length() - 100 * MiB / 512);
    if (cs) {
        const Layout::SpaceEdit space = Layout::resizeMoveSpace(d, sel);
        QVERIFY(space.fixedStart);
        QVERIFY(space.minSizeMiB >= 116 && space.minSizeMiB < 130);  // header + used + margin
        // The shrunk partition keeps the header: the mapping follows the partition.
        const Partition n = Layout::applyResizeMove(d, sel, space, space.beforeMiB, 200,
                                                    space.totalMiB - space.beforeMiB - 200, Alignment::MiB);
        QCOMPARE(n.encrypted->length(), n.length() - n.luksHeader);
        QCOMPARE(n.encrypted->sectorsUsed, Sector(100 * MiB / 512));
        QVERIFY(Session::validPartition(d, n, nullptr));
    }

    // The file system inside mounted: only unmount and online actions; no close.
    Partition mounted = sel;
    mounted.encrypted->busy = true;
    a = Session::allowedActions(d, &mounted, 0);
    QCOMPARE(a.toggle, AllowedActions::Toggle::Unmount);
    QVERIFY(!a.openCrypt && !a.format && !a.check && !a.copy);

    // Closed: open, delete, format, move/grow; no copy (content only while open), no shrink.
    Partition closed = sel;
    closed.busy = false;
    closed.encrypted.reset();
    closed.luks = LuksMapping();
    a = Session::allowedActions(d, &closed, 0);
    QVERIFY(a.openCrypt && !a.cryptIsOpen);
    QVERIFY(a.remove && a.format && a.resize && !a.copy && !a.check);
    const Layout::SpaceEdit cspace = Layout::resizeMoveSpace(d, closed);
    QCOMPARE(cspace.minSizeMiB, qint64(400));
    QVERIFY(!cspace.fixedStart);

    // Swap and LVM PV toggles.
    Partition swap = closed;
    swap.fsType = FsType::LinuxSwap;
    QCOMPARE(Session::allowedActions(d, &swap, 0).toggle, AllowedActions::Toggle::SwapOn);
    swap.busy = true;
    QCOMPARE(Session::allowedActions(d, &swap, 0).toggle, AllowedActions::Toggle::SwapOff);
    Partition pv = closed;
    pv.fsType = FsType::Lvm2Pv;
    QCOMPARE(Session::allowedActions(d, &pv, 0).toggle, AllowedActions::Toggle::None);  // not in a VG
    pv.mountPoints = { QStringLiteral("vg0") };
    QCOMPARE(Session::allowedActions(d, &pv, 0).toggle, AllowedActions::Toggle::ActivateVg);
    pv.busy = true;
    QCOMPARE(Session::allowedActions(d, &pv, 0).toggle, AllowedActions::Toggle::DeactivateVg);

    // Pending operations on a partition are counted for the busy state actions.
    Session s;
    s.devices = { d };
    QVERIFY(s.addOperation(std::make_unique<OperationCheck>(d, sel)));
    QCOMPARE(s.pendingCountFor(sel), 1);
    QCOMPARE(s.pendingCountFor(closed.filesystem()), 1);
}

void TestF3::luksOpenScanAndClose()
{
    if (!attachLuks(512, 301))
        QSKIP("needs root, losetup, parted and cryptsetup");
    Partition p;
    p.path = part(1);
    p.uuid = QStringLiteral("test-uuid");
    QString error;
    QVERIFY(!Luks::open(p, "wrong", error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!Luks::open(p, QByteArray(), error));
    QVERIFY(Luks::testPassphrase(p, kPass));
    QVERIFY(!Luks::testPassphrase(p, "wrong"));
    QVERIFY(Luks::open(p, kPass, error));
    QCOMPARE(PasswordStore::lookup(QStringLiteral("test-uuid")), kPass);
    const QString mapper = QStringLiteral("/dev/mapper/") + Luks::mappingNameFor(part(1));
    QVERIFY(run(QStringLiteral("mkfs.ext4"), { QStringLiteral("-q"), QStringLiteral("-L"), QStringLiteral("inside"), mapper }));
    const QByteArray hash = writeData(mapper, 30);

    Device d = scanOne(m_loop);
    const Partition *s1 = byNumber(d, 1);
    QVERIFY(s1);
    QCOMPARE(s1->fsType, FsType::Luks);
    QVERIFY(s1->isOpenLuks());
    QCOMPARE(s1->luks.name, Luks::mappingNameFor(part(1)));
    QCOMPARE(s1->luksHeader * 512, s1->luks.offset);
    QCOMPARE(s1->filesystem().fsType, FsType::Ext4);
    QCOMPARE(s1->filesystem().fsLabel, QStringLiteral("inside"));
    QCOMPARE(s1->filesystem().path, mapper);
    QVERIFY(s1->usageKnown());
    QVERIFY(s1->usedSectors() > s1->luksHeader + 30 * MiB / 512);
    QVERIFY(!s1->filesystem().busy);

    // Mounted inside: the partition is busy through it.
    QVERIFY(run(QStringLiteral("mount"), { mapper, m_mnt }));
    d = scanOne(m_loop);
    QVERIFY(byNumber(d, 1)->filesystem().busy);
    QCOMPARE(byNumber(d, 1)->filesystem().mountPoints, QStringList{ m_mnt });
    QVERIFY(!Session::allowedActions(d, byNumber(d, 1), 0).openCrypt);
    run(QStringLiteral("umount"), { m_mnt });

    d = scanOne(m_loop);
    QVERIFY(Luks::close(*byNumber(d, 1), error));
    d = scanOne(m_loop);
    QCOMPARE(byNumber(d, 1)->fsType, FsType::Luks);
    QVERIFY(!byNumber(d, 1)->busy);
    QVERIFY(!byNumber(d, 1)->encrypted);
    QCOMPARE(byNumber(d, 1)->usedSectors(), byNumber(d, 1)->length());

    QVERIFY(openLuks());
    QCOMPARE(readData(mapper), hash);
}

void TestF3::openLuksResizeShrinkAndGrow()
{
    if (!attachLuks(768, 301))
        QSKIP("needs root, losetup, parted and cryptsetup");
    QVERIFY(openLuks());
    const QString mapper = QStringLiteral("/dev/mapper/") + Luks::mappingNameFor(part(1));
    QVERIFY(run(QStringLiteral("mkfs.ext4"), { QStringLiteral("-q"), mapper }));
    const QByteArray hash = writeData(mapper, 40);

    Session s;
    s.devices = { scanOne(m_loop) };
    const Partition before = *byNumber(s.devices[0], 1);
    QVERIFY(before.isOpenLuks());
    PasswordStore::store(before.uuid, kPass);  // as the open dialog does

    // Shrink: file system, then encryption mapping, then partition.
    QVERIFY(queueResize(s, 1, 0, 150));
    QVERIFY(applyAll(s));
    s.devices = { scanOne(m_loop) };
    const Partition *p = byNumber(s.devices[0], 1);
    QCOMPARE(p->length(), Sector(150 * MiB / 512));
    QVERIFY(p->isOpenLuks());
    QCOMPARE(p->luks.offset + p->luks.length, p->byteLength());  // the mapping fills it
    QCOMPARE(readData(mapper), hash);
    QVERIFY(run(QStringLiteral("e2fsck"), { QStringLiteral("-f"), QStringLiteral("-n"), mapper }));

    // Grow: partition, encryption mapping, then file system to fill.
    QVERIFY(queueResize(s, 1, 0, 500));
    QVERIFY(applyAll(s));
    s.devices = { scanOne(m_loop) };
    p = byNumber(s.devices[0], 1);
    QCOMPARE(p->length(), Sector(500 * MiB / 512));
    QCOMPARE(p->luks.offset + p->luks.length, p->byteLength());
    QVERIFY(p->unallocatedSectors() == 0);
    QVERIFY(p->filesystem().sectorsUnallocated < Sector(4 * MiB / 512));
    QCOMPARE(readData(mapper), hash);

    // Without the passphrase (volume key in the keyring) the encryption can't be
    // resized: the grow stops after the partition, nothing is lost.
    PasswordStore::clear();
    QVERIFY(queueResize(s, 1, 0, 600));
    QVERIFY(!applyAll(s));
    s.devices = { scanOne(m_loop) };
    QCOMPARE(readData(mapper), hash);
    QVERIFY(run(QStringLiteral("e2fsck"), { QStringLiteral("-f"), QStringLiteral("-n"), mapper }));
}

void TestF3::openLuksCheckLabelUuidFormat()
{
    if (!attachLuks(512, 301))
        QSKIP("needs root, losetup, parted and cryptsetup");
    QVERIFY(openLuks());
    const QString mapper = QStringLiteral("/dev/mapper/") + Luks::mappingNameFor(part(1));
    QVERIFY(run(QStringLiteral("mkfs.ext4"), { QStringLiteral("-q"), mapper }));
    const QByteArray hash = writeData(mapper, 10);
    Session s;
    s.devices = { scanOne(m_loop) };
    const Partition p = *byNumber(s.devices[0], 1);
    const QString oldUuid = p.filesystem().uuid;
    const QString luksUuid = p.uuid;
    QVERIFY(s.addOperation(std::make_unique<OperationLabel>(s.devices[0], p, QStringLiteral("cifrato"))));
    QCOMPARE(s.displayDevice(0).partitions[0].type == PartitionType::Unallocated
                 ? s.displayDevice(0).partitions[1].filesystem().fsLabel
                 : s.displayDevice(0).partitions[0].filesystem().fsLabel,
             QStringLiteral("cifrato"));
    QVERIFY(s.addOperation(std::make_unique<OperationChangeUuid>(s.devices[0], *byNumber(s.displayDevice(0), 1))));
    QVERIFY(s.addOperation(std::make_unique<OperationCheck>(s.devices[0], *byNumber(s.displayDevice(0), 1))));
    QVERIFY(applyAll(s));
    s.devices = { scanOne(m_loop) };
    const Partition *q = byNumber(s.devices[0], 1);
    QCOMPARE(q->filesystem().fsLabel, QStringLiteral("cifrato"));
    QVERIFY(q->filesystem().uuid != oldUuid);
    QCOMPARE(q->uuid, luksUuid);  // the LUKS header is untouched
    QCOMPARE(readData(mapper), hash);

    // Format inside the open encryption: the LUKS header stays.
    Partition np = *q;
    np.filesystem().fsType = FsType::Ext2;
    np.filesystem().fsLabel.clear();
    np.filesystem().clearUsage();
    np.status = PartitionStatus::Formatted;
    QVERIFY(s.addOperation(std::make_unique<OperationFormat>(s.devices[0], *q, np)));
    QVERIFY(s.operations.back()->description().contains(QLatin1String("ext2")));
    QVERIFY(applyAll(s));
    s.devices = { scanOne(m_loop) };
    q = byNumber(s.devices[0], 1);
    QCOMPARE(q->fsType, FsType::Luks);
    QCOMPARE(q->uuid, luksUuid);
    QCOMPARE(q->filesystem().fsType, FsType::Ext2);
}

void TestF3::closedLuksMoveAndGrow()
{
    if (!attachLuks(768, 201))
        QSKIP("needs root, losetup, parted and cryptsetup");
    QVERIFY(openLuks());
    const QString mapper = QStringLiteral("/dev/mapper/") + Luks::mappingNameFor(part(1));
    QVERIFY(run(QStringLiteral("mkfs.ext4"), { QStringLiteral("-q"), mapper }));
    const QByteArray hash = writeData(mapper, 20);
    Session s;
    s.devices = { scanOne(m_loop) };
    QString error;
    QVERIFY(Luks::close(*byNumber(s.devices[0], 1), error));
    s.devices = { scanOne(m_loop) };
    QVERIFY(!byNumber(s.devices[0], 1)->busy);
    // Shrinking closed encryption is not offered.
    const Device disp = s.displayDevice(0);
    QCOMPARE(Layout::resizeMoveSpace(disp, *byNumber(disp, 1)).minSizeMiB, qint64(200));
    // Move right by 100 MiB and grow to 300 MiB.
    QVERIFY(queueResize(s, 1, 100, 300));
    QVERIFY(applyAll(s));
    s.devices = { scanOne(m_loop) };
    QCOMPARE(byNumber(s.devices[0], 1)->start, Sector(101 * MiB / 512));
    QCOMPARE(byNumber(s.devices[0], 1)->length(), Sector(300 * MiB / 512));
    // Opened again: the header moved along, the mapping fills the larger partition.
    QVERIFY(openLuks());
    s.devices = { scanOne(m_loop) };
    const Partition *p = byNumber(s.devices[0], 1);
    QVERIFY(p->isOpenLuks());
    QCOMPARE(p->luks.offset + p->luks.length, p->byteLength());
    QCOMPARE(readData(mapper), hash);
}

void TestF3::copyOpenLuksContent()
{
    if (!attachLuks(768, 201))
        QSKIP("needs root, losetup, parted and cryptsetup");
    QVERIFY(openLuks());
    const QString mapper = QStringLiteral("/dev/mapper/") + Luks::mappingNameFor(part(1));
    QVERIFY(run(QStringLiteral("mkfs.ext4"), { QStringLiteral("-q"), mapper }));
    const QByteArray hash = writeData(mapper, 20);
    Session s;
    s.devices = { scanOne(m_loop) };
    const Partition src = *byNumber(s.devices[0], 1);
    const Device disp = s.displayDevice(0);
    const Partition *un = nullptr;
    for (const Partition &p : disp.partitions)
        if (p.type == PartitionType::Unallocated && p.start > src.end)
            un = &p;
    QVERIFY(un);
    QVERIFY(Session::allowedActions(disp, &src, 0).copy);
    QVERIFY(Session::allowedActions(disp, un, 0, &src).paste);
    const Partition content = src.filesystem();
    const Layout::SpaceEdit space = Layout::pasteSpace(*un, content);
    const Partition np = Layout::makePastedPartition(disp, *un, content, space, space.beforeMiB, 250,
                                                     space.totalMiB - space.beforeMiB - 250, Alignment::MiB,
                                                     s.nextNewNumber());
    QCOMPARE(np.fsType, FsType::Ext4);
    QVERIFY(!np.encrypted);
    QVERIFY(s.addOperation(std::make_unique<OperationCopy>(s.devices[0], *un, np, src)));
    QVERIFY(applyAll(s));
    s.devices = { scanOne(m_loop) };
    const Partition *copy = byNumber(s.devices[0], 2);
    QVERIFY(copy);
    QCOMPARE(copy->fsType, FsType::Ext4);  // a plain, decrypted copy
    QCOMPARE(readData(part(2)), hash);
    QVERIFY(byNumber(s.devices[0], 1)->isOpenLuks());  // the source is untouched
}

void TestF3::mountUnmountAndSwap()
{
    if (!m_root)
        QSKIP("needs root, losetup and parted");
    const QString image = m_dir.filePath(QStringLiteral("plain.img"));
    QFile f(image);
    QVERIFY(f.open(QIODevice::WriteOnly) && f.resize(400 * MiB));
    f.close();
    m_loop = runCommand(QStringLiteral("losetup"), { QStringLiteral("-fP"), QStringLiteral("--show"), image }).output.trimmed();
    QVERIFY(!m_loop.isEmpty());
    QVERIFY(run(QStringLiteral("parted"), { QStringLiteral("-s"), m_loop, QStringLiteral("mklabel"), QStringLiteral("gpt"),
                                            QStringLiteral("mkpart"), QStringLiteral("a"), QStringLiteral("1MiB"), QStringLiteral("101MiB"),
                                            QStringLiteral("mkpart"), QStringLiteral("b"), QStringLiteral("101MiB"), QStringLiteral("201MiB") }));
    Parted::settle();
    QVERIFY(run(QStringLiteral("mkfs.ext4"), { QStringLiteral("-q"), QStringLiteral("-L"), QStringLiteral("f3mnt"), part(1) }));
    QVERIFY(run(QStringLiteral("mkswap"), { part(2) }));

    // An fstab entry offers "Mount on".
    QFile fstab(QStringLiteral("/etc/fstab"));
    QVERIFY(fstab.open(QIODevice::ReadOnly));
    const QByteArray saved = fstab.readAll();
    fstab.close();
    auto restore = qScopeGuard([&]() {
        QFile w(QStringLiteral("/etc/fstab"));
        if (w.open(QIODevice::WriteOnly | QIODevice::Truncate))
            w.write(saved);
    });
    {
        QFile w(QStringLiteral("/etc/fstab"));
        QVERIFY(w.open(QIODevice::Append));
        w.write(("LABEL=f3mnt " + m_mnt.toLocal8Bit() + " ext4 defaults 0 2\n"));
    }
    Device d = scanOne(m_loop);
    AllowedActions a = Session::allowedActions(d, byNumber(d, 1), 0);
    QCOMPARE(a.mountOn, QStringList{ m_mnt });
    QCOMPARE(a.toggle, AllowedActions::Toggle::None);
    QString error;
    QVERIFY2(BusyActions::mount(*byNumber(d, 1), m_mnt, error), qPrintable(error));
    d = scanOne(m_loop);
    QVERIFY(byNumber(d, 1)->busy);
    a = Session::allowedActions(d, byNumber(d, 1), 0);
    QCOMPARE(a.toggle, AllowedActions::Toggle::Unmount);
    QVERIFY(a.mountOn.isEmpty());

    // Stacked mounts on the same directory are refused, like GParted.
    QVERIFY(run(QStringLiteral("mount"), { QStringLiteral("-t"), QStringLiteral("tmpfs"), QStringLiteral("none"), m_mnt }));
    QVERIFY(!BusyActions::unmount(*byNumber(d, 1), error));
    QVERIFY(error.contains(m_mnt));
    QVERIFY(run(QStringLiteral("umount"), { m_mnt }));  // the tmpfs on top
    QVERIFY2(BusyActions::unmount(*byNumber(d, 1), error), qPrintable(error));
    d = scanOne(m_loop);
    QVERIFY(!byNumber(d, 1)->busy);

    // Mounting something that isn't there reports the commands that failed.
    QVERIFY(!BusyActions::mount(*byNumber(d, 1), m_dir.filePath(QStringLiteral("missing")), error));
    QVERIFY(error.contains(QLatin1String("# mount")));

    // Swap.
    a = Session::allowedActions(d, byNumber(d, 2), 0);
    QCOMPARE(a.toggle, AllowedActions::Toggle::SwapOn);
    if (!BusyActions::swapOn(*byNumber(d, 2), error))
        QSKIP(qPrintable(QStringLiteral("swapon not permitted here: ") + error));
    d = scanOne(m_loop);
    QVERIFY(byNumber(d, 2)->busy);
    QCOMPARE(Session::allowedActions(d, byNumber(d, 2), 0).toggle, AllowedActions::Toggle::SwapOff);
    QVERIFY2(BusyActions::swapOff(*byNumber(d, 2), error), qPrintable(error));
    d = scanOne(m_loop);
    QVERIFY(!byNumber(d, 2)->busy);
}

QTEST_GUILESS_MAIN(TestF3)
#include "tst_f3.moc"

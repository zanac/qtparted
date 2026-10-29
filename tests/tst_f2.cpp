/*
 * QtParted - end to end tests for resize, move, copy, check, label and UUID.
 * Copyright (C) 2026 Vanni Brutto <zanac4ever@gmail.com>
 *
 * Every test writes data into the file system and verifies its checksum after the
 * operations.  Needs root (loop devices, mount); tests are skipped otherwise.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "Command.h"
#include "Layout.h"
#include "OperationDetail.h"
#include "Parted.h"
#include "Session.h"
#include "Worker.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <unistd.h>

using namespace QtParted;

namespace {

bool run(const QString &program, const QStringList &args)
{
    const CommandResult r = runCommand(program, args);
    if (!r.ok())
        qWarning().noquote() << program << args.join(QLatin1Char(' ')) << "->" << r.exitCode << r.error.trimmed() << r.output.right(600);
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

// Queue a resize/move of partition `number` the way the dialog does: the free space
// before changes by `deltaBeforeMiB`, the size becomes `sizeMiB`.
bool queueResizeMove(Session &s, int number, qint64 deltaBeforeMiB, qint64 sizeMiB, Partition *result = nullptr)
{
    const Device disp = s.displayDevice(0);
    const Partition *p = byNumber(disp, number);
    if (!p)
        return false;
    const Layout::SpaceEdit space = Layout::resizeMoveSpace(disp, *p);
    const qint64 before = space.beforeMiB + deltaBeforeMiB;
    const qint64 after = space.totalMiB - before - sizeMiB;
    if (before < space.minBeforeMiB || sizeMiB < space.minSizeMiB || sizeMiB > space.maxSizeMiB || after < 0) {
        qWarning() << "resize outside limits" << before << sizeMiB << after << "limits" << space.minBeforeMiB
                   << space.minSizeMiB << space.maxSizeMiB << space.totalMiB;
        return false;
    }
    const Partition n = Layout::applyResizeMove(disp, *p, space, before, sizeMiB, after, Alignment::MiB);
    if (result)
        *result = n;
    QString error;
    const bool ok = s.addOperation(std::make_unique<OperationResizeMove>(s.devices[0], *p, n), &error);
    if (!ok)
        qWarning() << error;
    return ok;
}

} // namespace

class TestF2 : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void cleanup();
    void copyBlocksOverlap();
    void ext4ResizeAndMove();
    void logicalMoveKeepsNeighbours();
    void fatResizeWithLibparted();
    void ntfsResizeMoveBootSector();
    void swapResizeKeepsIdentity();
    void btrfsAndXfsGrow();
    void pasteLabelUuidCheck();
    void onlineGrow();
    void extendedResize();

private:
    bool attach(const QString &image, qint64 bytes);
    QString part(int n) const { return m_loop + QStringLiteral("p") + QString::number(n); }
    QByteArray writeData(const QString &dev, int mib, const QString &type = QString());
    QByteArray readData(const QString &dev, const QString &type = QString());

    QTemporaryDir m_dir;
    QString m_loop;
    QString m_mnt;
};

void TestF2::initTestCase()
{
    QVERIFY(m_dir.isValid());
    Parted::initialize();
    FileSystemRegistry::instance().detect();
    m_mnt = m_dir.filePath(QStringLiteral("mnt"));
    QVERIFY(QDir().mkpath(m_mnt));
}

bool TestF2::attach(const QString &image, qint64 bytes)
{
    QFile f(image);
    if (!f.open(QIODevice::WriteOnly) || !f.resize(bytes))
        return false;
    f.close();
    if (geteuid() != 0 || !Tools::has(QStringLiteral("losetup")) || !Tools::has(QStringLiteral("parted")))
        return false;
    const CommandResult r = runCommand(QStringLiteral("losetup"), { QStringLiteral("-f"), QStringLiteral("--show"), image });
    m_loop = r.output.trimmed();
    return r.ok() && !m_loop.isEmpty();
}

void TestF2::cleanup()
{
    runCommand(QStringLiteral("umount"), { m_mnt });
    if (!m_loop.isEmpty()) {
        runCommand(QStringLiteral("losetup"), { QStringLiteral("-d"), m_loop });
        m_loop.clear();
    }
}

QByteArray TestF2::writeData(const QString &dev, int mib, const QString &type)
{
    QStringList mountArgs;
    if (!type.isEmpty())
        mountArgs << QStringLiteral("-t") << type;
    if (!run(QStringLiteral("mount"), mountArgs + QStringList{ dev, m_mnt }))
        return QByteArray();
    const QString file = m_mnt + QStringLiteral("/data.bin");
    run(QStringLiteral("dd"), { QStringLiteral("if=/dev/urandom"), QStringLiteral("of=") + file, QStringLiteral("bs=1M"),
                                QStringLiteral("count=") + QString::number(mib), QStringLiteral("status=none") });
    QFile f(file);
    QByteArray hash;
    if (f.open(QIODevice::ReadOnly))
        hash = QCryptographicHash::hash(f.readAll(), QCryptographicHash::Md5).toHex();
    f.close();
    run(QStringLiteral("umount"), { m_mnt });
    return hash;
}

QByteArray TestF2::readData(const QString &dev, const QString &type)
{
    QStringList mountArgs{ QStringLiteral("-o"), QStringLiteral("ro") };
    if (!type.isEmpty())
        mountArgs << QStringLiteral("-t") << type;
    if (!run(QStringLiteral("mount"), mountArgs + QStringList{ dev, m_mnt }))
        return QByteArray("mount failed");
    QFile f(m_mnt + QStringLiteral("/data.bin"));
    QByteArray hash = "missing";
    if (f.open(QIODevice::ReadOnly))
        hash = QCryptographicHash::hash(f.readAll(), QCryptographicHash::Md5).toHex();
    f.close();
    run(QStringLiteral("umount"), { m_mnt });
    return hash;
}

void TestF2::copyBlocksOverlap()
{
    // Each 512 byte sector holds its own number: overlapping copies in both directions
    // must reproduce the source exactly.
    const QString img = m_dir.filePath(QStringLiteral("pattern.img"));
    const qint64 sectors = 64 * MiB / 512;
    {
        QFile f(img);
        QVERIFY(f.open(QIODevice::WriteOnly));
        QByteArray sector(512, 0);
        for (qint64 i = 0; i < sectors; ++i) {
            memcpy(sector.data(), &i, sizeof i);
            f.write(sector);
        }
    }
    auto sectorValue = [&](qint64 s) {
        QFile f(img);
        f.open(QIODevice::ReadOnly);
        f.seek(s * 512);
        qint64 v = -1;
        f.read(reinterpret_cast<char *>(&v), sizeof v);
        return v;
    };
    const Sector length = 20 * MiB / 512;  // not a multiple of the 4 MiB block size: partial blocks
    OperationDetail d;
    ByteValue done = 0;
    // Right, overlapping by most of the range.
    QVERIFY(Parted::copyBlocks(img, img, 2048, 2048 + 1000, length * 512 + 512 * 7, d, done));
    for (qint64 i : { qint64(0), qint64(1), length / 2, length + 6 })
        QCOMPARE(sectorValue(2048 + 1000 + i), 2048 + i);
    // Left, overlapping: copy that range back to where it came from.
    QVERIFY(Parted::copyBlocks(img, img, 2048 + 1000, 2048, length * 512, d, done));
    for (qint64 i : { qint64(0), qint64(999), length / 3, length - 1 })
        QCOMPARE(sectorValue(2048 + i), 2048 + i);
    QCOMPARE(done, length * 512);
}

void TestF2::ext4ResizeAndMove()
{
    if (!attach(m_dir.filePath(QStringLiteral("ext4.img")), 1024 * MiB))
        QSKIP("needs root, losetup and parted");
    QVERIFY(run(QStringLiteral("parted"), { QStringLiteral("-s"), m_loop, QStringLiteral("mklabel"), QStringLiteral("gpt"),
                                            QStringLiteral("mkpart"), QStringLiteral("data"), QStringLiteral("ext4"),
                                            QStringLiteral("1MiB"), QStringLiteral("201MiB") }));
    Parted::settle();
    QVERIFY(run(QStringLiteral("mkfs.ext4"), { QStringLiteral("-q"), QStringLiteral("-F"), QStringLiteral("-L"), QStringLiteral("data"), part(1) }));
    const QByteArray hash = writeData(part(1), 60);
    QVERIFY(!hash.isEmpty());

    Session s;
    auto rescan = [&]() { s.devices = { scanOne(m_loop) }; };
    rescan();
    QVERIFY(byNumber(s.devices[0], 1)->usageKnown());

    // Grow in place.
    QVERIFY(queueResizeMove(s, 1, 0, 400));
    QVERIFY(applyAll(s));
    rescan();
    QCOMPARE(byNumber(s.devices[0], 1)->length(), Sector(400 * MiB / 512));
    QCOMPARE(byNumber(s.devices[0], 1)->sectorsUnallocated, Sector(0));  // file system fills it
    QCOMPARE(readData(part(1)), hash);

    // Shrink in place.
    QVERIFY(queueResizeMove(s, 1, 0, 150));
    QVERIFY(applyAll(s));
    rescan();
    QCOMPARE(byNumber(s.devices[0], 1)->length(), Sector(150 * MiB / 512));
    QCOMPARE(readData(part(1)), hash);

    // Move right beyond the old end, then left overlapping itself.
    QVERIFY(queueResizeMove(s, 1, 300, 150));
    QVERIFY(applyAll(s));
    rescan();
    QCOMPARE(byNumber(s.devices[0], 1)->start, Sector(301 * MiB / 512));
    QCOMPARE(readData(part(1)), hash);

    QVERIFY(queueResizeMove(s, 1, -100, 150));
    QVERIFY(applyAll(s));
    rescan();
    QCOMPARE(byNumber(s.devices[0], 1)->start, Sector(201 * MiB / 512));
    QCOMPARE(readData(part(1)), hash);

    // Move left and grow at once (two steps), then move right and shrink.
    QVERIFY(queueResizeMove(s, 1, -100, 300));
    QVERIFY(applyAll(s));
    rescan();
    QCOMPARE(byNumber(s.devices[0], 1)->start, Sector(101 * MiB / 512));
    QCOMPARE(byNumber(s.devices[0], 1)->length(), Sector(300 * MiB / 512));
    QCOMPARE(readData(part(1)), hash);

    QVERIFY(queueResizeMove(s, 1, 50, 120));
    QVERIFY(applyAll(s));
    rescan();
    QCOMPARE(byNumber(s.devices[0], 1)->start, Sector(151 * MiB / 512));
    QCOMPARE(byNumber(s.devices[0], 1)->length(), Sector(120 * MiB / 512));
    QCOMPARE(readData(part(1)), hash);
    QVERIFY(run(QStringLiteral("e2fsck"), { QStringLiteral("-f"), QStringLiteral("-n"), part(1) }));

    // Below the used space is refused by the limits.
    const Device disp = s.displayDevice(0);
    const Layout::SpaceEdit space = Layout::resizeMoveSpace(disp, *byNumber(disp, 1));
    QVERIFY(space.minSizeMiB >= 60);
}

void TestF2::logicalMoveKeepsNeighbours()
{
    if (!attach(m_dir.filePath(QStringLiteral("logical.img")), 1024 * MiB))
        QSKIP("needs root, losetup and parted");
    QVERIFY(run(QStringLiteral("parted"), { QStringLiteral("-s"), m_loop, QStringLiteral("mklabel"), QStringLiteral("msdos"),
                                            QStringLiteral("mkpart"), QStringLiteral("extended"), QStringLiteral("1MiB"), QStringLiteral("100%"),
                                            QStringLiteral("mkpart"), QStringLiteral("logical"), QStringLiteral("2MiB"), QStringLiteral("202MiB"),
                                            QStringLiteral("mkpart"), QStringLiteral("logical"), QStringLiteral("403MiB"), QStringLiteral("603MiB") }));
    Parted::settle();
    QVERIFY(run(QStringLiteral("mkfs.ext4"), { QStringLiteral("-q"), QStringLiteral("-F"), part(5) }));
    QVERIFY(run(QStringLiteral("mkfs.ext4"), { QStringLiteral("-q"), QStringLiteral("-F"), part(6) }));
    const QByteArray h5 = writeData(part(5), 40);
    const QByteArray h6 = writeData(part(6), 40);

    Session s;
    s.devices = { scanOne(m_loop) };
    // Move #6 left next to #5 (overlapping itself), then #5 right towards #6.
    QVERIFY(queueResizeMove(s, 6, -150, 200));
    QVERIFY(applyAll(s));
    s.devices = { scanOne(m_loop) };
    QVERIFY(byNumber(s.devices[0], 6)->start < Sector(403 * MiB / 512));
    QVERIFY(queueResizeMove(s, 5, 30, 200));
    QVERIFY(applyAll(s));
    s.devices = { scanOne(m_loop) };
    QCOMPARE(readData(part(5)), h5);
    QCOMPARE(readData(part(6)), h6);
    // The EBR chain is intact: parted still sees both logicals.
    const CommandResult r = runCommand(QStringLiteral("parted"), { QStringLiteral("-s"), m_loop, QStringLiteral("print") });
    QVERIFY2(r.output.contains(QLatin1String(" 5 ")) && r.output.contains(QLatin1String(" 6 ")), qPrintable(r.output));
}

void TestF2::fatResizeWithLibparted()
{
    if (!attach(m_dir.filePath(QStringLiteral("fat.img")), 512 * MiB) || !Tools::has(QStringLiteral("mkfs.fat"))
        || !Tools::has(QStringLiteral("mcopy")))
        QSKIP("needs root, losetup, parted, dosfstools and mtools");
    QVERIFY(run(QStringLiteral("parted"), { QStringLiteral("-s"), m_loop, QStringLiteral("mklabel"), QStringLiteral("gpt"),
                                            QStringLiteral("mkpart"), QStringLiteral("efi"), QStringLiteral("fat32"),
                                            QStringLiteral("1MiB"), QStringLiteral("201MiB") }));
    Parted::settle();
    // 4 KiB clusters: libparted cannot resize FAT32 with 512 byte clusters (same as GParted).
    QVERIFY(run(QStringLiteral("mkfs.fat"), { QStringLiteral("-F"), QStringLiteral("32"), QStringLiteral("-s"), QStringLiteral("8"), part(1) }));
    // Through mtools: the vfat kernel module may be missing.
    const QString src = m_dir.filePath(QStringLiteral("fatdata.bin"));
    QVERIFY(run(QStringLiteral("dd"), { QStringLiteral("if=/dev/urandom"), QStringLiteral("of=") + src,
                                        QStringLiteral("bs=1M"), QStringLiteral("count=30"), QStringLiteral("status=none") }));
    QFile sf(src);
    QVERIFY(sf.open(QIODevice::ReadOnly));
    const QByteArray hash = QCryptographicHash::hash(sf.readAll(), QCryptographicHash::Md5).toHex();
    sf.close();
    QVERIFY(run(QStringLiteral("mcopy"), { QStringLiteral("-i"), part(1), src, QStringLiteral("::data.bin") }));
    auto fatHash = [&]() {
        const QString out = m_dir.filePath(QStringLiteral("fatout.bin"));
        QFile::remove(out);
        if (!run(QStringLiteral("mcopy"), { QStringLiteral("-i"), part(1), QStringLiteral("::data.bin"), out }))
            return QByteArray("missing");
        QFile f(out);
        f.open(QIODevice::ReadOnly);
        return QCryptographicHash::hash(f.readAll(), QCryptographicHash::Md5).toHex();
    };

    Session s;
    s.devices = { scanOne(m_loop) };
    QVERIFY2(byNumber(s.devices[0], 1)->usageKnown(), "FAT usage needs mtools");
    QVERIFY(queueResizeMove(s, 1, 0, 300));
    QVERIFY(applyAll(s));
    QCOMPARE(fatHash(), hash);
    s.devices = { scanOne(m_loop) };
    QVERIFY(byNumber(s.devices[0], 1)->sectorsUnallocated < Sector(4 * MiB / 512));

    QVERIFY(queueResizeMove(s, 1, 0, 120));
    QVERIFY(applyAll(s));
    QCOMPARE(fatHash(), hash);
    QVERIFY(run(QStringLiteral("fsck.fat"), { QStringLiteral("-n"), part(1) }));
}

void TestF2::ntfsResizeMoveBootSector()
{
    if (!attach(m_dir.filePath(QStringLiteral("ntfs.img")), 768 * MiB) || !Tools::has(QStringLiteral("mkntfs")))
        QSKIP("needs root, losetup, parted and ntfs-3g");
    QVERIFY(run(QStringLiteral("parted"), { QStringLiteral("-s"), m_loop, QStringLiteral("mklabel"), QStringLiteral("msdos"),
                                            QStringLiteral("mkpart"), QStringLiteral("primary"), QStringLiteral("ntfs"),
                                            QStringLiteral("1MiB"), QStringLiteral("201MiB") }));
    Parted::settle();
    QVERIFY(run(QStringLiteral("mkntfs"), { QStringLiteral("-Q"), QStringLiteral("-q"), QStringLiteral("-F"), part(1) }));
    Session s;
    s.devices = { scanOne(m_loop) };
    QVERIFY(byNumber(s.devices[0], 1)->usageKnown());
    QVERIFY(queueResizeMove(s, 1, 100, 300));  // move right and grow
    QVERIFY(applyAll(s));
    s.devices = { scanOne(m_loop) };
    const Partition *p = byNumber(s.devices[0], 1);
    QCOMPARE(p->start, Sector(101 * MiB / 512));
    QVERIFY(run(QStringLiteral("ntfsresize"), { QStringLiteral("-i"), QStringLiteral("-f"), part(1) }));
    QFile f(part(1));
    QVERIFY(f.open(QIODevice::ReadOnly));
    f.seek(0x1C);
    quint32 hidden = 0;
    f.read(reinterpret_cast<char *>(&hidden), 4);
    QCOMPARE(qint64(hidden), qint64(p->start));
    f.close();

    QVERIFY(queueResizeMove(s, 1, 0, 150));  // shrink
    QVERIFY(applyAll(s));
    s.devices = { scanOne(m_loop) };
    QCOMPARE(byNumber(s.devices[0], 1)->length(), Sector(150 * MiB / 512));
    QVERIFY(run(QStringLiteral("ntfsresize"), { QStringLiteral("-i"), QStringLiteral("-f"), part(1) }));
}

void TestF2::swapResizeKeepsIdentity()
{
    if (!attach(m_dir.filePath(QStringLiteral("swap.img")), 512 * MiB))
        QSKIP("needs root, losetup and parted");
    QVERIFY(run(QStringLiteral("parted"), { QStringLiteral("-s"), m_loop, QStringLiteral("mklabel"), QStringLiteral("gpt"),
                                            QStringLiteral("mkpart"), QStringLiteral("swap"), QStringLiteral("linux-swap"),
                                            QStringLiteral("1MiB"), QStringLiteral("101MiB") }));
    Parted::settle();
    const QString uuid = QStringLiteral("11111111-2222-3333-4444-555555555555");
    QVERIFY(run(QStringLiteral("mkswap"), { QStringLiteral("-L"), QStringLiteral("myswap"), QStringLiteral("-U"), uuid, part(1) }));
    Session s;
    s.devices = { scanOne(m_loop) };
    QVERIFY(queueResizeMove(s, 1, 50, 200));  // move right + grow: recreated
    QVERIFY(applyAll(s));
    s.devices = { scanOne(m_loop) };
    const Partition *p = byNumber(s.devices[0], 1);
    QCOMPARE(p->fsType, FsType::LinuxSwap);
    QCOMPARE(p->fsLabel, QStringLiteral("myswap"));
    QCOMPARE(p->uuid, uuid);
    QCOMPARE(p->length(), Sector(200 * MiB / 512));
}

void TestF2::btrfsAndXfsGrow()
{
    if (!attach(m_dir.filePath(QStringLiteral("bx.img")), 2048 * MiB) || !Tools::has(QStringLiteral("mkfs.btrfs"))
        || !Tools::has(QStringLiteral("mkfs.xfs")))
        QSKIP("needs root, losetup, parted, btrfs-progs and xfsprogs");
    QVERIFY(run(QStringLiteral("parted"), { QStringLiteral("-s"), m_loop, QStringLiteral("mklabel"), QStringLiteral("gpt"),
                                            QStringLiteral("mkpart"), QStringLiteral("b"), QStringLiteral("1MiB"), QStringLiteral("401MiB"),
                                            QStringLiteral("mkpart"), QStringLiteral("x"), QStringLiteral("1001MiB"), QStringLiteral("1401MiB") }));
    Parted::settle();
    QVERIFY(run(QStringLiteral("mkfs.btrfs"), { QStringLiteral("-q"), QStringLiteral("-f"), part(1) }));
    QVERIFY(run(QStringLiteral("mkfs.xfs"), { QStringLiteral("-q"), QStringLiteral("-f"), part(2) }));
    const QByteArray hb = writeData(part(1), 40);
    const QByteArray hx = writeData(part(2), 40);
    Session s;
    s.devices = { scanOne(m_loop) };
    QVERIFY(queueResizeMove(s, 1, 0, 600));  // btrfs grow
    QVERIFY(queueResizeMove(s, 2, 0, 600));  // xfs grow
    QVERIFY(applyAll(s));
    s.devices = { scanOne(m_loop) };
    QVERIFY(byNumber(s.devices[0], 1)->sectorsUnallocated < Sector(8 * MiB / 512));
    QVERIFY(byNumber(s.devices[0], 2)->sectorsUnallocated < Sector(8 * MiB / 512));
    QCOMPARE(readData(part(1)), hb);
    QCOMPARE(readData(part(2)), hx);

    QVERIFY(queueResizeMove(s, 1, 0, 450));  // btrfs shrink (mounted on a temporary directory)
    QVERIFY(applyAll(s));
    QCOMPARE(readData(part(1)), hb);
    // xfs can't shrink: the limits keep its size.
    const Device disp = s.displayDevice(0);
    QCOMPARE(Layout::resizeMoveSpace(disp, *byNumber(disp, 2)).minSizeMiB, qint64(600));
}

void TestF2::pasteLabelUuidCheck()
{
    if (!attach(m_dir.filePath(QStringLiteral("paste.img")), 1024 * MiB))
        QSKIP("needs root, losetup and parted");
    QVERIFY(run(QStringLiteral("parted"), { QStringLiteral("-s"), m_loop, QStringLiteral("mklabel"), QStringLiteral("gpt"),
                                            QStringLiteral("mkpart"), QStringLiteral("src"), QStringLiteral("1MiB"), QStringLiteral("201MiB") }));
    Parted::settle();
    QVERIFY(run(QStringLiteral("mkfs.ext4"), { QStringLiteral("-q"), QStringLiteral("-F"), QStringLiteral("-L"), QStringLiteral("orig"), part(1) }));
    const QByteArray hash = writeData(part(1), 50);

    Session s;
    s.devices = { scanOne(m_loop) };
    const Partition src = *byNumber(s.devices[0], 1);
    {
        const Device disp = s.displayDevice(0);
        const Partition *un = nullptr;
        for (const Partition &p : disp.partitions)
            if (p.type == PartitionType::Unallocated)
                un = &p;
        QVERIFY(un);
        QVERIFY(Session::allowedActions(disp, un, 0, &src).paste);
        const Layout::SpaceEdit space = Layout::pasteSpace(*un, src);
        QCOMPARE(space.sizeMiB, qint64(200));
        const Partition np = Layout::makePastedPartition(disp, *un, src, space, space.beforeMiB, 400,
                                                         space.totalMiB - space.beforeMiB - 400, Alignment::MiB,
                                                         s.nextNewNumber());
        QCOMPARE(np.status, PartitionStatus::Copy);
        QVERIFY(s.addOperation(std::make_unique<OperationCopy>(s.devices[0], *un, np, src)));
    }
    QVERIFY(applyAll(s));
    s.devices = { scanOne(m_loop) };
    const Partition *copy = byNumber(s.devices[0], 2);
    QVERIFY(copy);
    QCOMPARE(copy->fsType, FsType::Ext4);
    QCOMPARE(copy->uuid, src.uuid);  // a block copy keeps the UUID
    QCOMPARE(copy->length(), Sector(400 * MiB / 512));
    QVERIFY(copy->sectorsUnallocated < Sector(4 * MiB / 512));  // grown to fill
    QCOMPARE(readData(part(2)), hash);

    // New UUID and label for the copy, check the source.
    QVERIFY(s.addOperation(std::make_unique<OperationChangeUuid>(s.devices[0], *copy)));
    QVERIFY(s.addOperation(std::make_unique<OperationLabel>(s.devices[0], *copy, QStringLiteral("backup"))));
    QVERIFY(s.addOperation(std::make_unique<OperationCheck>(s.devices[0], *byNumber(s.devices[0], 1))));
    {
        const Device disp = s.displayDevice(0);
        QCOMPARE(byNumber(disp, 2)->fsLabel, QStringLiteral("backup"));
    }
    QVERIFY(applyAll(s));
    s.devices = { scanOne(m_loop) };
    QVERIFY(byNumber(s.devices[0], 2)->uuid != src.uuid);
    QCOMPARE(byNumber(s.devices[0], 2)->fsLabel, QStringLiteral("backup"));
    QCOMPARE(byNumber(s.devices[0], 1)->fsLabel, QStringLiteral("orig"));
    QCOMPARE(readData(part(2)), hash);
}

static bool hasCapability(int cap)
{
    QFile f(QStringLiteral("/proc/self/status"));
    if (!f.open(QIODevice::ReadOnly))
        return false;
    for (const QByteArray &line : f.readAll().split('\n'))
        if (line.startsWith("CapEff:"))
            return (line.mid(7).trimmed().toULongLong(nullptr, 16) >> cap) & 1;
    return false;
}

void TestF2::onlineGrow()
{
    // Online ext4 resize needs CAP_SYS_RESOURCE (24), which containers often drop;
    // xfs_growfs only needs CAP_SYS_ADMIN.
    const bool ext4 = hasCapability(24);
    const QString mkfs = ext4 ? QStringLiteral("mkfs.ext4") : QStringLiteral("mkfs.xfs");
    if (!attach(m_dir.filePath(QStringLiteral("online.img")), 1024 * MiB) || !Tools::has(mkfs))
        QSKIP("needs root, losetup, parted and mkfs.ext4 or mkfs.xfs");
    QVERIFY(run(QStringLiteral("parted"), { QStringLiteral("-s"), m_loop, QStringLiteral("mklabel"), QStringLiteral("gpt"),
                                            QStringLiteral("mkpart"), QStringLiteral("root"), QStringLiteral("1MiB"), QStringLiteral("401MiB") }));
    Parted::settle();
    QVERIFY(run(mkfs, { ext4 ? QStringLiteral("-q") : QStringLiteral("-q"), ext4 ? QStringLiteral("-F") : QStringLiteral("-f"), part(1) }));
    const QByteArray hash = writeData(part(1), 30);
    QVERIFY(run(QStringLiteral("mount"), { part(1), m_mnt }));
    Session s;
    s.devices = { scanOne(m_loop) };
    const Partition *p = byNumber(s.devices[0], 1);
    QVERIFY(p->busy);
    const AllowedActions a = Session::allowedActions(s.devices[0], p, 0);
    QVERIFY(a.resize);
    QVERIFY(!a.remove && !a.format && !a.check);
    // Online: the start is fixed, only the end moves.
    const Layout::SpaceEdit space = Layout::resizeMoveSpace(s.devices[0], *p);
    QVERIFY(space.fixedStart);
    QVERIFY(queueResizeMove(s, 1, 0, 800));
    const bool ok = applyAll(s);
    ByteValue size = 0, freeB = 0;
    QString error;
    mountedUsage(m_mnt, size, freeB, error);
    run(QStringLiteral("umount"), { m_mnt });
    QVERIFY(ok);
    QVERIFY2(size > 700 * MiB, qPrintable(formatBytes(size)));
    QCOMPARE(readData(part(1)), hash);
}

void TestF2::extendedResize()
{
    if (!attach(m_dir.filePath(QStringLiteral("ext.img")), 1024 * MiB))
        QSKIP("needs root, losetup and parted");
    QVERIFY(run(QStringLiteral("parted"), { QStringLiteral("-s"), m_loop, QStringLiteral("mklabel"), QStringLiteral("msdos"),
                                            QStringLiteral("mkpart"), QStringLiteral("extended"), QStringLiteral("101MiB"), QStringLiteral("601MiB"),
                                            QStringLiteral("mkpart"), QStringLiteral("logical"), QStringLiteral("102MiB"), QStringLiteral("302MiB") }));
    Parted::settle();
    QVERIFY(run(QStringLiteral("mkfs.ext4"), { QStringLiteral("-q"), QStringLiteral("-F"), part(5) }));
    const QByteArray hash = writeData(part(5), 20);
    Session s;
    s.devices = { scanOne(m_loop) };
    const Partition *ext = byNumber(s.devices[0], 1);
    QVERIFY(ext && ext->type == PartitionType::Extended);
    const Layout::SpaceEdit space = Layout::resizeMoveSpace(s.devices[0], *ext);
    // Can't shrink over the logical partition.
    QVERIFY(space.minSizeMiB >= 200);
    // Grow to the left and right around the logical.
    QVERIFY(queueResizeMove(s, 1, -50, 700));
    QVERIFY(applyAll(s));
    s.devices = { scanOne(m_loop) };
    ext = byNumber(s.devices[0], 1);
    QCOMPARE(ext->start, Sector(51 * MiB / 512));
    QCOMPARE(ext->length(), Sector(700 * MiB / 512));
    QCOMPARE(readData(part(5)), hash);
}

QTEST_GUILESS_MAIN(TestF2)
#include "tst_f2.moc"

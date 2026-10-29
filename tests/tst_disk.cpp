/*
 * QtParted - end to end tests on image files and loop devices.
 * Copyright (C) 2026 Vanni Brutto <zanac4ever@gmail.com>
 *
 * The loop device tests need root and create, format and delete partitions on a
 * temporary image attached with losetup; they are skipped otherwise.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "Command.h"
#include "Layout.h"
#include "Parted.h"
#include "Session.h"
#include "Worker.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <unistd.h>

using namespace QtParted;

namespace {

bool makeImage(const QString &path, qint64 bytes)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.resize(bytes);
}

Device scanOne(const QString &path)
{
    const std::vector<Device> d = scanDevices({ path });
    if (d.size() != 1)
        return Device();
    return d.front();
}

// Apply all pending operations synchronously; on failure print the details.
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

const Partition *largestUnallocated(const PartitionVector &v)
{
    const Partition *best = nullptr;
    for (const Partition &p : v) {
        if (p.fsType == FsType::Unallocated && (!best || p.length() > best->length()))
            best = &p;
        for (const Partition &l : p.logicals)
            if (l.fsType == FsType::Unallocated && (!best || l.length() > best->length()))
                best = &l;
    }
    return best;
}

// Queue the creation of a partition in the largest free space, like the dialog does.
Partition queueCreate(Session &s, PartitionType type, FsType fs, qint64 sizeMiB, const QString &label = QString(),
                      const QString &name = QString())
{
    const Device disp = s.displayDevice(0);
    const Partition *un = largestUnallocated(disp.partitions);
    if (!un)
        return Partition();
    Layout::NewPartitionRequest req;
    req.type = type;
    req.fsType = fs;
    req.beforeMiB = Layout::mibNeededForBootRecord(*un);
    const qint64 maxMiB = un->byteLength() / MiB - req.beforeMiB;
    req.sizeMiB = sizeMiB > 0 ? std::min(sizeMiB, maxMiB) : maxMiB;
    req.label = label;
    req.name = name;
    req.number = s.nextNewNumber();
    const Partition np = Layout::makeNewPartition(disp, *un, req);
    QString error;
    if (!s.addOperation(std::make_unique<OperationCreate>(s.devices[0], *un, np), &error))
        qWarning() << "addOperation failed:" << error;
    return np;
}

void queueTable(Session &s, const QString &type)
{
    const Parted::DiskTypeInfo info = Parted::diskTypeInfo(s.devices[0].path, type);
    s.addOperation(std::make_unique<OperationCreateTable>(s.devices[0], type, info));
}

} // namespace

class TestDisk : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void cleanup();
    void imageFileTableAndPartitions();
    void loopMsdosWithLogicals();
    void loopGptEspAndWholeDisk();

private:
    bool attachLoop(const QString &image);

    QTemporaryDir m_dir;
    QString m_loop;
};

void TestDisk::initTestCase()
{
    QVERIFY(m_dir.isValid());
    Parted::initialize();
    FileSystemRegistry::instance().detect();
}

bool TestDisk::attachLoop(const QString &image)
{
    if (geteuid() != 0 || !Tools::has(QStringLiteral("losetup")))
        return false;
    const CommandResult r = runCommand(QStringLiteral("losetup"), { QStringLiteral("-f"), QStringLiteral("--show"), image });
    if (!r.ok())
        return false;
    m_loop = r.output.trimmed();
    return !m_loop.isEmpty();
}

void TestDisk::cleanup()
{
    if (!m_loop.isEmpty()) {
        runCommand(QStringLiteral("losetup"), { QStringLiteral("-d"), m_loop });
        m_loop.clear();
    }
}

void TestDisk::imageFileTableAndPartitions()
{
    const QString img = m_dir.filePath(QStringLiteral("image.img"));
    QVERIFY(makeImage(img, 128 * MiB));

    Session s;
    s.devices.push_back(scanOne(img));
    QVERIFY(s.devices[0].isImageFile);
    QVERIFY(!s.devices[0].hasPartitionTable);
    QCOMPARE(s.devices[0].partitions.front().type, PartitionType::Unpartitioned);

    queueTable(s, QStringLiteral("gpt"));
    QVERIFY(applyAll(s));
    s.devices = { scanOne(img) };
    QCOMPARE(s.devices[0].diskType, QStringLiteral("gpt"));
    QCOMPARE(s.devices[0].maxNameLength, 36);

    // File system tools need a device node: only unformatted partitions on images.
    QVERIFY(!Layout::canCreate(FsType::Ext4, s.devices[0], 64 * MiB));
    const Partition np = queueCreate(s, PartitionType::Primary, FsType::Unformatted, 64, QString(), QStringLiteral("data"));
    QCOMPARE(np.start, Sector(2048));
    QVERIFY(applyAll(s));

    s.devices = { scanOne(img) };
    const Partition *p1 = byNumber(s.devices[0], 1);
    QVERIFY(p1);
    QCOMPARE(p1->start, Sector(2048));
    QCOMPARE(p1->length(), Sector(64 * MiB / 512));
    QCOMPARE(p1->name, QStringLiteral("data"));
    QVERIFY(p1->availableFlags.contains(QStringLiteral("bios_grub")));

    QVERIFY(s.addOperation(std::make_unique<OperationSetFlags>(s.devices[0], *p1, QStringList{ QStringLiteral("bios_grub") })));
    QVERIFY(s.addOperation(std::make_unique<OperationName>(s.devices[0], *p1, QStringLiteral("renamed"))));
    QVERIFY(applyAll(s));
    s.devices = { scanOne(img) };
    p1 = byNumber(s.devices[0], 1);
    QVERIFY(p1);
    QCOMPARE(p1->flags, QStringList{ QStringLiteral("bios_grub") });
    QCOMPARE(p1->name, QStringLiteral("renamed"));

    QVERIFY(s.addOperation(std::make_unique<OperationDelete>(s.devices[0], *p1)));
    QVERIFY(applyAll(s));
    s.devices = { scanOne(img) };
    QVERIFY(!byNumber(s.devices[0], 1));
    QCOMPARE(int(s.devices[0].partitions.size()), 1);
    QCOMPARE(s.devices[0].partitions.front().type, PartitionType::Unallocated);
}

void TestDisk::loopMsdosWithLogicals()
{
    const QString img = m_dir.filePath(QStringLiteral("msdos.img"));
    QVERIFY(makeImage(img, 512 * MiB));
    if (!attachLoop(img))
        QSKIP("needs root and losetup");

    Session s;
    s.devices.push_back(scanOne(m_loop));
    QVERIFY(!s.devices[0].isImageFile);
    queueTable(s, QStringLiteral("msdos"));
    queueCreate(s, PartitionType::Primary, FsType::Ext4, 128, QStringLiteral("root"));
    queueCreate(s, PartitionType::Extended, FsType::Extended, 0);
    queueCreate(s, PartitionType::Logical, FsType::Fat32, 64, QStringLiteral("efi"));
    queueCreate(s, PartitionType::Logical, FsType::LinuxSwap, 0, QStringLiteral("swap"));
    QCOMPARE(int(s.operations.size()), 5);
    QVERIFY(applyAll(s));

    s.devices = { scanOne(m_loop) };
    const Device &d = s.devices[0];
    QCOMPARE(d.diskType, QStringLiteral("msdos"));
    const Partition *p1 = byNumber(d, 1);
    QVERIFY(p1);
    QCOMPARE(p1->fsType, FsType::Ext4);
    QCOMPARE(p1->fsLabel, QStringLiteral("root"));
    QVERIFY2(p1->usageKnown(), qPrintable(p1->messages.join(QLatin1Char('\n'))));
    const Partition *p2 = byNumber(d, 2);
    QVERIFY(p2);
    QCOMPARE(p2->type, PartitionType::Extended);
    QVERIFY(p2->isFlagSet(QStringLiteral("lba")));
    const Partition *p5 = byNumber(d, 5);
    QVERIFY(p5);
    QCOMPARE(p5->fsType, FsType::Fat32);
    QCOMPARE(p5->fsLabel, QStringLiteral("EFI"));
    QVERIFY(p5->isFlagSet(QStringLiteral("lba")));
    QVERIFY2(p5->usageKnown(), qPrintable(p5->messages.join(QLatin1Char('\n'))));
    const Partition *p6 = byNumber(d, 6);
    QVERIFY(p6);
    QCOMPARE(p6->fsType, FsType::LinuxSwap);
    QCOMPARE(p6->fsLabel, QStringLiteral("swap"));

    // Reformat the primary and delete the first logical: the second one becomes #5.
    Partition ntfs = *p1;
    ntfs.fsType = FsType::Ntfs;
    ntfs.fsLabel = QStringLiteral("Data");
    ntfs.status = PartitionStatus::Formatted;
    QVERIFY(s.addOperation(std::make_unique<OperationFormat>(d, *p1, ntfs)));
    QVERIFY(s.addOperation(std::make_unique<OperationDelete>(d, *p5)));
    {
        const Device disp = s.displayDevice(0);
        const Partition *shown = byNumber(disp, 5);
        QVERIFY(shown);
        QCOMPARE(shown->fsType, FsType::LinuxSwap);
    }
    QVERIFY(applyAll(s));

    s.devices = { scanOne(m_loop) };
    p1 = byNumber(s.devices[0], 1);
    QVERIFY(p1);
    QCOMPARE(p1->fsType, FsType::Ntfs);
    QCOMPARE(p1->fsLabel, QStringLiteral("Data"));
    QVERIFY2(p1->usageKnown(), qPrintable(p1->messages.join(QLatin1Char('\n'))));
    p5 = byNumber(s.devices[0], 5);
    QVERIFY(p5);
    QCOMPARE(p5->fsType, FsType::LinuxSwap);
    QVERIFY(!byNumber(s.devices[0], 6));
}

void TestDisk::loopGptEspAndWholeDisk()
{
    const QString img = m_dir.filePath(QStringLiteral("gpt.img"));
    QVERIFY(makeImage(img, 256 * MiB));
    if (!attachLoop(img))
        QSKIP("needs root and losetup");

    // A file system on the whole disk device, no partition table.
    QVERIFY(runCommand(QStringLiteral("mkfs.ext4"), { QStringLiteral("-q"), QStringLiteral("-F"), QStringLiteral("-L"),
                                                     QStringLiteral("whole"), m_loop }).ok());
    Session s;
    s.devices.push_back(scanOne(m_loop));
    QCOMPARE(s.devices[0].diskType, QStringLiteral("none"));
    QCOMPARE(s.devices[0].partitions.front().type, PartitionType::Unpartitioned);
    QCOMPARE(s.devices[0].partitions.front().fsType, FsType::Ext4);
    QCOMPARE(s.devices[0].partitions.front().fsLabel, QStringLiteral("whole"));

    queueTable(s, QStringLiteral("gpt"));
    const Partition esp = queueCreate(s, PartitionType::Primary, FsType::Fat32, 100, QStringLiteral("ESP"),
                                      QStringLiteral("EFI system"));
    // Flags for a not yet created partition are merged into its creation.
    {
        const Device disp = s.displayDevice(0);
        QCOMPARE(disp.partitions.front().status, PartitionStatus::New);
        Partition withFlags = disp.partitions.front();
        withFlags.flags = { QStringLiteral("esp") };
        auto op = std::make_unique<OperationSetFlags>(s.devices[0], disp.partitions.front(), withFlags.flags);
        QVERIFY(s.addOperation(std::move(op)));
        QCOMPARE(int(s.operations.size()), 2);
    }
    {
        // 154 MiB are left: too small for btrfs (256 MiB minimum).
        const Device disp = s.displayDevice(0);
        const Partition *un = largestUnallocated(disp.partitions);
        QVERIFY(un);
        QVERIFY(!Layout::canCreate(FsType::Btrfs, disp, un->byteLength()));
        QVERIFY(Layout::canCreate(FsType::F2fs, disp, un->byteLength()));
    }
    queueCreate(s, PartitionType::Primary, FsType::F2fs, 0, QStringLiteral("pool"));
    QVERIFY(applyAll(s));

    s.devices = { scanOne(m_loop) };
    const Partition *p1 = byNumber(s.devices[0], 1);
    QVERIFY(p1);
    QCOMPARE(p1->fsType, FsType::Fat32);
    QCOMPARE(p1->name, QStringLiteral("EFI system"));
    QVERIFY2(p1->isFlagSet(QStringLiteral("esp")), qPrintable(p1->flags.join(QLatin1Char(','))));
    const Partition *p2 = byNumber(s.devices[0], 2);
    QVERIFY(p2);
    QCOMPARE(p2->fsType, FsType::F2fs);
    QCOMPARE(p2->fsLabel, QStringLiteral("pool"));
    QVERIFY2(p2->usageKnown(), qPrintable(p2->messages.join(QLatin1Char('\n'))));
    QVERIFY(esp.end < p2->start);

    // Unticking only one of two aliased flags must not lose the other (boot/esp on GPT).
    if (p1->flags.contains(QStringLiteral("boot"))) {
        QVERIFY(s.addOperation(std::make_unique<OperationSetFlags>(
            s.devices[0], *p1, QStringList{ QStringLiteral("esp"), QStringLiteral("hidden") })));
        QVERIFY(applyAll(s));
        s.devices = { scanOne(m_loop) };
        p1 = byNumber(s.devices[0], 1);
        QVERIFY(p1);
        QVERIFY(p1->isFlagSet(QStringLiteral("esp")));
        QVERIFY(p1->isFlagSet(QStringLiteral("hidden")));
    }
}

QTEST_GUILESS_MAIN(TestDisk)
#include "tst_disk.moc"

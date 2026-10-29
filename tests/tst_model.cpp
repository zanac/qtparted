/*
 * QtParted - unit tests for the partition model, layout and operation queue.
 * Copyright (C) 2026 Vanni Brutto <zanac4ever@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "Layout.h"
#include "Session.h"

#include <QTest>

using namespace QtParted;

namespace {

constexpr Sector kMiB = MiB / 512;

Device fakeDevice(const QString &diskType, Sector lengthMiB = 1024)
{
    Device d;
    d.path = QStringLiteral("/dev/fake");
    d.length = lengthMiB * kMiB;
    d.sectorSize = 512;
    d.heads = 255;
    d.sectorsPerTrack = 63;
    d.cylinderSize = 255 * 63;
    d.diskType = diskType;
    d.hasPartitionTable = true;
    d.maxPrimaries = diskType == QLatin1String("gpt") ? 128 : 4;
    d.supportsExtended = diskType == QLatin1String("msdos");
    d.maxNameLength = diskType == QLatin1String("gpt") ? 36 : 0;
    insertUnallocated(d.path, d.partitions, 0, d.length - 1, 512, false);
    return d;
}

Partition realPartition(const Device &d, int number, Sector start, Sector end, PartitionType type, FsType fs)
{
    Partition p;
    p.devicePath = d.path;
    p.path = d.path + QString::number(number);
    p.number = number;
    p.type = type;
    p.fsType = fs;
    p.start = start;
    p.end = end;
    p.sectorSize = 512;
    p.insideExtended = type == PartitionType::Logical;
    p.availableFlags = { QStringLiteral("boot"), QStringLiteral("lba") };
    return p;
}

const Partition *findUnallocated(const PartitionVector &v)
{
    for (const Partition &p : v)
        if (p.type == PartitionType::Unallocated)
            return &p;
    return nullptr;
}

} // namespace

class TestModel : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void formatSizes();
    void unallocatedGaps();
    void newPartitionGpt();
    void newPartitionGptFillsDisk();
    void newExtendedAndLogical();
    void mergeIntoCreate();
    void deleteNewRemovesOperations();
    void deleteLogicalRenumbers();
    void createTableVisual();
    void allowedActionsRules();
    void invalidPartitionRejected();
};

void TestModel::formatSizes()
{
    QCOMPARE(formatBytes(512), QStringLiteral("512 B"));
    QVERIFY(formatBytes(MiB).endsWith(QStringLiteral(" MiB")));
    QVERIFY(formatSize(2097152, 512).endsWith(QStringLiteral(" GiB")));
    QVERIFY(formatBytes(20 * TiB).startsWith(QStringLiteral("20")));
}

void TestModel::unallocatedGaps()
{
    Device d = fakeDevice(QStringLiteral("gpt"));
    PartitionVector v{ realPartition(d, 1, 2048, 206847, PartitionType::Primary, FsType::Ext4) };
    insertUnallocated(d.path, v, 0, d.length - 1, 512, false);
    // Exactly 1 MiB before the first partition is not shown, the rest of the disk is.
    QCOMPARE(int(v.size()), 2);
    QCOMPARE(v[1].type, PartitionType::Unallocated);
    QCOMPARE(v[1].start, Sector(206848));
    QCOMPARE(v[1].end, d.length - 1);
}

void TestModel::newPartitionGpt()
{
    Device d = fakeDevice(QStringLiteral("gpt"));
    const Partition un = d.partitions.front();
    QCOMPARE(Layout::mibNeededForBootRecord(un), 1);
    Layout::NewPartitionRequest req;
    req.fsType = FsType::Ext4;
    req.beforeMiB = 1;
    req.sizeMiB = 100;
    req.label = QStringLiteral("root");
    req.name = QStringLiteral(" system ");
    const Partition p = Layout::makeNewPartition(d, un, req);
    QCOMPARE(p.start, Sector(2048));
    QCOMPARE(p.end, Sector(2048 + 100 * kMiB - 1));
    QCOMPARE(p.status, PartitionStatus::New);
    QCOMPARE(p.name, QStringLiteral("system"));
    QCOMPARE(p.fsLabel, QStringLiteral("root"));
    QVERIFY(p.flags.isEmpty());
}

void TestModel::newPartitionGptFillsDisk()
{
    Device d = fakeDevice(QStringLiteral("gpt"));
    Layout::NewPartitionRequest req;
    req.beforeMiB = 1;
    req.sizeMiB = 1023;
    const Partition p = Layout::makeNewPartition(d, d.partitions.front(), req);
    // Last MiB is kept free for the backup GPT.
    QCOMPARE(p.start, Sector(2048));
    QCOMPARE(p.end, d.length - kMiB - 1);
}

void TestModel::newExtendedAndLogical()
{
    Session s;
    s.devices.push_back(fakeDevice(QStringLiteral("msdos")));
    Device disp = s.displayDevice(0);

    Layout::NewPartitionRequest ext;
    ext.type = PartitionType::Extended;
    ext.beforeMiB = 1;
    ext.sizeMiB = 1023;
    ext.number = s.nextNewNumber();
    Partition extended = Layout::makeNewPartition(disp, disp.partitions.front(), ext);
    QCOMPARE(extended.fsType, FsType::Extended);
    QCOMPARE(extended.flags, QStringList{ QStringLiteral("lba") });
    QCOMPARE(int(extended.logicals.size()), 1);
    QCOMPARE(extended.logicals.front().start, extended.start);
    QVERIFY(s.addOperation(std::make_unique<OperationCreate>(s.devices[0], disp.partitions.front(), extended)));

    disp = s.displayDevice(0);
    const int e = findExtended(disp.partitions);
    QVERIFY(e >= 0);
    const Partition &un = disp.partitions[size_t(e)].logicals.front();
    QVERIFY(un.insideExtended);
    QCOMPARE(Layout::mibNeededForBootRecord(un), 1);

    Layout::NewPartitionRequest log;
    log.type = PartitionType::Logical;
    log.fsType = FsType::Fat32;
    log.beforeMiB = 1;
    log.sizeMiB = 200;
    log.label = QStringLiteral("efi:boot");
    log.number = s.nextNewNumber();
    const Partition logical = Layout::makeNewPartition(disp, un, log);
    QCOMPARE(logical.number, 2);
    QCOMPARE(logical.type, PartitionType::Logical);
    QVERIFY(logical.start > extended.start);
    QCOMPARE(logical.start % kMiB, Sector(0));
    QCOMPARE((logical.end + 1) % kMiB, Sector(0));
    QCOMPARE(logical.fsLabel, QStringLiteral("EFIBOOT"));  // FAT labels are sanitized
    QCOMPARE(logical.flags, QStringList{ QStringLiteral("lba") });
    QVERIFY(s.addOperation(std::make_unique<OperationCreate>(s.devices[0], un, logical)));

    disp = s.displayDevice(0);
    const Partition &x = disp.partitions[size_t(findExtended(disp.partitions))];
    QCOMPARE(x.logicals.front().path, logical.path);
    QCOMPARE(x.logicals.back().type, PartitionType::Unallocated);
}

void TestModel::mergeIntoCreate()
{
    Session s;
    s.devices.push_back(fakeDevice(QStringLiteral("gpt")));
    Device disp = s.displayDevice(0);
    Layout::NewPartitionRequest req;
    req.beforeMiB = 1;
    req.sizeMiB = 300;
    req.number = s.nextNewNumber();
    const Partition np = Layout::makeNewPartition(disp, disp.partitions.front(), req);
    QVERIFY(s.addOperation(std::make_unique<OperationCreate>(s.devices[0], disp.partitions.front(), np)));

    disp = s.displayDevice(0);
    const Partition shown = disp.partitions.front();
    QCOMPARE(shown.status, PartitionStatus::New);
    Partition formatted = shown;
    formatted.fsType = FsType::Btrfs;
    QVERIFY(s.addOperation(std::make_unique<OperationFormat>(s.devices[0], shown, formatted)));
    QVERIFY(s.addOperation(std::make_unique<OperationName>(s.devices[0], s.displayDevice(0).partitions.front(),
                                                           QStringLiteral("pool"))));
    QCOMPARE(int(s.operations.size()), 1);
    QCOMPARE(s.operations.front()->partitionNew().fsType, FsType::Btrfs);
    QCOMPARE(s.operations.front()->partitionNew().name, QStringLiteral("pool"));
    QVERIFY(s.operations.front()->description().contains(QStringLiteral("btrfs")));
}

void TestModel::deleteNewRemovesOperations()
{
    Session s;
    s.devices.push_back(fakeDevice(QStringLiteral("gpt")));
    Device disp = s.displayDevice(0);
    Layout::NewPartitionRequest req;
    req.beforeMiB = 1;
    req.sizeMiB = 10;
    req.number = s.nextNewNumber();
    QVERIFY(s.addOperation(std::make_unique<OperationCreate>(
        s.devices[0], disp.partitions.front(), Layout::makeNewPartition(disp, disp.partitions.front(), req))));
    QCOMPARE(s.nextNewNumber(), 2);
    s.removeOperationsOnNew(s.displayDevice(0).partitions.front());
    QVERIFY(s.operations.empty());
    QCOMPARE(s.nextNewNumber(), 1);
    QCOMPARE(int(s.displayDevice(0).partitions.size()), 1);
}

void TestModel::deleteLogicalRenumbers()
{
    Device d = fakeDevice(QStringLiteral("msdos"));
    d.partitions.clear();
    Partition ext = realPartition(d, 1, 2048, d.length - 1, PartitionType::Extended, FsType::Extended);
    ext.insideExtended = false;
    ext.logicals = { realPartition(d, 5, 4096, 204799, PartitionType::Logical, FsType::Ext4),
                     realPartition(d, 6, 206848, 407551, PartitionType::Logical, FsType::Ext4),
                     realPartition(d, 7, 409600, 610303, PartitionType::Logical, FsType::Ntfs) };
    insertUnallocated(d.path, ext.logicals, ext.start, ext.end, 512, true);
    d.partitions.push_back(ext);
    insertUnallocated(d.path, d.partitions, 0, d.length - 1, 512, false);

    Session s;
    s.devices.push_back(d);
    const Partition victim = d.partitions[size_t(findExtended(d.partitions))].logicals[1];
    QCOMPARE(victim.number, 6);
    QVERIFY(s.addOperation(std::make_unique<OperationDelete>(d, victim)));
    const Device disp = s.displayDevice(0);
    const Partition &x = disp.partitions[size_t(findExtended(disp.partitions))];
    QStringList numbers;
    for (const Partition &l : x.logicals)
        if (l.type == PartitionType::Logical)
            numbers << l.path;
    QCOMPARE(numbers, (QStringList{ QStringLiteral("/dev/fake5"), QStringLiteral("/dev/fake6") }));
    QVERIFY(findUnallocated(x.logicals));
}

void TestModel::createTableVisual()
{
    Device d = fakeDevice(QStringLiteral("msdos"));
    d.partitions = { realPartition(d, 1, 2048, 409599, PartitionType::Primary, FsType::Ext4) };
    insertUnallocated(d.path, d.partitions, 0, d.length - 1, 512, false);
    Session s;
    s.devices.push_back(d);
    Parted::DiskTypeInfo info;
    info.maxPrimaries = 128;
    info.maxNameLength = 36;
    QVERIFY(s.addOperation(std::make_unique<OperationCreateTable>(d, QStringLiteral("gpt"), info)));
    const Device disp = s.displayDevice(0);
    QCOMPARE(disp.diskType, QStringLiteral("gpt"));
    QCOMPARE(disp.maxPrimaries, 128);
    QVERIFY(disp.partitionNamingSupported());
    QCOMPARE(int(disp.partitions.size()), 1);
    QCOMPARE(disp.partitions.front().type, PartitionType::Unallocated);
    // A second table change is not allowed while the first is pending.
    QVERIFY(!Session::allowedActions(disp, nullptr, s.pendingCount(d.path)).createTable);
}

void TestModel::allowedActionsRules()
{
    Device d = fakeDevice(QStringLiteral("msdos"));
    Partition p = realPartition(d, 1, 2048, 409599, PartitionType::Primary, FsType::Ext4);
    AllowedActions a = Session::allowedActions(d, &p, 0);
    QVERIFY(a.format && a.remove && a.flags && a.info);
    QVERIFY(!a.create && !a.name);  // msdos has no partition names

    p.busy = true;
    a = Session::allowedActions(d, &p, 0);
    QVERIFY(!a.format && !a.remove && a.info);

    Partition ext = realPartition(d, 2, 409600, 819199, PartitionType::Extended, FsType::Extended);
    ext.insideExtended = false;
    ext.logicals = { realPartition(d, 5, 411648, 600000, PartitionType::Logical, FsType::Ext4) };
    QVERIFY(!Session::allowedActions(d, &ext, 0).remove);
    ext.logicals = { Partition::makeUnallocated(d.path, ext.start, ext.end, 512, true) };
    QVERIFY(Session::allowedActions(d, &ext, 0).remove);

    const Partition un = d.partitions.front();
    a = Session::allowedActions(d, &un, 0);
    QVERIFY(a.create && !a.format && !a.remove);

    Device img = d;
    img.isImageFile = true;
    QVERIFY(!Session::allowedActions(img, &p, 0).format);
    QVERIFY(!Layout::canCreate(FsType::Ext4, img, 100 * MiB));
    QVERIFY(Layout::canCreate(FsType::Unformatted, img, 100 * MiB));
}

void TestModel::invalidPartitionRejected()
{
    Session s;
    s.devices.push_back(fakeDevice(QStringLiteral("gpt"), 64));
    Partition bad = s.devices[0].partitions.front();
    bad.status = PartitionStatus::New;
    bad.type = PartitionType::Primary;
    bad.path = QStringLiteral("New Partition #1");
    bad.end = s.devices[0].length + 10;
    QString error;
    QVERIFY(!s.addOperation(std::make_unique<OperationCreate>(s.devices[0], s.devices[0].partitions.front(), bad),
                            &error));
    QVERIFY(error.contains(QStringLiteral("after the end")));
    QVERIFY(s.operations.empty());
}

QTEST_GUILESS_MAIN(TestModel)
#include "tst_model.moc"

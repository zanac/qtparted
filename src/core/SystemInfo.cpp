/*
 * QtParted - a Qt frontend to libparted for manipulating disk partitions
 * Copyright (C) 2002-2026 Vanni Brutto <zanac4ever@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "SystemInfo.h"

#include "Command.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTextStream>

#include <algorithm>

#include <cerrno>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/sysmacros.h>

namespace QtParted {

static QString readTextFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    return QString::fromLocal8Bit(f.readAll());
}

// /proc/self/mountinfo escapes blanks as octal \040 etc.
static QString unescapeMountField(const QString &s)
{
    QString out;
    out.reserve(s.size());
    for (int i = 0; i < s.size(); ++i) {
        if (s[i] == QLatin1Char('\\') && i + 3 < s.size()) {
            bool ok = false;
            const int code = s.mid(i + 1, 3).toInt(&ok, 8);
            if (ok) {
                out += QChar(code);
                i += 3;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

quint64 SystemInfo::devNumber(const QString &path)
{
    struct stat st;
    if (::stat(path.toLocal8Bit().constData(), &st) != 0 || !S_ISBLK(st.st_mode))
        return 0;
    return quint64(st.st_rdev);
}

bool SystemInfo::isRegularFile(const QString &path)
{
    struct stat st;
    return ::stat(path.toLocal8Bit().constData(), &st) == 0 && S_ISREG(st.st_mode);
}

void SystemInfo::load()
{
    m_mountsByDev.clear();
    m_mountsBySource.clear();
    m_allMountPoints.clear();
    m_swapsByDev.clear();
    m_swapsByPath.clear();
    m_fstab.clear();

    // 36 35 98:0 /mnt1 /mnt2 rw,noatime master:1 - ext3 /dev/root rw,errors=continue
    const QStringList lines = readTextFile(QStringLiteral("/proc/self/mountinfo")).split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QStringList f = line.split(QLatin1Char(' '));
        if (f.size() < 10)
            continue;
        const int sep = f.indexOf(QStringLiteral("-"), 6);
        if (sep < 0 || sep + 2 >= f.size())
            continue;
        Mount m;
        m.mountPoint = unescapeMountField(f[4]);
        m.readonly = f[5].split(QLatin1Char(',')).contains(QStringLiteral("ro"));
        m_allMountPoints.append(m.mountPoint);
        const QStringList mm = f[2].split(QLatin1Char(':'));
        if (mm.size() == 2) {
            const quint64 dev = makedev(mm[0].toUInt(), mm[1].toUInt());
            m_mountsByDev[dev].append(m);
        }
        const QString source = unescapeMountField(f[sep + 2]);
        if (source.startsWith(QLatin1Char('/'))) {
            m_mountsBySource[source].append(m);
            const quint64 sdev = devNumber(source);
            if (sdev && (mm.size() != 2 || sdev != makedev(mm[0].toUInt(), mm[1].toUInt())))
                m_mountsByDev[sdev].append(m);  // e.g. btrfs reports an anonymous dev number
        }
    }

    // Filename  Type  Size  Used  Priority
    const QStringList swaps = readTextFile(QStringLiteral("/proc/swaps")).split(QLatin1Char('\n'));
    for (int i = 1; i < swaps.size(); ++i) {
        const QString name = swaps[i].section(QLatin1Char(' '), 0, 0, QString::SectionSkipEmpty);
        if (name.isEmpty())
            continue;
        const QString path = unescapeMountField(name);
        m_swapsByPath.insert(path);
        if (const quint64 d = devNumber(path))
            m_swapsByDev.insert(d);
    }

    const QStringList fstab = readTextFile(QStringLiteral("/etc/fstab")).split(QLatin1Char('\n'));
    static const QRegularExpression ws(QStringLiteral("\\s+"));
    for (const QString &l : fstab) {
        const QString line = l.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        const QStringList f = line.split(ws, Qt::SkipEmptyParts);
        if (f.size() < 2 || f[1] == QLatin1String("none") || f[1] == QLatin1String("swap"))
            continue;
        m_fstab.append({ unescapeMountField(f[0]), unescapeMountField(f[1]) });
    }
}

QStringList SystemInfo::mountPoints(const QString &devPath) const
{
    QStringList result;
    auto add = [&](const QList<Mount> &list) {
        for (const Mount &m : list)
            if (!result.contains(m.mountPoint))
                result.append(m.mountPoint);
    };
    if (const quint64 d = devNumber(devPath))
        add(m_mountsByDev.value(d));
    add(m_mountsBySource.value(devPath));
    std::sort(result.begin(), result.end());
    return result;
}

bool SystemInfo::isMounted(const QString &devPath) const
{
    if (const quint64 d = devNumber(devPath))
        if (m_mountsByDev.contains(d))
            return true;
    return m_mountsBySource.contains(devPath);
}

bool SystemInfo::isMountedReadonly(const QString &devPath) const
{
    QList<Mount> list;
    if (const quint64 d = devNumber(devPath))
        list = m_mountsByDev.value(d);
    if (list.isEmpty())
        list = m_mountsBySource.value(devPath);
    if (list.isEmpty())
        return false;
    for (const Mount &m : list)
        if (!m.readonly)
            return false;
    return true;
}

bool SystemInfo::isSwapActive(const QString &devPath) const
{
    if (const quint64 d = devNumber(devPath))
        if (m_swapsByDev.contains(d))
            return true;
    return m_swapsByPath.contains(devPath);
}

bool SystemInfo::hasHolders(const QString &devPath)
{
    const quint64 d = devNumber(devPath);
    if (!d)
        return false;
    const QString sys = QStringLiteral("/sys/dev/block/%1:%2/holders").arg(major(d)).arg(minor(d));
    QDir dir(sys);
    return dir.exists() && !dir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty();
}

bool SystemInfo::isExclusivelyBusy(const QString &devPath)
{
    if (!devNumber(devPath))
        return false;  // only block devices can be claimed
    const int fd = ::open(devPath.toLocal8Bit().constData(), O_RDONLY | O_EXCL | O_CLOEXEC | O_NONBLOCK);
    if (fd >= 0) {
        ::close(fd);
        return false;
    }
    return errno == EBUSY;
}

bool SystemInfo::isInUse(const QString &devPath)
{
    SystemInfo sys;
    sys.load();
    return sys.isMounted(devPath) || sys.isSwapActive(devPath) || hasHolders(devPath)
        || isExclusivelyBusy(devPath);
}

QStringList SystemInfo::kernelPartitions(const QString &diskPath)
{
    QStringList result;
    const quint64 d = devNumber(diskPath);
    if (!d)
        return result;
    const QString sysDir = QFileInfo(QStringLiteral("/sys/dev/block/%1:%2").arg(major(d)).arg(minor(d))).canonicalFilePath();
    const QStringList entries = QDir(sysDir).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &e : entries)
        if (QFileInfo::exists(sysDir + QLatin1Char('/') + e + QStringLiteral("/partition")))
            result.append(QStringLiteral("/dev/") + e);
    return result;
}

QStringList SystemInfo::fstabMountPoints(const QString &devPath, const QString &uuid, const QString &label) const
{
    QStringList result;
    const quint64 d = devNumber(devPath);
    for (const FstabEntry &e : m_fstab) {
        bool match = false;
        if (e.spec.startsWith(QLatin1String("UUID=")))
            match = !uuid.isEmpty() && e.spec.mid(5).remove(QLatin1Char('"')).compare(uuid, Qt::CaseInsensitive) == 0;
        else if (e.spec.startsWith(QLatin1String("LABEL=")))
            match = !label.isEmpty() && e.spec.mid(6).remove(QLatin1Char('"')) == label;
        else if (e.spec.startsWith(QLatin1Char('/')))
            match = e.spec == devPath || (d && devNumber(e.spec) == d);
        if (match && !result.contains(e.mountPoint))
            result.append(e.mountPoint);
    }
    return result;
}

QStringList SystemInfo::wholeDiskPaths()
{
    static const QRegularExpression line(
        QStringLiteral("^\\s*\\d+\\s+\\d+\\s+\\d+\\s+(\\S+)$"));
    static const QList<QRegularExpression> wholeDisk = {
        QRegularExpression(QStringLiteral("^[^0-9]+$")),
        QRegularExpression(QStringLiteral("^md[0-9]+$")),
        QRegularExpression(QStringLiteral("^mmcblk[0-9]+$")),
        QRegularExpression(QStringLiteral("^nvme[0-9]+n[0-9]+$")),
        QRegularExpression(QStringLiteral("^[a-z]+/c[0-9]+d[0-9]+$")),
        QRegularExpression(QStringLiteral("^bcache[0-9]+$")),
        QRegularExpression(QStringLiteral("^nbd[0-9]+$")),
    };
    static const QRegularExpression loop(QStringLiteral("^loop[0-9]+$"));

    QStringList paths;
    const QStringList lines = readTextFile(QStringLiteral("/proc/partitions")).split(QLatin1Char('\n'));
    for (const QString &l : lines) {
        const auto m = line.match(l);
        if (!m.hasMatch())
            continue;
        const QString name = m.captured(1);
        bool accept = false;
        for (const auto &re : wholeDisk)
            if (re.match(name).hasMatch()) {
                accept = true;
                break;
            }
        // Skip optical drives, ram and zram disks which match the generic pattern.
        if (accept && (name.startsWith(QLatin1String("sr")) || name.startsWith(QLatin1String("ram"))
                       || name.startsWith(QLatin1String("zram"))))
            accept = false;
        if (!accept && loop.match(name).hasMatch())
            accept = QFileInfo::exists(QStringLiteral("/sys/block/%1/loop/backing_file").arg(name));
        if (accept)
            paths.append(QStringLiteral("/dev/") + name);
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}

QString SystemInfo::serialNumber(const QString &devPath)
{
    if (Tools::has(QStringLiteral("udevadm"))) {
        const CommandResult r = runCommand(QStringLiteral("udevadm"),
                                           { QStringLiteral("info"), QStringLiteral("--query=property"),
                                             QStringLiteral("--name=") + devPath },
                                           nullptr, QByteArray(), 5000);
        static const QRegularExpression re(QStringLiteral("^ID_SERIAL_SHORT=(.*)$"),
                                           QRegularExpression::MultilineOption);
        const auto m = re.match(r.output);
        if (m.hasMatch())
            return m.captured(1).trimmed();
    }
    const QString name = QFileInfo(devPath).fileName();
    return readTextFile(QStringLiteral("/sys/block/%1/device/serial").arg(name)).trimmed();
}

} // namespace QtParted

/*
 * QtParted - a Qt frontend to libparted for manipulating disk partitions
 * Copyright (C) 2002-2026 Vanni Brutto <zanac4ever@gmail.com>
 *
 * Open LUKS mapping discovery and the in-memory passphrase store are ported from
 * GParted's LUKS_Info, luks.cc and PasswordRAMStore (https://gparted.org),
 * Copyright (C) 2015 Mike Fleetwood and the GParted contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "Luks.h"

#include "Command.h"
#include "OperationDetail.h"
#include "SystemInfo.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QHash>
#include <QMutex>

#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <sys/sysmacros.h>

namespace QtParted {

static QString tr(const char *s)
{
    return QCoreApplication::translate("Luks", s);
}

namespace Luks {

namespace {
QMutex g_mutex;
QList<CryptTarget> g_cache;
bool g_loaded = false;

QString commandError(const QString &program, const QStringList &args, const CommandResult &r)
{
    return QStringLiteral("# %1 %2\n%3").arg(program, args.join(QLatin1Char(' ')),
                                           (r.error + r.output).trimmed());
}
} // namespace

QList<CryptTarget> parseDmsetupTable(const QString &output)
{
    // name: start length crypt cipher key iv_offset device offset [options]
    QList<CryptTarget> result;
    for (const QString &line : output.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QStringList f = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (f.size() <= 8 || f[3] != QLatin1String("crypt"))
            continue;
        if (f[0].size() <= 1 || !f[0].endsWith(QLatin1Char(':')))
            continue;
        CryptTarget t;
        t.mapping.name = f[0].chopped(1);
        const QString &dev = f[7];
        if (dev.startsWith(QLatin1Char('/'))) {
            t.containerPath = dev;
        } else {
            const QStringList mm = dev.split(QLatin1Char(':'));
            bool ok1 = false, ok2 = false;
            if (mm.size() != 2)
                continue;
            const uint maj = mm[0].toUInt(&ok1), min = mm[1].toUInt(&ok2);
            if (!ok1 || !ok2)
                continue;
            t.containerDev = makedev(maj, min);
        }
        const qint64 offset = f[8].toLongLong();
        const qint64 length = f[2].toLongLong();
        if (offset <= 0 || length <= 0)
            continue;
        // Device-mapper always counts 512 byte sectors.
        t.mapping.offset = offset * 512;
        t.mapping.length = length * 512;
        t.mapping.keyLocation = f[5].startsWith(QLatin1Char(':')) ? LuksMapping::KeyLocation::KeyRing
                                                                  : LuksMapping::KeyLocation::DmCrypt;
        result.append(t);
    }
    return result;
}

void loadCache()
{
    QList<CryptTarget> targets;
    if (Tools::has(QStringLiteral("dmsetup"))) {
        const CommandResult r = runCommand(QStringLiteral("dmsetup"),
                                           { QStringLiteral("table"), QStringLiteral("--target"), QStringLiteral("crypt") });
        if (r.started)
            targets = parseDmsetupTable(r.output);
    }
    QMutexLocker lock(&g_mutex);
    g_cache = targets;
    g_loaded = true;
}

LuksMapping mapping(const QString &path)
{
    {
        QMutexLocker lock(&g_mutex);
        if (!g_loaded) {
            lock.unlock();
            loadCache();
        }
    }
    const quint64 dev = SystemInfo::devNumber(path);
    QMutexLocker lock(&g_mutex);
    for (const CryptTarget &t : g_cache) {
        if ((dev && t.containerDev == dev) || (!t.containerPath.isEmpty() && t.containerPath == path)
            || (dev && !t.containerPath.isEmpty() && SystemInfo::devNumber(t.containerPath) == dev))
            return t.mapping;
    }
    return LuksMapping();
}

QString mappingNameFor(const QString &path)
{
    return QFileInfo(path).fileName() + QStringLiteral("_crypt");
}

bool open(const Partition &p, const QByteArray &passphrase, QString &error)
{
    if (passphrase.isEmpty()) {
        error.clear();  // cryptsetup refuses an empty passphrase
        return false;
    }
    const QStringList args{ QStringLiteral("luksOpen"), p.path, mappingNameFor(p.path) };
    const CommandResult r = runCommand(QStringLiteral("cryptsetup"), args, nullptr, passphrase);
    if (r.ok()) {
        PasswordStore::store(p.uuid, passphrase);
        error.clear();
        loadCache();
        return true;
    }
    PasswordStore::erase(p.uuid);
    error = commandError(QStringLiteral("cryptsetup"), args, r);
    return false;
}

bool close(const Partition &p, QString &error)
{
    loadCache();
    const LuksMapping m = mapping(p.path);
    if (!m.isOpen()) {
        error = tr("%1 has no open encryption mapping").arg(p.path);
        return false;
    }
    const QStringList args{ QStringLiteral("luksClose"), m.name };
    const CommandResult r = runCommand(QStringLiteral("cryptsetup"), args);
    loadCache();
    if (r.ok()) {
        error.clear();
        return true;
    }
    error = commandError(QStringLiteral("cryptsetup"), args, r);
    return false;
}

bool testPassphrase(const Partition &p, const QByteArray &passphrase)
{
    if (passphrase.isEmpty())
        return false;
    return runCommand(QStringLiteral("cryptsetup"),
                      { QStringLiteral("luksOpen"), QStringLiteral("--test-passphrase"), p.path,
                        mappingNameFor(p.path) },
                      nullptr, passphrase)
        .ok();
}

bool resize(const Partition &p, bool fill, OperationDetail &detail)
{
    // Port of luks::resize().
    const LuksMapping m = mapping(p.path);
    if (!m.isOpen()) {
        if (!fill)
            return false;  // a closed LUKS volume can't be shrunk
        detail.addChild(tr("Maximize closed LUKS encryption skipped because it will automatically fill the "
                           "partition when opened"),
                        OperationDetail::Status::None, OperationDetail::Font::Italic);
        return true;
    }
    QStringList args{ QStringLiteral("-v") };
    if (!fill) {
        // cryptsetup sizes the mapping, not the partition, always in 512 byte sectors.
        args << QStringLiteral("--size") << QString::number((p.byteLength() - m.offset) / 512);
    }
    args << QStringLiteral("resize") << m.name;
    QByteArray pw;
    if (m.keyLocation == LuksMapping::KeyLocation::KeyRing)
        pw = PasswordStore::lookup(p.uuid);
    const bool ok = runCommand(QStringLiteral("cryptsetup"), args, &detail, pw).ok();
    loadCache();
    return ok;
}

} // namespace Luks

// --- passphrases ------------------------------------------------------------------

namespace PasswordStore {

namespace {
struct Secret {
    char *data = nullptr;
    size_t size = 0;
};

void wipe(Secret &s)
{
    if (!s.data)
        return;
    explicit_bzero(s.data, s.size);
    // No munlock: the page may still hold another passphrase.
    std::free(s.data);
    s.data = nullptr;
    s.size = 0;
}

struct Store {
    QMutex mutex;
    QHash<QString, Secret> secrets;
    ~Store()
    {
        for (Secret &s : secrets)
            wipe(s);
    }
};

Store &instance()
{
    static Store s;
    return s;
}
} // namespace

void store(const QString &uuid, const QByteArray &passphrase)
{
    if (uuid.isEmpty() || passphrase.isEmpty())
        return;
    Store &st = instance();
    QMutexLocker lock(&st.mutex);
    auto it = st.secrets.find(uuid);
    if (it != st.secrets.end())
        wipe(*it);
    Secret s;
    s.size = size_t(passphrase.size());
    s.data = static_cast<char *>(std::malloc(s.size));
    if (!s.data)
        return;
    mlock(s.data, s.size);  // best effort: keep it out of swap
    std::memcpy(s.data, passphrase.constData(), s.size);
    st.secrets.insert(uuid, s);
}

QByteArray lookup(const QString &uuid)
{
    Store &st = instance();
    QMutexLocker lock(&st.mutex);
    auto it = st.secrets.constFind(uuid);
    if (it == st.secrets.constEnd())
        return QByteArray();
    return QByteArray(it->data, qsizetype(it->size));
}

void erase(const QString &uuid)
{
    Store &st = instance();
    QMutexLocker lock(&st.mutex);
    auto it = st.secrets.find(uuid);
    if (it != st.secrets.end()) {
        wipe(*it);
        st.secrets.erase(it);
    }
}

void clear()
{
    Store &st = instance();
    QMutexLocker lock(&st.mutex);
    for (Secret &s : st.secrets)
        wipe(s);
    st.secrets.clear();
}

} // namespace PasswordStore
} // namespace QtParted

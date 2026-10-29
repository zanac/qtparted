/*
 * QtParted - a Qt frontend to libparted for manipulating disk partitions
 * Copyright (C) 2002-2026 Vanni Brutto <zanac4ever@gmail.com>
 *
 * Open LUKS mapping discovery and the in-memory passphrase store are ported from
 * GParted's LUKS_Info and PasswordRAMStore (https://gparted.org),
 * Copyright (C) 2015 Mike Fleetwood and the GParted contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#pragma once

#include "Partition.h"

#include <QList>

namespace QtParted {

class OperationDetail;

namespace Luks {

struct CryptTarget {
    QString containerPath;  // when dmsetup names the device by path
    quint64 containerDev = 0;  // major:minor otherwise
    LuksMapping mapping;
};

// Parse the output of "dmsetup table --target crypt".
QList<CryptTarget> parseDmsetupTable(const QString &output);

// Reload the open crypt mappings (called at every device scan and before changes).
void loadCache();
// The open mapping of the LUKS partition `path`, or an empty one.
LuksMapping mapping(const QString &path);

// "sdb1_crypt" for /dev/sdb1, like GParted.
QString mappingNameFor(const QString &path);

// Immediate actions (not queued), with their output in `error` on failure.
bool open(const Partition &partition, const QByteArray &passphrase, QString &error);
bool close(const Partition &partition, QString &error);
bool testPassphrase(const Partition &partition, const QByteArray &passphrase);

// "cryptsetup resize" of the mapping of `partition` to fill it (fill = true) or to
// the size of `partition` minus the header; uses the stored passphrase when the
// volume key is in the kernel keyring.
bool resize(const Partition &partition, bool fill, OperationDetail &detail);

} // namespace Luks

// Passphrases that opened a LUKS volume, by LUKS UUID, kept in locked memory that is
// wiped when removed or at exit (GParted's PasswordRAMStore).
namespace PasswordStore {
void store(const QString &uuid, const QByteArray &passphrase);
// Empty when unknown.
QByteArray lookup(const QString &uuid);
void erase(const QString &uuid);
void clear();
} // namespace PasswordStore

} // namespace QtParted

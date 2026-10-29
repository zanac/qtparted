/*
 * QtParted - a Qt frontend to libparted for manipulating disk partitions
 * Copyright (C) 2002-2026 Vanni Brutto <zanac4ever@gmail.com>
 *
 * The partition model follows GParted (https://gparted.org),
 * Copyright (C) 2004 Bart Hakvoort and the GParted contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#pragma once

#include "FsType.h"
#include "Units.h"

#include <QStringList>

#include <memory>
#include <vector>

namespace QtParted {

enum class PartitionType {
    Primary,
    Logical,
    Extended,
    Unallocated,    // free space inside a partition table
    Unpartitioned,  // whole disk device without a partition table
};

enum class PartitionStatus {
    Real,       // exists on disk
    New,        // will be created by a pending operation
    Copy,       // will be created by a pending paste
    Formatted,  // will be reformatted by a pending operation
};

enum class Alignment {
    Cylinder,
    MiB,
    None,  // exact sectors
};

struct Partition;
using PartitionVector = std::vector<Partition>;

// Value-semantic optional holder for a type that is still incomplete (a Partition
// inside a Partition): copies deep-copy the held object.
template <typename T>
class Boxed
{
public:
    Boxed() = default;
    Boxed(const Boxed &o) : m_p(o.m_p ? std::make_unique<T>(*o.m_p) : nullptr) {}
    Boxed(Boxed &&) noexcept = default;
    Boxed &operator=(const Boxed &o)
    {
        if (this != &o)
            m_p = o.m_p ? std::make_unique<T>(*o.m_p) : nullptr;
        return *this;
    }
    Boxed &operator=(Boxed &&) noexcept = default;
    ~Boxed() = default;

    explicit operator bool() const { return bool(m_p); }
    T &operator*() { return *m_p; }
    const T &operator*() const { return *m_p; }
    T *operator->() { return m_p.get(); }
    const T *operator->() const { return m_p.get(); }
    T &emplace(const T &value) { m_p = std::make_unique<T>(value); return *m_p; }
    void reset() { m_p.reset(); }

private:
    std::unique_ptr<T> m_p;
};

// An open dm-crypt mapping of a LUKS partition (port of GParted's LUKS_Mapping).
struct LuksMapping {
    enum class KeyLocation { Unknown, DmCrypt, KeyRing };
    QString name;            // /dev/mapper/<name>
    ByteValue offset = 0;    // start of the encrypted data in the partition
    ByteValue length = 0;    // size of the mapping
    KeyLocation keyLocation = KeyLocation::Unknown;

    bool isOpen() const { return !name.isEmpty(); }
    QString path() const { return QStringLiteral("/dev/mapper/") + name; }
};

struct Partition {
    QString devicePath;
    QString path;           // /dev/sda1, or "New Partition #1" for pending ones
    int number = -1;
    PartitionType type = PartitionType::Unallocated;
    PartitionStatus status = PartitionStatus::Real;
    Alignment alignment = Alignment::MiB;
    FsType fsType = FsType::Unallocated;

    QString fsLabel;
    QString uuid;
    QString name;           // partition name (GPT and other tables supporting names)
    QStringList flags;      // libparted flag names currently set
    QStringList availableFlags;  // libparted flag names this partition supports

    Sector start = -1;
    Sector end = -1;
    ByteValue sectorSize = 512;
    bool insideExtended = false;
    bool busy = false;
    bool fsReadonly = false;

    QStringList mountPoints;
    QStringList messages;

    // File system usage; -1 when unknown.
    Sector sectorsUsed = -1;
    Sector sectorsUnused = -1;
    Sector sectorsUnallocated = 0;  // partition space not used by the file system
    ByteValue fsBlockSize = -1;

    // Used by the dialogs creating or moving partitions.
    Sector freeSpaceBefore = -1;
    bool strictStart = false;

    PartitionVector logicals;  // only for Extended

    // LUKS: the open mapping and the file system inside it (GParted's PartitionLUKS).
    LuksMapping luks;
    Sector luksHeader = 0;      // sectors before the encrypted data
    Boxed<Partition> encrypted; // present while the mapping is open

    // --- helpers -----------------------------------------------------------
    Sector length() const { return end - start + 1; }
    ByteValue byteLength() const { return length() * sectorSize; }
    Sector middleSector() const { return (start + end) / 2; }

    bool usageKnown() const;
    // Usage as displayed (GParted's get_sectors_*): an open LUKS partition adds its
    // header to the usage of the file system inside; unallocated space below the
    // significant threshold counts as used.
    Sector usedSectors() const;
    Sector unusedSectors() const;
    Sector unallocatedSectors() const;

    // The partition directly holding the file system: the one inside an open LUKS
    // mapping, otherwise this one.
    bool isOpenLuks() const { return fsType == FsType::Luks && busy && bool(encrypted); }
    const Partition &filesystem() const { return isOpenLuks() ? *encrypted : *this; }
    Partition &filesystem() { return isOpenLuks() ? *encrypted : *this; }
    // LUKS: after start/end changed, the encrypted data and the mapping fill the
    // partition again (PartitionLUKS::resize).
    void luksFollowResize();
    // fsSize/fsUnused in sectors as reported by the file system tools (-1 = unknown size).
    void setUsage(Sector fsSize, Sector fsUnused);
    void clearUsage();
    // Smallest size the file system can be shrunk to (sectors), -1 when unknown.
    Sector estimatedMinSize() const;
    // Unallocated space inside the partition below this is considered intrinsic to the
    // file system (GParted's significant threshold).
    Sector significantUnallocatedThreshold() const;

    bool isFlagSet(const QString &flag) const { return flags.contains(flag); }
    void setOnlyFlag(const QString &flag);

    bool isUnallocated() const { return type == PartitionType::Unallocated || fsType == FsType::Unallocated; }
    bool isPartitionInTable() const
    {
        return type == PartitionType::Primary || type == PartitionType::Logical || type == PartitionType::Extended;
    }

    // Messages of this partition and, while open, of the file system inside the encryption.
    QStringList allMessages() const
    {
        return isOpenLuks() ? messages + encrypted->messages : messages;
    }

    QString displayPath() const;   // localized "unallocated" for free space
    QString mountPointsText() const { return mountPoints.join(QStringLiteral(", ")); }

    static Partition makeUnallocated(const QString &devicePath, Sector start, Sector end,
                                     ByteValue sectorSize, bool insideExtended);
    static Partition makeUnpartitioned(const QString &devicePath, const QString &path, FsType fsType,
                                       Sector length, ByteValue sectorSize, bool busy);

    // Same identity as GParted: device, number, start sector and type.
    bool operator==(const Partition &other) const
    {
        return devicePath == other.devicePath && number == other.number && start == other.start
            && type == other.type;
    }
    bool operator!=(const Partition &other) const { return !(*this == other); }
};

// Index of the extended partition in a vector, or -1.
int findExtended(const PartitionVector &partitions);

// Insert unallocated partitions for every gap larger than 1 MiB between [start, end].
void insertUnallocated(const QString &devicePath, PartitionVector &partitions, Sector start, Sector end,
                       ByteValue sectorSize, bool insideExtended);

} // namespace QtParted

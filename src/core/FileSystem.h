/*
 * QtParted - a Qt frontend to libparted for manipulating disk partitions
 * Copyright (C) 2002-2026 Vanni Brutto <zanac4ever@gmail.com>
 *
 * Commands, limits and usage parsing for each file system are ported from GParted
 * (https://gparted.org), Copyright (C) 2004 Bart Hakvoort and the GParted contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#pragma once

#include "FsType.h"
#include "Partition.h"

#include <QStringList>

#include <functional>
#include <map>
#include <memory>

namespace QtParted {

class OperationDetail;

struct FsLimits {
    ByteValue minSize = 0;  // 0 => no limit
    ByteValue maxSize = 0;  // 0 => no limit
};

// How a file system is resized, moved or copied.
enum class FsMethod {
    None,
    External,   // file system tool
    Libparted,  // libparted-fs-resize (FAT, HFS, HFS+)
    Block,      // QtParted's own block copy (move / copy)
    Recreate,   // recreated instead (linux-swap)
};

struct FsSupport {
    bool create = false;
    bool createWithLabel = false;
    bool readUsage = false;       // while not mounted
    bool readUsageOnline = true;  // statvfs() of the mount point
    FsMethod grow = FsMethod::None;
    FsMethod shrink = FsMethod::None;
    FsMethod onlineGrow = FsMethod::None;    // while mounted / active
    FsMethod onlineShrink = FsMethod::None;
    FsMethod move = FsMethod::None;
    FsMethod copy = FsMethod::None;
    bool check = false;
    bool writeLabel = false;
    bool onlineWriteLabel = false;
    bool writeUuid = false;

    bool canResize() const { return grow != FsMethod::None || shrink != FsMethod::None || move != FsMethod::None; }
    bool canResizeOnline() const { return onlineGrow != FsMethod::None || onlineShrink != FsMethod::None; }
};

class FileSystem
{
public:
    using Action = std::function<bool(const Partition &, OperationDetail &)>;

    struct Definition {
        FsType type;
        QString mkfsProgram;                                   // empty: cannot create
        std::function<QStringList(const Partition &)> mkfsArgs;
        bool labelAtCreate = true;
        QStringList usagePrograms;                             // all needed to read usage
        std::function<void(Partition &)> readUsage;            // unmounted usage reader
        FsLimits limits;
        QString software;                                      // packages providing the tools
        std::function<QString(const QString &)> sanitizeLabel; // optional

        // Resize: called with the partition at its new size.  `fill` grows the file
        // system to fill the partition, otherwise it is resized to the partition size.
        std::function<bool(const Partition &, OperationDetail &, bool fill)> resize;
        QStringList growPrograms;            // needed by resize when growing
        QStringList shrinkPrograms;          // needed by resize when shrinking
        bool shrinkNeedsUsage = true;        // the minimum size comes from the usage
        bool requireCheckForResize = false;  // GParted only offers resizing when it can check
        bool libpartedGrow = false;
        bool libpartedShrink = false;
        bool recreateResize = false;         // linux-swap
        bool onlineGrow = false;
        bool onlineShrink = false;

        QString checkProgram;
        std::function<QStringList(const Partition &)> checkArgs;
        QList<int> checkSuccessCodes{ 0 };

        Action writeLabel;
        QStringList labelPrograms;
        bool onlineLabel = false;

        Action writeUuid;
        QStringList uuidPrograms;

        // Moving and copying use QtParted's block copy unless stated otherwise.
        bool movable = true;
        bool copyable = true;
        bool moveNeedsCheck = false;  // GParted only moves/copies when it can check afterwards
    };

    explicit FileSystem(Definition def);

    FsType type() const { return m_def.type; }
    const FsSupport &support() const { return m_support; }
    const FsLimits &limits() const { return m_def.limits; }
    QString software() const { return m_def.software; }
    QString mkfsProgram() const { return m_def.mkfsProgram; }
    QStringList usagePrograms() const { return m_def.usagePrograms; }
    QStringList allPrograms() const;

    void detect();
    QString sanitizeLabel(const QString &label) const;
    bool create(const Partition &partition, OperationDetail &detail) const;
    void readUsage(Partition &partition) const;  // not mounted

    // External resize (FsMethod::External or Recreate).  `fill`: grow to fill the partition.
    bool resize(const Partition &partition, OperationDetail &detail, bool fill) const;
    bool check(const Partition &partition, OperationDetail &detail) const;
    bool writeLabel(const Partition &partition, OperationDetail &detail) const;
    bool writeUuid(const Partition &partition, OperationDetail &detail) const;

private:
    friend class FileSystemRegistry;
    Definition m_def;
    FsSupport m_support;
};

class FileSystemRegistry
{
public:
    static FileSystemRegistry &instance();

    void detect();  // look for the external tools again
    const FileSystem *get(FsType type) const;
    QList<const FileSystem *> all() const;
    FsSupport support(FsType type) const;  // empty support for unknown types

    // Fill in usage for a partition (mounted: statvfs, otherwise FS specific tool).
    void readUsage(Partition &partition) const;

private:
    FileSystemRegistry();
    void addResizeAndMaintenance();
    std::map<FsType, std::unique_ptr<FileSystem>> m_fs;
};

// statvfs() helper: returns false and sets error on failure.
bool mountedUsage(const QString &mountPoint, ByteValue &fsSize, ByteValue &fsFree, QString &error);

// Run `body` with the file system mounted: on its current mount point when busy,
// otherwise on a private temporary directory that is removed afterwards.
bool withMountedFileSystem(const Partition &partition, const QString &mountType, OperationDetail &detail,
                           const std::function<bool(const QString &mountPoint)> &body);

QString randomUuid();

} // namespace QtParted

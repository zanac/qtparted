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

struct FsSupport {
    bool create = false;
    bool createWithLabel = false;
    bool readUsage = false;      // while not mounted
    bool readUsageOnline = true; // statvfs() of the mount point
};

class FileSystem
{
public:
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
    };

    explicit FileSystem(Definition def);

    FsType type() const { return m_def.type; }
    const FsSupport &support() const { return m_support; }
    const FsLimits &limits() const { return m_def.limits; }
    QString software() const { return m_def.software; }
    QString mkfsProgram() const { return m_def.mkfsProgram; }
    QStringList usagePrograms() const { return m_def.usagePrograms; }

    void detect();
    QString sanitizeLabel(const QString &label) const;
    bool create(const Partition &partition, OperationDetail &detail) const;
    void readUsage(Partition &partition) const;  // not mounted

private:
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

    // Fill in usage for a partition (mounted: statvfs, otherwise FS specific tool).
    void readUsage(Partition &partition) const;

private:
    FileSystemRegistry();
    std::map<FsType, std::unique_ptr<FileSystem>> m_fs;
};

// statvfs() helper: returns false and sets error on failure.
bool mountedUsage(const QString &mountPoint, ByteValue &fsSize, ByteValue &fsFree, QString &error);

} // namespace QtParted

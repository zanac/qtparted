/*
 * QtParted - a Qt frontend to libparted for manipulating disk partitions
 * Copyright (C) 2002-2026 Vanni Brutto <zanac4ever@gmail.com>
 *
 * The pending operation model (visual re-application, merging) is ported from GParted
 * (https://gparted.org), Copyright (C) 2004 Bart Hakvoort and the GParted contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#pragma once

#include "Device.h"
#include "OperationDetail.h"
#include "Parted.h"

#include <memory>

namespace QtParted {

enum class OperationType { CreateTable, Create, Delete, Format, SetFlags, Name };

class Operation
{
public:
    virtual ~Operation() = default;
    Operation(const Operation &) = delete;
    Operation &operator=(const Operation &) = delete;

    OperationType type() const { return m_type; }
    const Device &device() const { return m_device; }
    Partition &original() { return m_original; }
    const Partition &original() const { return m_original; }
    Partition &partitionNew() { return m_new; }
    const Partition &partitionNew() const { return m_new; }
    QString description() const { return m_description; }
    OperationDetail &detail() { return *m_detail; }
    const OperationDetail &detail() const { return *m_detail; }

    virtual void createDescription() = 0;
    // Re-apply this pending operation to a copy of the device as scanned from disk.
    virtual void applyToVisual(Device &device) = 0;
    // Absorb a later operation on the same partition; true when merged.
    virtual bool mergeWith(const Operation &) { return false; }
    // Apply to disk.  Runs in the worker thread.
    bool apply();

protected:
    Operation(OperationType type, const Device &device, const Partition &original, const Partition &newPartition);
    virtual bool execute(OperationDetail &detail) = 0;

    int findIndexOriginal(const PartitionVector &partitions) const;
    int findIndexNew(const PartitionVector &partitions) const;
    void substituteNew(PartitionVector &partitions) const;
    void insertNew(PartitionVector &partitions) const;

    OperationType m_type;
    Device m_device;  // copy without partitions
    Partition m_original;
    Partition m_new;
    QString m_description;
    std::unique_ptr<OperationDetail> m_detail;
};

class OperationCreateTable : public Operation
{
public:
    OperationCreateTable(const Device &device, const QString &diskType, const Parted::DiskTypeInfo &info);
    QString diskType() const { return m_diskType; }
    void createDescription() override;
    void applyToVisual(Device &device) override;

protected:
    bool execute(OperationDetail &detail) override;

private:
    QString m_diskType;
    Parted::DiskTypeInfo m_info;
};

class OperationCreate : public Operation
{
public:
    OperationCreate(const Device &device, const Partition &unallocated, const Partition &newPartition);
    void createDescription() override;
    void applyToVisual(Device &device) override;
    bool mergeWith(const Operation &candidate) override;

protected:
    bool execute(OperationDetail &detail) override;
};

class OperationDelete : public Operation
{
public:
    OperationDelete(const Device &device, const Partition &partition);
    void createDescription() override;
    void applyToVisual(Device &device) override;

protected:
    bool execute(OperationDetail &detail) override;
};

class OperationFormat : public Operation
{
public:
    OperationFormat(const Device &device, const Partition &original, const Partition &newPartition);
    void createDescription() override;
    void applyToVisual(Device &device) override;
    bool mergeWith(const Operation &candidate) override;

protected:
    bool execute(OperationDetail &detail) override;
};

class OperationSetFlags : public Operation
{
public:
    OperationSetFlags(const Device &device, const Partition &original, const QStringList &flags);
    void createDescription() override;
    void applyToVisual(Device &device) override;
    bool mergeWith(const Operation &candidate) override;

protected:
    bool execute(OperationDetail &detail) override;
};

class OperationName : public Operation
{
public:
    OperationName(const Device &device, const Partition &original, const QString &name);
    void createDescription() override;
    void applyToVisual(Device &device) override;
    bool mergeWith(const Operation &candidate) override;

protected:
    bool execute(OperationDetail &detail) override;
};

// Create the file system of `p` (after its partition exists), with the detail entries GParted shows.
bool createFileSystem(const Partition &p, OperationDetail &detail);

} // namespace QtParted

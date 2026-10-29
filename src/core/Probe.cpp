/*
 * QtParted - a Qt frontend to libparted for manipulating disk partitions
 * Copyright (C) 2002-2026 Vanni Brutto <zanac4ever@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "Probe.h"

#include <blkid/blkid.h>

#include <fcntl.h>
#include <unistd.h>

namespace QtParted {

static ProbeResult doProbe(int fd, ByteValue offset, ByteValue size, bool withPartitionTables)
{
    ProbeResult r;
    blkid_probe pr = blkid_new_probe();
    if (!pr)
        return r;
    if (blkid_probe_set_device(pr, fd, offset, size) != 0) {
        blkid_free_probe(pr);
        return r;
    }
    blkid_probe_enable_superblocks(pr, 1);
    blkid_probe_set_superblocks_flags(pr, BLKID_SUBLKS_TYPE | BLKID_SUBLKS_LABEL | BLKID_SUBLKS_UUID
                                              | BLKID_SUBLKS_VERSION);
    blkid_probe_enable_partitions(pr, withPartitionTables ? 1 : 0);

    const int rc = blkid_do_safeprobe(pr);
    r.ok = rc >= -2 && rc != -1;
    r.ambivalent = rc == -2;
    if (rc == 0) {
        auto value = [&](const char *name) -> QString {
            const char *data = nullptr;
            if (blkid_probe_lookup_value(pr, name, &data, nullptr) == 0 && data)
                return QString::fromUtf8(data);
            return QString();
        };
        r.type = value("TYPE");
        r.version = value("VERSION");
        r.label = value("LABEL");
        r.uuid = value("UUID");
        r.ptType = value("PTTYPE");
        r.fsType = fsTypeFromBlkid(r.type, r.version);
    }
    blkid_free_probe(pr);
    return r;
}

ProbeResult Probe::path(const QString &p, bool withPartitionTables)
{
    const int fd = ::open(p.toLocal8Bit().constData(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        return ProbeResult();
    ProbeResult r = doProbe(fd, 0, 0, withPartitionTables);
    ::close(fd);
    return r;
}

ProbeResult Probe::range(const QString &devicePath, ByteValue offset, ByteValue size)
{
    const int fd = ::open(devicePath.toLocal8Bit().constData(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        return ProbeResult();
    ProbeResult r = doProbe(fd, offset, size, false);
    ::close(fd);
    return r;
}

} // namespace QtParted

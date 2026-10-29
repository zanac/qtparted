/*
 * QtParted - a Qt frontend to libparted for manipulating disk partitions
 * Copyright (C) 2002-2026 Vanni Brutto <zanac4ever@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "DialogFlags.h"

#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QLabel>
#include <QListWidget>
#include <QVBoxLayout>

namespace QtParted {

static QString flagDescription(const QString &flag)
{
    struct D { const char *flag; const char *text; };
    static const D descriptions[] = {
        { "boot", QT_TRANSLATE_NOOP("DialogFlags", "Bootable (MBR) / EFI System Partition (GPT)") },
        { "esp", QT_TRANSLATE_NOOP("DialogFlags", "EFI System Partition for UEFI boot") },
        { "bios_grub", QT_TRANSLATE_NOOP("DialogFlags", "GRUB core image for BIOS boot from GPT") },
        { "legacy_boot", QT_TRANSLATE_NOOP("DialogFlags", "Bootable by legacy BIOS (GPT)") },
        { "lba", QT_TRANSLATE_NOOP("DialogFlags", "LBA addressing (MBR)") },
        { "hidden", QT_TRANSLATE_NOOP("DialogFlags", "Hidden from the operating system") },
        { "msftdata", QT_TRANSLATE_NOOP("DialogFlags", "Microsoft basic data partition") },
        { "msftres", QT_TRANSLATE_NOOP("DialogFlags", "Microsoft reserved partition") },
        { "diag", QT_TRANSLATE_NOOP("DialogFlags", "Diagnostics / recovery partition") },
        { "lvm", QT_TRANSLATE_NOOP("DialogFlags", "Linux LVM physical volume") },
        { "raid", QT_TRANSLATE_NOOP("DialogFlags", "Linux software RAID member") },
        { "swap", QT_TRANSLATE_NOOP("DialogFlags", "Linux swap") },
        { "linux-home", QT_TRANSLATE_NOOP("DialogFlags", "Linux /home partition (discoverable partitions)") },
        { "no_automount", QT_TRANSLATE_NOOP("DialogFlags", "Do not mount automatically") },
        { "prep", QT_TRANSLATE_NOOP("DialogFlags", "PowerPC PReP boot partition") },
        { "irst", QT_TRANSLATE_NOOP("DialogFlags", "Intel Rapid Start Technology") },
        { "chromeos_kernel", QT_TRANSLATE_NOOP("DialogFlags", "ChromeOS kernel") },
        { "bls_boot", QT_TRANSLATE_NOOP("DialogFlags", "Boot Loader Specification extended boot partition") },
    };
    for (const D &d : descriptions)
        if (flag == QLatin1String(d.flag))
            return QCoreApplication::translate("DialogFlags", d.text);
    return QString();
}

DialogFlags::DialogFlags(const Partition &partition, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Manage flags on %1").arg(partition.path));
    m_list = new QListWidget(this);
    for (const QString &f : partition.availableFlags) {
        auto *item = new QListWidgetItem(f, m_list);
        const QString d = flagDescription(f);
        if (!d.isEmpty()) {
            item->setText(QStringLiteral("%1  —  %2").arg(f, d));
            item->setToolTip(d);
        }
        item->setData(Qt::UserRole, f);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable);
        item->setCheckState(partition.isFlagSet(f) ? Qt::Checked : Qt::Unchecked);
    }

    auto *note = new QLabel(tr("The change is added to the pending operations. Some partition tables "
                               "tie flags together (on GPT \"boot\" and \"esp\" are the same flag)."),
                            this);
    note->setWordWrap(true);
    note->setForegroundRole(QPalette::PlaceholderText);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *top = new QVBoxLayout(this);
    top->addWidget(m_list);
    top->addWidget(note);
    top->addWidget(buttons);
    resize(460, 420);
}

QStringList DialogFlags::flags() const
{
    QStringList f;
    for (int i = 0; i < m_list->count(); ++i)
        if (m_list->item(i)->checkState() == Qt::Checked)
            f.append(m_list->item(i)->data(Qt::UserRole).toString());
    return f;
}

} // namespace QtParted

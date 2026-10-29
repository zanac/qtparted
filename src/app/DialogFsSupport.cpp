/*
 * QtParted - a Qt frontend to libparted for manipulating disk partitions
 * Copyright (C) 2002-2026 Vanni Brutto <zanac4ever@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "DialogFsSupport.h"

#include "Command.h"
#include "FileSystem.h"
#include "UiHelpers.h"

#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace QtParted {

namespace {
enum Column { ColName, ColCreate, ColGrow, ColShrink, ColMove, ColCopy, ColCheck, ColLabel, ColUuid, ColUsage,
              ColTools, ColSoftware, ColumnCount };
}

DialogFsSupport::DialogFsSupport(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("File System Support"));

    m_table = new QTableWidget(this);
    m_table->setColumnCount(ColumnCount);
    m_table->setHorizontalHeaderLabels({ tr("File System"), tr("Create"), tr("Grow"), tr("Shrink"), tr("Move"),
                                         tr("Copy"), tr("Check"), tr("Label"), tr("UUID"), tr("Read usage"),
                                         tr("Tools"), tr("Software") });
    m_table->verticalHeader()->hide();
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->horizontalHeader()->setSectionResizeMode(ColTools, QHeaderView::Stretch);
    m_table->setAlternatingRowColors(true);

    auto *legend = new QLabel(tr("Struck out tools are missing: install the listed software and press "
                                 "<i>Rescan</i> to enable more actions. Hover a mark to see how an action is "
                                 "done and whether it also works on a mounted file system."),
                              this);
    legend->setWordWrap(true);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    QPushButton *rescan = buttons->addButton(tr("&Rescan For Supported Actions"), QDialogButtonBox::ActionRole);
    rescan->setIcon(Ui::icon("view-refresh", QStyle::SP_BrowserReload));
    connect(rescan, &QPushButton::clicked, this, [this]() {
        FileSystemRegistry::instance().detect();
        fill();
        Q_EMIT rescanned();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *top = new QVBoxLayout(this);
    top->addWidget(m_table);
    top->addWidget(legend);
    top->addWidget(buttons);

    fill();
    resize(1040, 640);
}

void DialogFsSupport::fill()
{
    const QIcon yes = Ui::icon("dialog-ok-apply", QStyle::SP_DialogApplyButton);
    const QIcon no = Ui::icon("dialog-cancel", QStyle::SP_DialogCancelButton);
    const auto fsList = FileSystemRegistry::instance().all();
    m_table->setRowCount(int(fsList.size()));
    int row = 0;
    for (const FileSystem *fs : fsList) {
        auto *name = new QTableWidgetItem(Ui::fsIcon(fs->type()), fsName(fs->type()));
        m_table->setItem(row, 0, name);

        auto mark = [&](bool ok, const QString &detail = QString()) {
            auto *i = new QTableWidgetItem(ok ? yes : no, QString());
            i->setToolTip(ok ? (detail.isEmpty() ? tr("Supported") : detail) : tr("Not available"));
            return i;
        };
        auto method = [&](FsMethod offline, FsMethod online) {
            QString how;
            switch (offline) {
            case FsMethod::Libparted: how = tr("Supported (libparted)"); break;
            case FsMethod::Recreate: how = tr("Supported (the file system is recreated)"); break;
            case FsMethod::Block: how = tr("Supported (block copy)"); break;
            default: how = tr("Supported"); break;
            }
            if (online != FsMethod::None)
                how += tr(", also while mounted");
            auto *i = mark(offline != FsMethod::None || online != FsMethod::None, how);
            if (offline == FsMethod::None && online != FsMethod::None)
                i->setToolTip(tr("Only while mounted"));
            return i;
        };
        const FsSupport &f = fs->support();
        m_table->setItem(row, ColCreate, mark(f.create));
        m_table->setItem(row, ColGrow, method(f.grow, f.onlineGrow));
        m_table->setItem(row, ColShrink, method(f.shrink, f.onlineShrink));
        m_table->setItem(row, ColMove, method(f.move, FsMethod::None));
        m_table->setItem(row, ColCopy, method(f.copy, FsMethod::None));
        m_table->setItem(row, ColCheck, mark(f.check));
        m_table->setItem(row, ColLabel, mark(f.writeLabel || f.onlineWriteLabel,
                                             f.onlineWriteLabel ? tr("Supported, also while mounted") : QString()));
        m_table->setItem(row, ColUuid, mark(f.writeUuid));
        m_table->setItem(row, ColUsage, mark(f.readUsage));

        QStringList tools;
        QStringList programs = fs->allPrograms();
        programs.removeDuplicates();
        for (const QString &p : programs)
            tools << (Tools::has(p) ? p : QStringLiteral("<s>%1</s>").arg(p));
        auto *toolsLabel = new QLabel(tools.join(QStringLiteral(", ")), m_table);
        toolsLabel->setContentsMargins(4, 0, 4, 0);
        m_table->setCellWidget(row, ColTools, toolsLabel);
        m_table->setItem(row, ColSoftware, new QTableWidgetItem(fs->software()));
        ++row;
    }
    m_table->resizeColumnsToContents();
    m_table->horizontalHeader()->setSectionResizeMode(ColTools, QHeaderView::Stretch);
}

} // namespace QtParted

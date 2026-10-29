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

DialogFsSupport::DialogFsSupport(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("File System Support"));

    m_table = new QTableWidget(this);
    m_table->setColumnCount(5);
    m_table->setHorizontalHeaderLabels({ tr("File System"), tr("Create"), tr("Read usage"), tr("Tools"),
                                         tr("Software") });
    m_table->verticalHeader()->hide();
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_table->setAlternatingRowColors(true);

    auto *legend = new QLabel(tr("Install the listed software and press <i>Rescan</i> to enable more file "
                                 "systems. Resizing, moving, copying, checking and relabelling come in the "
                                 "next QtParted versions."),
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
    resize(760, 600);
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

        auto mark = [&](bool ok) {
            auto *i = new QTableWidgetItem(ok ? yes : no, QString());
            i->setToolTip(ok ? tr("Supported") : tr("Not available"));
            return i;
        };
        m_table->setItem(row, 1, mark(fs->support().create));
        m_table->setItem(row, 2, mark(fs->support().readUsage));

        QStringList tools;
        QStringList programs;
        if (!fs->mkfsProgram().isEmpty())
            programs << fs->mkfsProgram();
        programs += fs->usagePrograms();
        programs.removeDuplicates();
        for (const QString &p : programs)
            tools << (Tools::has(p) ? p : QStringLiteral("<s>%1</s>").arg(p));
        auto *toolsLabel = new QLabel(tools.join(QStringLiteral(", ")), m_table);
        toolsLabel->setContentsMargins(4, 0, 4, 0);
        m_table->setCellWidget(row, 3, toolsLabel);
        m_table->setItem(row, 4, new QTableWidgetItem(fs->software()));
        ++row;
    }
    m_table->resizeColumnsToContents();
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
}

} // namespace QtParted

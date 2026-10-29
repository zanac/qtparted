/*
 * QtParted - a Qt frontend to libparted for manipulating disk partitions
 * Copyright (C) 2002-2026 Vanni Brutto <zanac4ever@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#pragma once

#include "Session.h"

#include <QMainWindow>

class QAction;
class QDockWidget;
class QFormLayout;
class QLabel;
class QListWidget;
class QMenu;
class QProgressBar;
class QTreeView;

namespace QtParted {

class DiskMapWidget;
class PartitionTreeModel;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(const QStringList &userPaths, QWidget *parent = nullptr);
    ~MainWindow() override;

    void refreshDevices();

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void createActions();
    void createMenus();
    void createToolBar();
    void createCentral();
    void createDocks();
    void installPrompter();

    void onScanFinished(std::vector<Device> devices);
    void selectDevice(int index);
    void refreshVisual();
    void updateDeviceList();
    void updateDeviceInfo();
    void updatePendingList();
    void updateActions();
    void updateBanner();
    void setSelectedKey(const QString &key, bool fromTree = false);
    const Partition *selectedPartition() const;
    const Partition *findDisplayPartition(const QString &key) const;
    void showPartitionMenu(const QPoint &globalPos);
    void addOperation(std::unique_ptr<Operation> operation);
    void setScanning(bool scanning, const QString &message = QString());
    int promptLibparted(int type, const QString &message, int options);

    void actionNew();
    void actionDelete();
    void actionFormat(FsType type);
    void actionName();
    void actionFlags();
    void actionInfo();
    void actionCreateTable();
    void actionUndo();
    void actionClear();
    void actionApply();
    void actionFsSupport();
    void actionAbout();

    Session m_session;
    int m_current = -1;
    Device m_display;
    QString m_selectedKey;
    QStringList m_userPaths;
    bool m_scanning = false;

    DiskMapWidget *m_map = nullptr;
    QTreeView *m_tree = nullptr;
    PartitionTreeModel *m_model = nullptr;
    QLabel *m_banner = nullptr;
    QListWidget *m_deviceList = nullptr;
    QWidget *m_deviceInfo = nullptr;
    QFormLayout *m_deviceInfoForm = nullptr;
    QListWidget *m_pending = nullptr;
    QDockWidget *m_devicesDock = nullptr;
    QDockWidget *m_pendingDock = nullptr;
    QLabel *m_statusLabel = nullptr;
    QProgressBar *m_busy = nullptr;

    QAction *m_actRefresh = nullptr;
    QAction *m_actQuit = nullptr;
    QAction *m_actUndo = nullptr;
    QAction *m_actClear = nullptr;
    QAction *m_actApply = nullptr;
    QAction *m_actFsSupport = nullptr;
    QAction *m_actCreateTable = nullptr;
    QAction *m_actNew = nullptr;
    QAction *m_actDelete = nullptr;
    QAction *m_actName = nullptr;
    QAction *m_actFlags = nullptr;
    QAction *m_actInfo = nullptr;
    QAction *m_actAbout = nullptr;
    QAction *m_actAboutQt = nullptr;
    QMenu *m_formatMenu = nullptr;
    QMenu *m_partitionMenu = nullptr;
};

} // namespace QtParted

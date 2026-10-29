/*
 * QtParted - a Qt frontend to libparted for manipulating disk partitions
 * Copyright (C) 2002-2026 Vanni Brutto <zanac4ever@gmail.com>
 *
 * Action rules and messages follow GParted's Win_GParted (https://gparted.org),
 * Copyright (C) 2004 Bart Hakvoort and the GParted contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "MainWindow.h"

#include "DialogApply.h"
#include "DialogCreateTable.h"
#include "DialogFlags.h"
#include "DialogFsSupport.h"
#include "DialogName.h"
#include "DialogNewPartition.h"
#include "BusyActions.h"
#include "Command.h"
#include "DialogPartitionInfo.h"
#include "DialogPassword.h"
#include "Luks.h"
#include "DialogResizeMove.h"
#include "DiskMapWidget.h"
#include "Layout.h"
#include "Parted.h"
#include "PartitionTreeModel.h"
#include "UiHelpers.h"
#include "Worker.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDockWidget>
#include <QEventLoop>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QThread>
#include <QTimer>
#include <QToolBar>
#include <QTreeView>
#include <QVBoxLayout>

#include <unistd.h>

namespace QtParted {

MainWindow::MainWindow(const QStringList &userPaths, QWidget *parent)
    : QMainWindow(parent)
    , m_userPaths(userPaths)
{
    setWindowTitle(QStringLiteral("QtParted"));
    setWindowIcon(QIcon::fromTheme(QStringLiteral("qtparted"), QIcon(QStringLiteral(":/icons/qtparted-64.png"))));

    createActions();
    createCentral();
    createDocks();
    createMenus();
    createToolBar();
    installPrompter();

    m_statusLabel = new QLabel(this);
    m_busy = new QProgressBar(this);
    m_busy->setRange(0, 0);
    m_busy->setMaximumWidth(140);
    m_busy->hide();
    statusBar()->addWidget(m_statusLabel, 1);
    statusBar()->addPermanentWidget(m_busy);

    QSettings settings;
    if (!restoreGeometry(settings.value(QStringLiteral("window/geometry")).toByteArray()))
        resize(1100, 680);
    if (!restoreState(settings.value(QStringLiteral("window/state")).toByteArray())) {
        // Dock sizes only apply once the window has its final geometry.
        QTimer::singleShot(0, this, [this]() {
            resizeDocks({ m_devicesDock }, { 240 }, Qt::Horizontal);
            resizeDocks({ m_pendingDock }, { 110 }, Qt::Vertical);
        });
    }

    updateBanner();
    updateActions();
}

MainWindow::~MainWindow()
{
    Parted::setPrompter({});
}

// --- construction -------------------------------------------------------------------

void MainWindow::createActions()
{
    m_actRefresh = new QAction(Ui::icon("view-refresh", QStyle::SP_BrowserReload), tr("&Refresh Devices"), this);
    m_actRefresh->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_R));
    connect(m_actRefresh, &QAction::triggered, this, &MainWindow::refreshDevices);

    m_actQuit = new QAction(Ui::icon("application-exit", QStyle::SP_DialogCloseButton), tr("&Quit"), this);
    m_actQuit->setShortcut(QKeySequence::Quit);
    connect(m_actQuit, &QAction::triggered, this, &QWidget::close);

    m_actUndo = new QAction(Ui::icon("edit-undo", QStyle::SP_ArrowBack), tr("&Undo Last Operation"), this);
    m_actUndo->setShortcut(QKeySequence::Undo);
    connect(m_actUndo, &QAction::triggered, this, &MainWindow::actionUndo);

    m_actClear = new QAction(Ui::icon("edit-clear-all", QStyle::SP_DialogResetButton), tr("&Clear All Operations"), this);
    connect(m_actClear, &QAction::triggered, this, &MainWindow::actionClear);

    m_actApply = new QAction(Ui::icon("dialog-ok-apply", QStyle::SP_DialogApplyButton), tr("&Apply All Operations"), this);
    m_actApply->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return));
    connect(m_actApply, &QAction::triggered, this, &MainWindow::actionApply);

    m_actFsSupport = new QAction(Ui::icon("preferences-system", QStyle::SP_FileDialogDetailedView),
                                 tr("&File System Support"), this);
    connect(m_actFsSupport, &QAction::triggered, this, &MainWindow::actionFsSupport);

    m_actCreateTable = new QAction(Ui::icon("document-new", QStyle::SP_FileIcon), tr("&Create Partition Table..."), this);
    connect(m_actCreateTable, &QAction::triggered, this, &MainWindow::actionCreateTable);

    m_actNew = new QAction(Ui::icon("list-add", QStyle::SP_FileIcon), tr("&New"), this);
    m_actNew->setShortcut(QKeySequence(Qt::Key_Insert));
    m_actNew->setToolTip(tr("Create a new partition in the selected unallocated space"));
    connect(m_actNew, &QAction::triggered, this, &MainWindow::actionNew);

    m_actDelete = new QAction(Ui::icon("edit-delete", QStyle::SP_TrashIcon), tr("&Delete"), this);
    m_actDelete->setShortcut(QKeySequence::Delete);
    m_actDelete->setToolTip(tr("Delete the selected partition"));
    connect(m_actDelete, &QAction::triggered, this, &MainWindow::actionDelete);

    m_actResize = new QAction(Ui::icon("transform-scale", QStyle::SP_ArrowRight), tr("&Resize/Move"), this);
    m_actResize->setToolTip(tr("Resize or move the selected partition"));
    connect(m_actResize, &QAction::triggered, this, &MainWindow::actionResizeMove);

    m_actCopy = new QAction(Ui::icon("edit-copy", QStyle::SP_FileIcon), tr("&Copy"), this);
    m_actCopy->setShortcut(QKeySequence::Copy);
    m_actCopy->setToolTip(tr("Copy the selected partition to the clipboard"));
    connect(m_actCopy, &QAction::triggered, this, &MainWindow::actionCopy);

    m_actPaste = new QAction(Ui::icon("edit-paste", QStyle::SP_FileIcon), tr("&Paste"), this);
    m_actPaste->setShortcut(QKeySequence::Paste);
    m_actPaste->setToolTip(tr("Paste the copied partition into the selection"));
    connect(m_actPaste, &QAction::triggered, this, &MainWindow::actionPaste);

    m_actCheck = new QAction(Ui::icon("emblem-checked", QStyle::SP_DialogApplyButton), tr("C&heck"), this);
    m_actCheck->setToolTip(tr("Check and repair the file system, then grow it to fill the partition"));
    connect(m_actCheck, &QAction::triggered, this, &MainWindow::actionCheck);

    m_actLabel = new QAction(Ui::icon("tag", QStyle::SP_FileDialogContentsView), tr("&Label File System"), this);
    connect(m_actLabel, &QAction::triggered, this, &MainWindow::actionLabel);

    m_actUuid = new QAction(Ui::icon("view-refresh", QStyle::SP_BrowserReload), tr("New UU&ID"), this);
    connect(m_actUuid, &QAction::triggered, this, &MainWindow::actionNewUuid);

    m_actCrypt = new QAction(Ui::icon("unlock", QStyle::SP_DialogOkButton), tr("&Open Encryption"), this);
    connect(m_actCrypt, &QAction::triggered, this, &MainWindow::actionToggleCrypt);

    m_actBusy = new QAction(Ui::icon("media-mount", QStyle::SP_DriveHDIcon), tr("&Unmount"), this);
    connect(m_actBusy, &QAction::triggered, this, &MainWindow::actionToggleBusy);

    m_mountMenu = new QMenu(tr("&Mount on"), this);
    m_mountMenu->setIcon(Ui::icon("media-mount", QStyle::SP_DriveHDIcon));

    m_actName = new QAction(Ui::icon("edit-rename", QStyle::SP_FileDialogContentsView), tr("&Name Partition"), this);
    connect(m_actName, &QAction::triggered, this, &MainWindow::actionName);

    m_actFlags = new QAction(Ui::icon("flag", QStyle::SP_FileDialogListView), tr("Manage F&lags"), this);
    connect(m_actFlags, &QAction::triggered, this, &MainWindow::actionFlags);

    m_actInfo = new QAction(Ui::icon("dialog-information", QStyle::SP_MessageBoxInformation), tr("&Information"), this);
    m_actInfo->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Return));
    connect(m_actInfo, &QAction::triggered, this, &MainWindow::actionInfo);

    m_actAbout = new QAction(QIcon(QStringLiteral(":/icons/qtparted-64.png")), tr("&About QtParted"), this);
    connect(m_actAbout, &QAction::triggered, this, &MainWindow::actionAbout);
    m_actAboutQt = new QAction(tr("About &Qt"), this);
    connect(m_actAboutQt, &QAction::triggered, qApp, &QApplication::aboutQt);

    m_formatMenu = new QMenu(tr("&Format to"), this);
    m_formatMenu->setIcon(Ui::icon("edit-clear-all", QStyle::SP_DriveHDIcon));
    QList<FsType> formats = creatableFsTypes();
    formats.append(FsType::Cleared);
    for (FsType t : formats) {
        QAction *a = m_formatMenu->addAction(Ui::fsIcon(t), fsName(t));
        a->setData(int(t));
        connect(a, &QAction::triggered, this, [this, t]() { actionFormat(t); });
    }
    m_formatMenu->setToolTipsVisible(true);
}

void MainWindow::createMenus()
{
    QMenu *app = menuBar()->addMenu(QStringLiteral("&QtParted"));
    app->addAction(m_actRefresh);
    app->addSeparator();
    app->addAction(m_actQuit);

    QMenu *edit = menuBar()->addMenu(tr("&Edit"));
    edit->addAction(m_actUndo);
    edit->addAction(m_actClear);
    edit->addAction(m_actApply);

    QMenu *view = menuBar()->addMenu(tr("&View"));
    QAction *devices = m_devicesDock->toggleViewAction();
    devices->setText(tr("Device &Information"));
    view->addAction(devices);
    QAction *pending = m_pendingDock->toggleViewAction();
    pending->setText(tr("Pending &Operations"));
    view->addAction(pending);
    view->addSeparator();
    view->addAction(m_actFsSupport);

    QMenu *device = menuBar()->addMenu(tr("&Device"));
    device->addAction(m_actCreateTable);

    m_partitionMenu = menuBar()->addMenu(tr("&Partition"));
    m_partitionMenu->addAction(m_actNew);
    m_partitionMenu->addAction(m_actDelete);
    m_partitionMenu->addSeparator();
    m_partitionMenu->addAction(m_actResize);
    m_partitionMenu->addSeparator();
    m_partitionMenu->addAction(m_actCopy);
    m_partitionMenu->addAction(m_actPaste);
    m_partitionMenu->addSeparator();
    m_partitionMenu->addMenu(m_formatMenu);
    m_partitionMenu->addSeparator();
    m_partitionMenu->addAction(m_actName);
    m_partitionMenu->addAction(m_actFlags);
    m_partitionMenu->addAction(m_actCheck);
    m_partitionMenu->addAction(m_actLabel);
    m_partitionMenu->addAction(m_actUuid);
    m_partitionMenu->addSeparator();
    m_partitionMenu->addAction(m_actCrypt);
    m_partitionMenu->addAction(m_actBusy);
    m_mountMenuAction = m_partitionMenu->addMenu(m_mountMenu);
    m_partitionMenu->addSeparator();
    m_partitionMenu->addAction(m_actInfo);

    QMenu *help = menuBar()->addMenu(tr("&Help"));
    help->addAction(m_actAbout);
    help->addAction(m_actAboutQt);
}

void MainWindow::createToolBar()
{
    QToolBar *tb = addToolBar(tr("Main Toolbar"));
    tb->setObjectName(QStringLiteral("mainToolBar"));
    tb->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    tb->addAction(m_actNew);
    tb->addAction(m_actDelete);
    tb->addAction(m_actResize);
    tb->addSeparator();
    tb->addAction(m_actCopy);
    tb->addAction(m_actPaste);
    tb->addSeparator();
    tb->addAction(m_actUndo);
    tb->addAction(m_actApply);
    tb->addSeparator();
    tb->addAction(m_actRefresh);
}

void MainWindow::createCentral()
{
    auto *central = new QWidget(this);
    auto *layout = new QVBoxLayout(central);
    layout->setContentsMargins(6, 6, 6, 0);

    m_banner = new QLabel(central);
    m_banner->setWordWrap(true);
    m_banner->setContentsMargins(10, 6, 10, 6);
    m_banner->setAutoFillBackground(true);
    m_banner->hide();
    layout->addWidget(m_banner);

    m_map = new DiskMapWidget(central);
    layout->addWidget(m_map);

    m_model = new PartitionTreeModel(this);
    m_tree = new QTreeView(central);
    m_tree->setModel(m_model);
    m_tree->setRootIsDecorated(true);
    m_tree->setUniformRowHeights(true);
    m_tree->setAlternatingRowColors(true);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->setAllColumnsShowFocus(true);
    m_tree->header()->setStretchLastSection(true);
    layout->addWidget(m_tree, 1);
    setCentralWidget(central);

    connect(m_map, &DiskMapWidget::partitionSelected, this, [this](const QString &k) { setSelectedKey(k); });
    connect(m_map, &DiskMapWidget::partitionActivated, this, [this](const QString &k) {
        setSelectedKey(k);
        actionInfo();
    });
    connect(m_map, &DiskMapWidget::contextMenuRequested, this, &MainWindow::showPartitionMenu);
    connect(m_tree->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            [this](const QModelIndex &current) {
                if (current.isValid())
                    setSelectedKey(m_model->index(current.row(), 0, current.parent())
                                       .data(PartitionTreeModel::KeyRole).toString(),
                                   true);
            });
    connect(m_tree, &QTreeView::doubleClicked, this, [this](const QModelIndex &) { actionInfo(); });
    connect(m_tree, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        const QModelIndex i = m_tree->indexAt(pos);
        if (!i.isValid())
            return;
        setSelectedKey(i.siblingAtColumn(0).data(PartitionTreeModel::KeyRole).toString());
        showPartitionMenu(m_tree->viewport()->mapToGlobal(pos));
    });
}

void MainWindow::createDocks()
{
    // Devices: QtParted's classic navigator with GParted's device information under it.
    m_devicesDock = new QDockWidget(tr("Devices"), this);
    m_devicesDock->setObjectName(QStringLiteral("devicesDock"));
    auto *split = new QSplitter(Qt::Vertical, m_devicesDock);
    m_deviceList = new QListWidget(split);
    m_deviceList->setIconSize(QSize(32, 32));
    m_deviceList->setSpacing(1);
    connect(m_deviceList, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row >= 0 && row != m_current)
            selectDevice(row);
    });
    auto *scroll = new QScrollArea(split);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    m_deviceInfo = new QWidget(scroll);
    m_deviceInfoForm = new QFormLayout(m_deviceInfo);
    m_deviceInfoForm->setLabelAlignment(Qt::AlignRight);
    scroll->setWidget(m_deviceInfo);
    split->addWidget(m_deviceList);
    split->addWidget(scroll);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 2);
    split->setSizes({ 150, 330 });
    m_devicesDock->setWidget(split);
    addDockWidget(Qt::LeftDockWidgetArea, m_devicesDock);

    m_pendingDock = new QDockWidget(tr("Pending Operations"), this);
    m_pendingDock->setObjectName(QStringLiteral("pendingDock"));
    m_pending = new QListWidget(m_pendingDock);
    m_pending->setSelectionMode(QAbstractItemView::NoSelection);
    m_pendingDock->setWidget(m_pending);
    addDockWidget(Qt::BottomDockWidgetArea, m_pendingDock);
}

void MainWindow::installPrompter()
{
    Parted::setPrompter([this](int type, const QString &message, int options) -> int {
        if (QThread::currentThread() == thread())
            return promptLibparted(type, message, options);
        int answer = 0;
        QMetaObject::invokeMethod(
            this, [&]() { answer = promptLibparted(type, message, options); }, Qt::BlockingQueuedConnection);
        return answer;
    });
}

int MainWindow::promptLibparted(int type, const QString &message, int options)
{
    QMessageBox box(this);
    box.setWindowTitle(Parted::exceptionTypeName(type));
    box.setText(message);
    box.setIcon(QMessageBox::Warning);
    QHash<QAbstractButton *, int> map;
    int cancel = 0;
    for (const auto &[opt, name] : Parted::exceptionOptions(options)) {
        QString label = name;
        if (!label.isEmpty())
            label[0] = label[0].toUpper();
        QPushButton *b = box.addButton(label, QMessageBox::ActionRole);
        map.insert(b, opt);
        if (name.compare(QLatin1String("cancel"), Qt::CaseInsensitive) == 0) {
            cancel = opt;
            box.setEscapeButton(b);
        }
    }
    box.exec();
    if (QAbstractButton *clicked = box.clickedButton())
        return map.value(clicked, cancel);
    return cancel;
}

// --- scanning ---------------------------------------------------------------------------

void MainWindow::setScanning(bool scanning, const QString &message)
{
    m_scanning = scanning;
    m_busy->setVisible(scanning);
    if (!message.isEmpty() || scanning)
        m_statusLabel->setText(message);
    centralWidget()->setEnabled(!scanning);
    m_devicesDock->setEnabled(!scanning);
    updateActions();
}

void MainWindow::refreshDevices()
{
    if (m_scanning)
        return;
    setScanning(true, tr("Scanning all devices..."));
    auto result = std::make_shared<std::vector<Device>>();
    const QStringList paths = m_userPaths;
    QThread *thread = QThread::create([this, result, paths]() {
        *result = scanDevices(paths, [this](const QString &msg) {
            QMetaObject::invokeMethod(this, [this, msg]() { m_statusLabel->setText(msg); }, Qt::QueuedConnection);
        });
    });
    connect(thread, &QThread::finished, this, [this, thread, result]() {
        thread->deleteLater();
        onScanFinished(std::move(*result));
    });
    thread->start();
}

void MainWindow::onScanFinished(std::vector<Device> devices)
{
    const QString previous = (m_current >= 0 && m_current < int(m_session.devices.size()))
                                 ? m_session.devices[size_t(m_current)].path
                                 : QString();
    m_session.devices = std::move(devices);
    // Pending operations for devices that disappeared are dropped.
    for (auto it = m_session.operations.begin(); it != m_session.operations.end();)
        if (m_session.deviceIndex((*it)->device().path) < 0)
            it = m_session.operations.erase(it);
        else
            ++it;

    setScanning(false);
    updateDeviceList();
    int index = m_session.deviceIndex(previous);
    if (index < 0 && !m_session.devices.empty())
        index = 0;
    m_current = -1;
    selectDevice(index);

    if (m_session.devices.empty()) {
        m_statusLabel->setText(tr("No devices detected"));
        if (geteuid() == 0)
            QMessageBox::information(this, tr("No devices detected"),
                                     tr("QtParted found no disk devices. Disk image files can be opened by "
                                        "passing their path on the command line."));
    }
}

void MainWindow::updateDeviceList()
{
    const QSignalBlocker block(m_deviceList);
    m_deviceList->clear();
    for (const Device &d : m_session.devices) {
        const char *iconName = d.isImageFile ? "application-x-raw-disk-image"
                             : d.path.startsWith(QLatin1String("/dev/loop")) ? "drive-removable-media"
                                                                             : "drive-harddisk";
        auto *item = new QListWidgetItem(Ui::icon(iconName, QStyle::SP_DriveHDIcon),
                                         QStringLiteral("%1\n%2").arg(d.path, formatSize(d.length, d.sectorSize)),
                                         m_deviceList);
        item->setToolTip(d.model.isEmpty() ? d.path : d.model);
    }
}

void MainWindow::selectDevice(int index)
{
    m_current = index;
    {
        const QSignalBlocker block(m_deviceList);
        m_deviceList->setCurrentRow(index);
    }
    m_selectedKey.clear();
    refreshVisual();
}

// --- visuals ----------------------------------------------------------------------------

void MainWindow::refreshVisual()
{
    m_display = m_session.displayDevice(m_current);
    // Like GParted, the clipboard follows the source partition and is dropped when the
    // partition it holds is gone (deleted, moved, renumbered or rescanned differently).
    if (m_clipboard) {
        const Device source = m_session.displayDevice(m_session.deviceIndex(m_clipboard->devicePath));
        const Partition *found = nullptr;
        for (const Partition &p : source.partitions) {
            if (p.path == m_clipboard->path && p.start == m_clipboard->start && p.end == m_clipboard->end
                && p.status == PartitionStatus::Real)
                found = &p;
            for (const Partition &l : p.logicals)
                if (l.path == m_clipboard->path && l.start == m_clipboard->start && l.end == m_clipboard->end
                    && l.status == PartitionStatus::Real)
                    found = &l;
        }
        if (found && found->fsType == m_clipboard->fsType)
            m_clipboard = *found;
        else
            m_clipboard.reset();
    }
    m_map->setDevice(m_display);
    m_model->setDevice(m_display);
    m_tree->expandAll();
    m_tree->setColumnHidden(PartitionTreeModel::ColName, !m_display.partitionNamingSupported());
    for (int c = 0; c < PartitionTreeModel::ColumnCount - 1; ++c)
        m_tree->resizeColumnToContents(c);

    setWindowTitle(m_current >= 0 ? QStringLiteral("%1 - QtParted").arg(m_display.path) : QStringLiteral("QtParted"));
    updateDeviceInfo();
    updatePendingList();
    updateBanner();

    // Keep the selection, or select the largest unallocated space like GParted.
    QString key = findDisplayPartition(m_selectedKey) ? m_selectedKey : QString();
    if (key.isEmpty()) {
        Sector largest = -1;
        for (const Partition &p : m_display.partitions) {
            if (p.fsType == FsType::Unallocated && p.length() > largest) {
                largest = p.length();
                key = Ui::partitionKey(p);
            }
            for (const Partition &l : p.logicals)
                if (l.fsType == FsType::Unallocated && l.length() > largest) {
                    largest = l.length();
                    key = Ui::partitionKey(l);
                }
        }
    }
    m_selectedKey.clear();
    setSelectedKey(key);
}

void MainWindow::updateDeviceInfo()
{
    while (m_deviceInfoForm->rowCount() > 0)
        m_deviceInfoForm->removeRow(0);
    if (m_current < 0)
        return;
    const Device &d = m_display;
    auto add = [this](const QString &label, const QString &value) {
        auto *v = new QLabel(value.isEmpty() ? QStringLiteral("—") : value, m_deviceInfo);
        v->setTextInteractionFlags(Qt::TextSelectableByMouse);
        v->setWordWrap(true);
        m_deviceInfoForm->addRow(QStringLiteral("<b>%1</b>").arg(label), v);
    };
    add(tr("Model:"), d.model);
    add(tr("Serial:"), d.serial);
    add(tr("Size:"), formatSize(d.length, d.sectorSize));
    add(tr("Path:"), d.path);
    add(tr("Partition table:"), d.diskType);
    add(tr("Heads:"), QString::number(d.heads));
    add(tr("Sectors/track:"), QString::number(d.sectorsPerTrack));
    add(tr("Cylinders:"), QString::number(d.cylinders));
    add(tr("Total sectors:"), QLocale().toString(d.length));
    add(tr("Sector size:"), QString::number(d.sectorSize));
}

void MainWindow::updatePendingList()
{
    m_pending->clear();
    for (const auto &op : m_session.operations) {
        const char *iconName = "dialog-ok";
        QStyle::StandardPixmap fallback = QStyle::SP_FileIcon;
        switch (op->type()) {
        case OperationType::CreateTable: iconName = "document-new"; break;
        case OperationType::Create:      iconName = "list-add"; break;
        case OperationType::Delete:      iconName = "edit-delete"; fallback = QStyle::SP_TrashIcon; break;
        case OperationType::Format:      iconName = "edit-clear-all"; fallback = QStyle::SP_DriveHDIcon; break;
        case OperationType::SetFlags:    iconName = "flag"; break;
        case OperationType::Name:        iconName = "edit-rename"; break;
        case OperationType::ResizeMove:  iconName = "transform-scale"; fallback = QStyle::SP_ArrowRight; break;
        case OperationType::Copy:        iconName = "edit-paste"; break;
        case OperationType::Check:       iconName = "emblem-checked"; fallback = QStyle::SP_DialogApplyButton; break;
        case OperationType::Label:       iconName = "tag"; break;
        case OperationType::ChangeUuid:  iconName = "view-refresh"; fallback = QStyle::SP_BrowserReload; break;
        }
        new QListWidgetItem(Ui::icon(iconName, fallback), op->description(), m_pending);
    }
    const int n = int(m_session.operations.size());
    if (!m_scanning) {
        if (n == 0)
            m_statusLabel->setText(tr("No operations pending"));
        else if (n == 1)
            m_statusLabel->setText(tr("1 operation pending"));
        else
            m_statusLabel->setText(tr("%1 operations pending").arg(n));
    }
    if (n > 0 && !m_pendingDock->isVisible())
        m_pendingDock->show();
}

void MainWindow::updateBanner()
{
    QString text;
    bool warning = false;
    if (geteuid() != 0) {
        text = tr("QtParted is not running with root privileges: disk devices are not accessible. "
                  "Start it with the <b>qtparted</b> launcher (pkexec) or as root.");
        warning = true;
    } else if (m_current >= 0 && m_display.isImageFile) {
        text = tr("%1 is a disk image file: the partition table can be edited, but file systems can only be "
                  "created on a block device (attach it with <tt>losetup -P</tt>).")
                   .arg(m_display.path.toHtmlEscaped());
    } else if (m_current >= 0 && m_display.readonly) {
        text = tr("Partitions of %1 are in use and the kernel can not be informed of changes: only "
                  "informational actions are possible until they are unmounted.")
                   .arg(m_display.path.toHtmlEscaped());
        warning = true;
    }
    m_banner->setVisible(!text.isEmpty());
    m_banner->setText(text);
    QPalette pal = m_banner->palette();
    const bool dark = Ui::isDarkPalette(palette());
    const QColor bg = warning ? (dark ? QColor(0x5c, 0x45, 0x10) : QColor(0xfd, 0xf1, 0xcf))
                              : (dark ? QColor(0x1d, 0x3d, 0x5c) : QColor(0xdb, 0xea, 0xf8));
    pal.setColor(QPalette::Window, bg);
    pal.setColor(QPalette::WindowText, palette().color(QPalette::WindowText));
    m_banner->setPalette(pal);
}

const Partition *MainWindow::findDisplayPartition(const QString &key) const
{
    if (key.isEmpty())
        return nullptr;
    for (const Partition &p : m_display.partitions) {
        if (Ui::partitionKey(p) == key)
            return &p;
        for (const Partition &l : p.logicals)
            if (Ui::partitionKey(l) == key)
                return &l;
    }
    return nullptr;
}

const Partition *MainWindow::selectedPartition() const
{
    return findDisplayPartition(m_selectedKey);
}

void MainWindow::setSelectedKey(const QString &key, bool fromTree)
{
    if (key == m_selectedKey)
        return;
    m_selectedKey = key;
    m_map->setSelectedKey(key);
    if (!fromTree) {
        const QModelIndex index = m_model->indexForKey(key);
        const QSignalBlocker block(m_tree->selectionModel());
        if (index.isValid()) {
            m_tree->selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect
                                                                 | QItemSelectionModel::Rows);
            m_tree->scrollTo(index);
        } else {
            m_tree->selectionModel()->clearSelection();
        }
    }
    updateActions();
}

void MainWindow::updateActions()
{
    const bool idle = !m_scanning;
    const bool hasOps = !m_session.operations.empty();
    m_actUndo->setEnabled(idle && hasOps);
    m_actClear->setEnabled(idle && hasOps);
    m_actApply->setEnabled(idle && hasOps);
    m_actRefresh->setEnabled(idle);
    m_actFsSupport->setEnabled(idle);  // its rescan must not race with a device scan

    const Partition *sel = selectedPartition();
    AllowedActions a;
    if (idle && m_current >= 0)
        a = Session::allowedActions(m_display, sel, m_session.pendingCount(m_display.path),
                                    m_clipboard ? &*m_clipboard : nullptr);
    m_actCreateTable->setEnabled(a.createTable);
    m_actNew->setEnabled(a.create);
    m_actDelete->setEnabled(a.remove);
    m_actResize->setEnabled(a.resize);
    m_actCopy->setEnabled(a.copy);
    m_actPaste->setEnabled(a.paste);
    m_actCheck->setEnabled(a.check);
    m_actLabel->setEnabled(a.label);
    m_actUuid->setEnabled(a.uuid);
    m_actName->setEnabled(a.name);

    // Open/close encryption, then mount/unmount, swapon/swapoff or VG (de)activation;
    // "Mount on" replaces the toggle for an unmounted file system with fstab entries.
    m_actCrypt->setText(a.cryptIsOpen ? tr("Close Encryption") : tr("Open Encryption"));
    m_actCrypt->setIcon(Ui::icon(a.cryptIsOpen ? "lock" : "unlock", QStyle::SP_DialogOkButton));
    m_actCrypt->setEnabled(a.openCrypt);
    m_actCrypt->setVisible(sel && sel->fsType == FsType::Luks);
    QString toggleText = tr("&Unmount");
    switch (a.toggle) {
    case AllowedActions::Toggle::SwapOn:       toggleText = tr("&Swapon"); break;
    case AllowedActions::Toggle::SwapOff:      toggleText = tr("&Swapoff"); break;
    case AllowedActions::Toggle::ActivateVg:   toggleText = tr("&Activate"); break;
    case AllowedActions::Toggle::DeactivateVg: toggleText = tr("&Deactivate"); break;
    default: break;
    }
    m_actBusy->setText(toggleText);
    m_actBusy->setEnabled(a.toggle != AllowedActions::Toggle::None);
    m_mountMenu->clear();
    for (const QString &mp : a.mountOn) {
        QAction *act = m_mountMenu->addAction(mp);
        connect(act, &QAction::triggered, this, [this, mp]() { actionMountOn(mp); });
    }
    m_mountMenuAction->setVisible(!a.mountOn.isEmpty());
    m_actBusy->setVisible(a.mountOn.isEmpty());
    m_actFlags->setEnabled(a.flags);
    m_actInfo->setEnabled(a.info);
    m_formatMenu->setEnabled(a.format);
    if (a.format && sel) {
        for (QAction *act : m_formatMenu->actions()) {
            const FsType t = FsType(act->data().toInt());
            bool ok = Layout::canCreate(t, m_display, sel->byteLength());
            QString tip;
            if (const FileSystem *fs = FileSystemRegistry::instance().get(t)) {
                if (!fs->support().create)
                    tip = tr("Requires %1 (%2)").arg(fs->mkfsProgram(), fs->software());
                else if (fs->limits().maxSize && sel->byteLength() > fs->limits().maxSize) {
                    ok = false;
                    tip = tr("A partition with a %1 file system has a maximum size of %2")
                              .arg(fsName(t), formatBytes(fs->limits().maxSize));
                } else if (sel->byteLength() < fs->limits().minSize)
                    tip = tr("A %1 file system requires a partition of at least %2")
                              .arg(fsName(t), formatBytes(fs->limits().minSize));
            }
            act->setEnabled(ok);
            act->setToolTip(tip);
        }
    }
}

void MainWindow::showPartitionMenu(const QPoint &globalPos)
{
    m_partitionMenu->exec(globalPos);
}

// --- actions ----------------------------------------------------------------------------

void MainWindow::addOperation(std::unique_ptr<Operation> op)
{
    QString error;
    if (!m_session.addOperation(std::move(op), &error)) {
        QMessageBox::critical(this, tr("Could not add this operation to the list"), error);
        return;
    }
    refreshVisual();
}

void MainWindow::actionNew()
{
    const Partition *sel = selectedPartition();
    if (!sel)
        return;
    if (sel->type == PartitionType::Unpartitioned && sel->fsType == FsType::Unallocated) {
        QMessageBox box(QMessageBox::Information, tr("No partition table found on device %1").arg(m_display.path),
                        tr("A partition table is required before partitions can be added."), QMessageBox::Cancel, this);
        box.setInformativeText(tr("To create a new partition table select the menu item:\nDevice --> Create Partition Table."));
        QPushButton *create = box.addButton(tr("&Create Partition Table..."), QMessageBox::AcceptRole);
        box.exec();
        if (box.clickedButton() == create)
            actionCreateTable();
        return;
    }

    int primaries = 0;
    bool anyExtended = false;
    for (const Partition &p : m_display.partitions) {
        if (p.type == PartitionType::Primary || p.type == PartitionType::Extended)
            ++primaries;
        anyExtended = anyExtended || p.type == PartitionType::Extended;
    }
    if (!sel->insideExtended && primaries >= m_display.maxPrimaries) {
        QMessageBox box(QMessageBox::Critical, QStringLiteral("QtParted"),
                        tr("It is not possible to create more than %n primary partition(s)", nullptr,
                           m_display.maxPrimaries),
                        QMessageBox::Ok, this);
        box.setInformativeText(tr("If you want more partitions you should first create an extended partition. "
                                  "Such a partition can contain other partitions. Because an extended partition "
                                  "is also a primary partition it might be necessary to remove a primary "
                                  "partition first."));
        box.exec();
        return;
    }

    const Partition unallocated = *sel;
    DialogNewPartition dialog(m_display, unallocated, anyExtended, m_session.nextNewNumber(), this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const Partition np = Layout::makeNewPartition(m_display, unallocated, dialog.request());
    addOperation(std::make_unique<OperationCreate>(m_session.devices[size_t(m_current)], unallocated, np));
    m_selectedKey = Ui::partitionKey(np);
    refreshVisual();
}

void MainWindow::actionDelete()
{
    const Partition *sel = selectedPartition();
    if (!sel)
        return;
    // Logical partitions are renumbered after a deletion; refuse if a higher one is in use.
    if (sel->type == PartitionType::Logical && sel->status != PartitionStatus::New
        && sel->number < m_display.highestBusy) {
        QMessageBox box(QMessageBox::Critical, tr("Unable to delete %1!").arg(sel->path),
                        tr("Please unmount any logical partitions having a number higher than %1").arg(sel->number),
                        QMessageBox::Ok, this);
        box.exec();
        return;
    }
    if (sel->status == PartitionStatus::New || sel->status == PartitionStatus::Copy) {
        m_session.removeOperationsOnNew(*sel);
        m_selectedKey.clear();
        refreshVisual();
        return;
    }
    if (sel->fsType == FsType::Lvm2Pv && !sel->mountPoints.isEmpty() && !confirmRemoveLvmPv(*sel, RemovePv::Delete))
        return;
    const Partition victim = *sel;
    m_selectedKey.clear();
    addOperation(std::make_unique<OperationDelete>(m_session.devices[size_t(m_current)], victim));
}

void MainWindow::actionFormat(FsType type)
{
    const Partition *sel = selectedPartition();
    if (!sel)
        return;
    // Inside open encryption the file system gets the space after the LUKS overhead.
    const ByteValue space = sel->filesystem().byteLength();
    if (const FileSystem *fs = FileSystemRegistry::instance().get(type)) {
        const FsLimits l = fs->limits();
        if (space < l.minSize || (l.maxSize && space > l.maxSize)) {
            QMessageBox box(QMessageBox::Critical, QStringLiteral("QtParted"),
                            tr("Cannot format this file system to %1").arg(fsName(type)), QMessageBox::Ok, this);
            box.setInformativeText(space < l.minSize
                                       ? tr("A %1 file system requires a partition of at least %2.")
                                             .arg(fsName(type), formatBytes(l.minSize))
                                       : tr("A partition with a %1 file system has a maximum size of %2.")
                                             .arg(fsName(type), formatBytes(l.maxSize)));
            box.exec();
            return;
        }
    }
    if (sel->filesystem().fsType == FsType::Lvm2Pv && !sel->filesystem().mountPoints.isEmpty()
        && !confirmRemoveLvmPv(sel->filesystem(), RemovePv::Format))
        return;
    Partition np = *sel;
    if (sel->fsType == FsType::Luks && !sel->isOpenLuks()) {
        // Formatting closed encryption replaces it with a plain file system (GParted).
        np.luks = LuksMapping();
        np.luksHeader = 0;
        np.encrypted.reset();
    }
    {
        Partition &f = np.filesystem();
        f.fsType = type;
        f.fsLabel.clear();
        f.uuid.clear();
        f.mountPoints.clear();
        f.messages.clear();
        f.clearUsage();
        if (!np.isOpenLuks())
            f.busy = false;
    }
    if (np.status == PartitionStatus::Real)
        np.status = PartitionStatus::Formatted;
    if (!np.isOpenLuks())
        Parted::composePartitionFlags(np, m_display.diskType);
    const Partition original = *sel;
    addOperation(std::make_unique<OperationFormat>(m_session.devices[size_t(m_current)], original, np));
}

void MainWindow::actionResizeMove()
{
    const Partition *sel = selectedPartition();
    if (!sel)
        return;
    const Partition original = *sel;
    if (original.filesystem().busy && original.filesystem().fsReadonly) {
        QMessageBox::information(this, tr("Unable to resize read-only file system %1").arg(original.path),
                                 tr("The file system can not be resized while it is mounted read-only.\n"
                                    "Either unmount the file system or remount it read-write."));
        return;
    }
    DialogResizeMove dialog(m_display, original, this);
    if (dialog.exec() != QDialog::Accepted || !dialog.changed() || !passphraseForResize(original))
        return;
    const Partition np = dialog.result();
    if (np.start != original.start && original.status == PartitionStatus::Real
        && original.type != PartitionType::Extended) {
        // GParted's warning: boot loaders refer to sectors, a moved partition may not boot.
        QMessageBox box(QMessageBox::Warning, tr("Moving a partition might cause your operating system to fail to boot"),
                        tr("This operation moves the start sector of partition %1.").arg(original.path),
                        QMessageBox::Cancel, this);
        box.setInformativeText(tr("Failure to boot is most likely to occur if you move the start of a partition "
                                  "containing the operating system, the boot loader or files it needs. You can "
                                  "learn how to repair the boot configuration in the documentation of your "
                                  "boot loader (for GRUB: reinstall it from a live system)."));
        QPushButton *ok = box.addButton(tr("&Queue the Move"), QMessageBox::AcceptRole);
        box.exec();
        if (box.clickedButton() != ok)
            return;
    }
    m_selectedKey = Ui::partitionKey(np);
    addOperation(std::make_unique<OperationResizeMove>(m_session.devices[size_t(m_current)], original, np));
}

void MainWindow::actionCopy()
{
    if (const Partition *sel = selectedPartition()) {
        m_clipboard = *sel;
        m_statusLabel->setText(tr("%1 copied: select unallocated space or a partition and paste").arg(sel->path));
        updateActions();
    }
}

void MainWindow::actionPaste()
{
    const Partition *sel = selectedPartition();
    if (!sel || !m_clipboard)
        return;
    // The source operation keeps the copied partition; the dialogs see its content (the
    // file system inside the encryption when open).
    const Partition source = *m_clipboard;
    const Partition copied = source.filesystem();
    const Device &device = m_session.devices[size_t(m_current)];
    if (sel->type == PartitionType::Unallocated) {
        int primaries = 0;
        for (const Partition &p : m_display.partitions)
            if (p.type == PartitionType::Primary || p.type == PartitionType::Extended)
                ++primaries;
        if (!sel->insideExtended && primaries >= m_display.maxPrimaries) {
            QMessageBox::critical(this, QStringLiteral("QtParted"),
                                  tr("It is not possible to create more than %n primary partition(s)", nullptr,
                                     m_display.maxPrimaries));
            return;
        }
        const Partition unallocated = *sel;
        DialogResizeMove dialog(m_display, unallocated, copied, m_session.nextNewNumber(), this);
        if (dialog.exec() != QDialog::Accepted)
            return;
        const Partition np = dialog.result();
        m_selectedKey = Ui::partitionKey(np);
        addOperation(std::make_unique<OperationCopy>(device, unallocated, np, source));
        return;
    }
    // Over an existing partition: its contents are replaced.
    if (sel->fsType == FsType::Lvm2Pv && !sel->mountPoints.isEmpty() && !confirmRemoveLvmPv(*sel, RemovePv::Paste))
        return;
    Partition destination = *sel;
    if (destination.fsType == FsType::Luks) {
        // Pasting over closed encryption replaces it with a plain file system (GParted).
        destination.luks = LuksMapping();
        destination.luksHeader = 0;
        destination.encrypted.reset();
    }
    QMessageBox box(QMessageBox::Warning, tr("You have pasted into an existing partition"),
                    tr("The data in %1 will be lost when the operation is applied.").arg(destination.path),
                    QMessageBox::Cancel, this);
    QPushButton *ok = box.addButton(tr("&Paste"), QMessageBox::DestructiveRole);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() != ok)
        return;
    addOperation(std::make_unique<OperationCopy>(device, destination, Layout::pasteIntoExisting(destination, copied),
                                                 source));
}

void MainWindow::actionCheck()
{
    if (const Partition *sel = selectedPartition()) {
        const Partition p = *sel;
        // Checking open encryption grows the mapping to fill the partition, which needs
        // the passphrase when the volume key is in the kernel keyring.
        if (!passphraseForResize(p))
            return;
        addOperation(std::make_unique<OperationCheck>(m_session.devices[size_t(m_current)], p));
    }
}

void MainWindow::actionLabel()
{
    const Partition *sel = selectedPartition();
    if (!sel)
        return;
    const Partition &f = sel->filesystem();
    DialogName dialog(tr("Set file system label on %1").arg(sel->path), tr("&Label:"), f.fsLabel,
                      fsLabelMaxLength(f.fsType), this);
    if (dialog.exec() != QDialog::Accepted || dialog.name() == f.fsLabel)
        return;
    const Partition original = *sel;
    addOperation(std::make_unique<OperationLabel>(m_session.devices[size_t(m_current)], original, dialog.name()));
}

void MainWindow::actionNewUuid()
{
    const Partition *sel = selectedPartition();
    if (!sel)
        return;
    const Partition original = *sel;
    addOperation(std::make_unique<OperationChangeUuid>(m_session.devices[size_t(m_current)], original));
}

// --- busy state: encryption, mount, swap, Volume Groups ----------------------------------

bool MainWindow::toggleAllowed(const Partition &p, const QString &disallowed)
{
    const int n = m_session.pendingCountFor(p);
    if (n == 0)
        return true;
    QMessageBox box(QMessageBox::Information, QStringLiteral("QtParted"),
                    tr("%n operation(s) currently pending for partition %1", nullptr, n).arg(p.path), QMessageBox::Ok,
                    this);
    box.setInformativeText(disallowed + QLatin1Char('\n')
                           + tr("Use the Edit menu to undo, clear or apply pending operations."));
    box.exec();
    return false;
}

bool MainWindow::runBlocking(const QString &message, const std::function<bool(QString &)> &action, QString &error)
{
    // The commands can take seconds (cryptsetup derives keys slowly on purpose): keep
    // the window painting while the UI is blocked.
    setScanning(true, message);
    bool ok = false;
    QString err;
    QEventLoop loop;
    QThread *thread = QThread::create([&]() { ok = action(err); });
    connect(thread, &QThread::finished, &loop, &QEventLoop::quit);
    thread->start();
    loop.exec(QEventLoop::ExcludeUserInputEvents);
    thread->wait();
    delete thread;
    setScanning(false);
    error = err;
    return ok;
}

void MainWindow::showToggleFailure(const QString &summary, const QString &error)
{
    QMessageBox box(QMessageBox::Critical, QStringLiteral("QtParted"), summary, QMessageBox::Ok, this);
    box.setInformativeText(error);
    box.exec();
}

void MainWindow::actionToggleCrypt()
{
    const Partition *sel = selectedPartition();
    if (!sel || sel->fsType != FsType::Luks)
        return;
    const Partition p = *sel;
    QString error;
    if (p.busy) {
        if (!toggleAllowed(p, tr("The close encryption action cannot be performed when there are operations "
                                 "pending for the partition.")))
            return;
        if (!runBlocking(tr("Closing encryption on %1").arg(p.path),
                         [p](QString &e) { return Luks::close(p, e); }, error))
            showToggleFailure(tr("Could not close encryption"), error);
        refreshDevices();
        return;
    }
    if (!toggleAllowed(p, tr("The open encryption action cannot be performed when there are operations pending "
                             "for the partition.")))
        return;
    // A passphrase that opened it before in this session is tried first.
    const QByteArray stored = PasswordStore::lookup(p.uuid);
    bool opened = !stored.isEmpty()
               && runBlocking(tr("Opening encryption on %1").arg(p.path),
                              [p, stored](QString &e) { return Luks::open(p, stored, e); }, error);
    if (!opened) {
        DialogPassword dialog(tr("Enter LUKS passphrase to open %1").arg(p.path), p.path, this);
        while (!opened) {
            if (dialog.exec() != QDialog::Accepted)
                return;  // cancelled: nothing changed, no refresh needed
            const QByteArray pw = dialog.password();
            opened = runBlocking(tr("Opening encryption on %1").arg(p.path),
                                 [p, pw](QString &e) { return Luks::open(p, pw, e); }, error);
            if (!opened)
                dialog.setError(pw.isEmpty() ? QString() : tr("Failed to open LUKS encryption"));
        }
        dialog.clearPassword();
    }
    refreshDevices();
}

bool MainWindow::passphraseForResize(const Partition &p)
{
    if (!p.isOpenLuks() || p.luks.keyLocation != LuksMapping::KeyLocation::KeyRing)
        return true;  // the volume key is in the dm-crypt table: no passphrase needed
    if (!PasswordStore::lookup(p.uuid).isEmpty())
        return true;  // worked before; assume it still does, like GParted
    DialogPassword dialog(tr("Enter LUKS passphrase to resize %1").arg(p.path), p.path, this);
    while (true) {
        if (dialog.exec() != QDialog::Accepted)
            return false;
        const QByteArray pw = dialog.password();
        if (pw.isEmpty()) {
            dialog.setError(QString());
            continue;
        }
        QString ignored;
        const bool ok = runBlocking(tr("Testing the passphrase of %1").arg(p.path),
                                    [p, pw](QString &) { return Luks::testPassphrase(p, pw); }, ignored);
        if (ok) {
            PasswordStore::store(p.uuid, pw);
            dialog.clearPassword();
            return true;
        }
        dialog.setError(tr("LUKS encryption passphrase check failed"));
    }
}

void MainWindow::actionToggleBusy()
{
    const Partition *sel = selectedPartition();
    if (!sel)
        return;
    const AllowedActions a = Session::allowedActions(m_display, sel, m_session.pendingCount(m_display.path));
    const Partition f = sel->filesystem();
    QString disallowed, pulse, failure;
    std::function<bool(QString &)> action;
    switch (a.toggle) {
    case AllowedActions::Toggle::SwapOff:
        disallowed = tr("The swapoff action cannot be performed when there are operations pending for the partition.");
        pulse = tr("Deactivating swap on %1").arg(f.path);
        failure = tr("Could not deactivate swap");
        action = [f](QString &e) { return BusyActions::swapOff(f, e); };
        break;
    case AllowedActions::Toggle::SwapOn:
        disallowed = tr("The swapon action cannot be performed when there are operations pending for the partition.");
        pulse = tr("Activating swap on %1").arg(f.path);
        failure = tr("Could not activate swap");
        action = [f](QString &e) { return BusyActions::swapOn(f, e); };
        break;
    case AllowedActions::Toggle::DeactivateVg:
        disallowed = tr("The deactivate Volume Group action cannot be performed when there are operations pending "
                        "for the partition.");
        pulse = tr("Deactivating Volume Group %1").arg(f.mountPointsText());
        failure = tr("Could not deactivate Volume Group");
        action = [f](QString &e) { return BusyActions::setVolumeGroupActive(f, false, e); };
        break;
    case AllowedActions::Toggle::ActivateVg:
        disallowed = tr("The activate Volume Group action cannot be performed when there are operations pending "
                        "for the partition.");
        pulse = tr("Activating Volume Group %1").arg(f.mountPointsText());
        failure = tr("Could not activate Volume Group");
        action = [f](QString &e) { return BusyActions::setVolumeGroupActive(f, true, e); };
        break;
    case AllowedActions::Toggle::Unmount:
        disallowed = tr("The unmount action cannot be performed when there are operations pending for the partition.");
        pulse = tr("Unmounting %1").arg(f.path);
        failure = tr("Could not unmount %1").arg(f.path);
        action = [f](QString &e) { return BusyActions::unmount(f, e); };
        break;
    case AllowedActions::Toggle::None:
        return;
    }
    if (!toggleAllowed(*sel, disallowed))
        return;
    QString error;
    if (!runBlocking(pulse, action, error))
        showToggleFailure(failure, error);
    refreshDevices();
}

void MainWindow::actionMountOn(const QString &mountPoint)
{
    const Partition *sel = selectedPartition();
    if (!sel)
        return;
    if (!toggleAllowed(*sel, tr("The mount action cannot be performed when an operation is pending for the partition.")))
        return;
    const Partition f = sel->filesystem();
    QString error;
    if (!runBlocking(tr("mounting %1 on %2").arg(f.path, mountPoint),
                     [f, mountPoint](QString &e) { return BusyActions::mount(f, mountPoint, e); }, error))
        showToggleFailure(tr("Could not mount %1 on %2").arg(f.path, mountPoint), error);
    refreshDevices();
}

bool MainWindow::confirmRemoveLvmPv(const Partition &pv, RemovePv kind)
{
    QString text;
    switch (kind) {
    case RemovePv::Delete: text = tr("You are deleting non-empty LVM2 Physical Volume %1").arg(pv.path); break;
    case RemovePv::Format: text = tr("You are formatting over non-empty LVM2 Physical Volume %1").arg(pv.path); break;
    case RemovePv::Paste:  text = tr("You are pasting over non-empty LVM2 Physical Volume %1").arg(pv.path); break;
    }
    const QString vg = pv.mountPoints.value(0);
    QStringList members;
    if (!vg.isEmpty()) {
        const CommandResult r = runCommand(QStringLiteral("lvm"), { QStringLiteral("vgs"), QStringLiteral("--noheadings"),
                                                                   QStringLiteral("-o"), QStringLiteral("pv_name"), vg });
        for (const QString &l : r.output.split(QLatin1Char('\n'), Qt::SkipEmptyParts))
            members << l.trimmed();
    }
    QMessageBox box(QMessageBox::Warning, QStringLiteral("QtParted"), text, QMessageBox::Cancel, this);
    box.setInformativeText(
        tr("Deleting or overwriting the Physical Volume is irrecoverable and will destroy or damage the Volume Group.")
        + QStringLiteral("\n\n")
        + tr("To avoid destroying or damaging the Volume Group, you are advised to cancel and use external LVM "
             "commands to free the Physical Volume before attempting this operation.")
        + QStringLiteral("\n\n") + tr("Do you want to continue to forcibly delete the Physical Volume?")
        + QStringLiteral("\n\n") + tr("Volume Group: %1").arg(vg) + QLatin1Char('\n')
        + tr("Members: %1").arg(members.join(QStringLiteral(", "))));
    QPushButton *cont = box.addButton(tr("&Continue"), QMessageBox::DestructiveRole);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    return box.clickedButton() == cont;
}

void MainWindow::actionName()
{
    const Partition *sel = selectedPartition();
    if (!sel)
        return;
    DialogName dialog(*sel, m_display.maxNameLength, this);
    if (dialog.exec() != QDialog::Accepted || dialog.name() == sel->name)
        return;
    const Partition original = *sel;
    addOperation(std::make_unique<OperationName>(m_session.devices[size_t(m_current)], original, dialog.name()));
}

void MainWindow::actionFlags()
{
    const Partition *sel = selectedPartition();
    if (!sel)
        return;
    DialogFlags dialog(*sel, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    QStringList now = dialog.flags();
    QStringList before = sel->flags;
    now.sort();
    before.sort();
    if (now == before)
        return;
    const Partition original = *sel;
    addOperation(std::make_unique<OperationSetFlags>(m_session.devices[size_t(m_current)], original, dialog.flags()));
}

void MainWindow::actionInfo()
{
    if (const Partition *sel = selectedPartition()) {
        DialogPartitionInfo dialog(m_display, *sel, this);
        dialog.exec();
    }
}

void MainWindow::actionCreateTable()
{
    if (m_current < 0)
        return;
    if (m_session.pendingCount(m_display.path) > 0) {
        QMessageBox::information(this, QStringLiteral("QtParted"),
                                 tr("There are pending operations on %1. Apply or clear them before creating a new "
                                    "partition table.").arg(m_display.path));
        return;
    }
    if (m_display.highestBusy > 0) {
        QMessageBox::warning(this, QStringLiteral("QtParted"),
                             tr("Partitions of %1 are in use. Unmount them before creating a new partition table.")
                                 .arg(m_display.path));
        return;
    }
    DialogCreateTable dialog(m_display, Parted::diskLabelTypes(), this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const QString type = dialog.diskType();
    const Parted::DiskTypeInfo info = Parted::diskTypeInfo(m_display.path, type);
    m_selectedKey.clear();
    addOperation(std::make_unique<OperationCreateTable>(m_session.devices[size_t(m_current)], type, info));
}

void MainWindow::actionUndo()
{
    m_session.undoLast();
    refreshVisual();
}

void MainWindow::actionClear()
{
    m_session.clear();
    refreshVisual();
}

void MainWindow::actionApply()
{
    if (m_session.operations.empty())
        return;
    QMessageBox box(QMessageBox::Warning, tr("Apply operations to device?"),
                    tr("Are you sure you want to apply the pending operations?"),
                    QMessageBox::Cancel, this);
    box.setInformativeText(tr("Editing partitions has the potential to cause LOSS of DATA.\n"
                              "You are advised to backup your data before proceeding."));
    QPushButton *apply = box.addButton(tr("&Apply"), QMessageBox::AcceptRole);
    apply->setIcon(Ui::icon("dialog-ok-apply", QStyle::SP_DialogApplyButton));
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() != apply)
        return;

    std::vector<Operation *> ops;
    for (auto &op : m_session.operations)
        ops.push_back(op.get());
    {
        DialogApply dialog(ops, this);
        dialog.run();
    }
    m_session.clear();
    refreshDevices();
}

void MainWindow::actionFsSupport()
{
    DialogFsSupport dialog(this);
    connect(&dialog, &DialogFsSupport::rescanned, this, &MainWindow::updateActions);
    dialog.exec();
}

void MainWindow::actionAbout()
{
    QMessageBox about(this);
    about.setWindowTitle(tr("About QtParted"));
    about.setIconPixmap(QPixmap(QStringLiteral(":/icons/qtparted-64.png")));
    about.setTextFormat(Qt::RichText);
    about.setText(tr("<h2>QtParted %1</h2><p>A partition editor for Qt 6, based on libparted.</p>"
                     "<p>Copyright &copy; 2002&ndash;2026 Vanni Brutto</p>"
                     "<p>Partition handling logic derived from <a href=\"https://gparted.org\">GParted</a> "
                     "&copy; Bart Hakvoort, Curtis Gedak, Mike Fleetwood and contributors.</p>"
                     "<p>libparted %2 &middot; Qt %3</p>"
                     "<p>This program is free software, released under the "
                     "GNU General Public License version 2 or later.</p>"
                     "<p><a href=\"https://github.com/zanac/qtparted\">github.com/zanac/qtparted</a></p>")
                      .arg(QStringLiteral(QTPARTED_VERSION), Parted::version(), QString::fromLatin1(qVersion())));
    about.exec();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_scanning) {
        event->ignore();
        return;
    }
    if (!m_session.operations.empty()) {
        QMessageBox box(QMessageBox::Question, tr("Quit QtParted?"),
                        tr("%n operation(s) currently pending.", nullptr, int(m_session.operations.size())),
                        QMessageBox::Cancel, this);
        QPushButton *quit = box.addButton(tr("&Quit"), QMessageBox::DestructiveRole);
        box.exec();
        if (box.clickedButton() != quit) {
            event->ignore();
            return;
        }
    }
    QSettings settings;
    settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("window/state"), saveState());
    event->accept();
}

} // namespace QtParted

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

#include <QDialog>
#include <QElapsedTimer>
#include <QSet>
#include <QTimer>

#include <vector>

class QLabel;
class QProgressBar;
class QPushButton;
class QThread;
class QTreeWidget;
class QTreeWidgetItem;

namespace QtParted {

class Operation;
class OperationApplier;
class OperationDetail;

// Applies the pending operations in a worker thread, showing live details.
class DialogApply : public QDialog
{
    Q_OBJECT
public:
    DialogApply(std::vector<Operation *> operations, QWidget *parent = nullptr);
    ~DialogApply() override;

    // Starts applying and runs the dialog; returns true when everything succeeded.
    bool run();

protected:
    void closeEvent(QCloseEvent *event) override;
    void reject() override;

private:
    void refresh();
    void addDetail(QTreeWidgetItem *parent, const OperationDetail &detail, const QString &path);
    void onOperationStarted(int index);
    void onFinished(bool success, int completed);
    void saveDetails();

    std::vector<Operation *> m_operations;
    QProgressBar *m_progress;
    QLabel *m_current;
    QLabel *m_elapsed;
    QLabel *m_result;
    QTreeWidget *m_tree;
    QPushButton *m_stop;
    QPushButton *m_save;
    QPushButton *m_close;

    QThread *m_thread = nullptr;
    OperationApplier *m_applier = nullptr;
    QTimer m_timer;
    QElapsedTimer m_clock;
    quint64 m_generation = ~0ULL;
    QSet<QString> m_collapsed;   // detail paths the user collapsed
    QSet<QString> m_expanded;    // detail paths the user expanded
    int m_runningIndex = -1;
    bool m_running = false;
    bool m_success = false;
};

} // namespace QtParted

/*
 * QtParted - a Qt frontend to libparted for manipulating disk partitions
 * Copyright (C) 2002-2026 Vanni Brutto <zanac4ever@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "DialogApply.h"

#include "Operation.h"
#include "Parted.h"
#include "UiHelpers.h"
#include "Worker.h"

#include <QCloseEvent>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFontDatabase>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QMutexLocker>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QThread>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace QtParted {

namespace {

QIcon statusIcon(OperationDetail::Status s)
{
    switch (s) {
    case OperationDetail::Status::Executing:
        return Ui::icon("media-playback-start", QStyle::SP_MediaPlay);
    case OperationDetail::Status::Success:
        return Ui::icon("dialog-ok-apply", QStyle::SP_DialogApplyButton);
    case OperationDetail::Status::Error:
        return Ui::icon("dialog-error", QStyle::SP_MessageBoxCritical);
    case OperationDetail::Status::Info:
        return Ui::icon("dialog-information", QStyle::SP_MessageBoxInformation);
    case OperationDetail::Status::Warning:
        return Ui::icon("dialog-warning", QStyle::SP_MessageBoxWarning);
    default:
        return QIcon();
    }
}

QString elapsedText(qint64 ms)
{
    return QTime(0, 0).addMSecs(int(ms)).toString(QStringLiteral("HH:mm:ss"));
}

} // namespace

DialogApply::DialogApply(std::vector<Operation *> operations, QWidget *parent)
    : QDialog(parent)
    , m_operations(std::move(operations))
{
    setWindowTitle(tr("Applying pending operations"));
    setModal(true);

    auto *intro = new QLabel(tr("<b>Applying pending operations</b><br>Depending on the number and type of "
                                "operations this might take a long time."),
                             this);
    m_progress = new QProgressBar(this);
    m_progress->setRange(0, int(m_operations.size()));
    m_progress->setFormat(tr("%v of %m operations completed"));
    m_current = new QLabel(this);
    m_current->setWordWrap(true);
    m_elapsed = new QLabel(this);
    m_elapsed->setForegroundRole(QPalette::PlaceholderText);
    m_result = new QLabel(this);
    m_result->setWordWrap(true);
    m_result->hide();

    m_tree = new QTreeWidget(this);
    m_tree->setHeaderHidden(true);
    m_tree->setUniformRowHeights(false);
    m_tree->setColumnCount(2);
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    connect(m_tree, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem *i) {
        const QString path = i->data(0, Qt::UserRole).toString();
        m_collapsed.insert(path);
        m_expanded.remove(path);
    });
    connect(m_tree, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem *i) {
        const QString path = i->data(0, Qt::UserRole).toString();
        m_expanded.insert(path);
        m_collapsed.remove(path);
    });

    auto *buttons = new QDialogButtonBox(this);
    m_stop = buttons->addButton(tr("&Stop After Current Operation"), QDialogButtonBox::RejectRole);
    m_save = buttons->addButton(tr("&Save Details..."), QDialogButtonBox::ActionRole);
    m_close = buttons->addButton(QDialogButtonBox::Close);
    m_save->setIcon(Ui::icon("document-save", QStyle::SP_DialogSaveButton));
    m_save->setEnabled(false);
    m_close->setEnabled(false);
    connect(m_stop, &QPushButton::clicked, this, [this]() {
        if (m_applier)
            m_applier->requestStop();
        m_stop->setEnabled(false);
        m_stop->setText(tr("Stopping..."));
    });
    connect(m_save, &QPushButton::clicked, this, &DialogApply::saveDetails);
    connect(m_close, &QPushButton::clicked, this, &QDialog::accept);

    auto *top = new QVBoxLayout(this);
    top->addWidget(intro);
    top->addWidget(m_progress);
    top->addWidget(m_current);
    top->addWidget(m_elapsed);
    top->addWidget(m_result);
    top->addWidget(m_tree, 1);
    top->addWidget(buttons);
    resize(760, 560);

    m_timer.setInterval(200);
    connect(&m_timer, &QTimer::timeout, this, &DialogApply::refresh);
}

DialogApply::~DialogApply()
{
    if (m_thread) {
        m_thread->quit();
        m_thread->wait();
    }
}

bool DialogApply::run()
{
    m_thread = new QThread(this);
    m_applier = new OperationApplier(m_operations);
    m_applier->moveToThread(m_thread);
    connect(m_thread, &QThread::started, m_applier, &OperationApplier::run);
    connect(m_thread, &QThread::finished, m_applier, &QObject::deleteLater);
    connect(m_applier, &OperationApplier::operationStarted, this, &DialogApply::onOperationStarted);
    connect(m_applier, &OperationApplier::operationFinished, this, [this](int index, bool) {
        m_progress->setValue(index + 1);
    });
    connect(m_applier, &OperationApplier::finished, this, &DialogApply::onFinished);

    m_running = true;
    m_clock.start();
    m_timer.start();
    m_thread->start();
    exec();
    return m_success;
}

void DialogApply::onOperationStarted(int index)
{
    m_runningIndex = index;
    m_current->setText(tr("Operation %1 of %2: <i>%3</i>")
                           .arg(index + 1)
                           .arg(m_operations.size())
                           .arg(m_operations[size_t(index)]->description().toHtmlEscaped()));
    refresh();
}

void DialogApply::onFinished(bool success, int completed)
{
    m_running = false;
    m_success = success;
    m_timer.stop();
    m_thread->quit();
    m_applier = nullptr;
    m_generation = ~0ULL;
    refresh();

    m_progress->setValue(completed);
    m_stop->hide();
    m_save->setEnabled(true);
    m_close->setEnabled(true);
    m_close->setDefault(true);
    m_close->setFocus();
    m_current->clear();
    m_result->show();
    if (success) {
        m_result->setText(tr("<b>All operations successfully completed</b>"));
    } else if (completed < int(m_operations.size()) && m_runningIndex == completed - 1) {
        m_result->setText(tr("<b>Stopped after %1 of %2 operations.</b> The remaining operations were not applied.")
                              .arg(completed)
                              .arg(m_operations.size()));
    } else {
        m_result->setText(tr("<b>An error occurred while applying the operations.</b> See the details below; "
                             "the operations after the failed one were not applied."));
        m_result->setStyleSheet(QStringLiteral("color: palette(bright-text);"));
    }
    m_elapsed->setText(tr("Total time: %1").arg(elapsedText(m_clock.elapsed())));
}

void DialogApply::addDetail(QTreeWidgetItem *parent, const OperationDetail &d, const QString &path)
{
    auto *item = new QTreeWidgetItem(parent);
    QString text = d.description();
    while (text.endsWith(QLatin1Char('\n')))
        text.chop(1);
    item->setText(0, text);
    item->setIcon(0, statusIcon(d.status()));
    item->setData(0, Qt::UserRole, path);
    if (d.font() != OperationDetail::Font::Normal) {
        QFont f = d.font() == OperationDetail::Font::Monospace ? QFontDatabase::systemFont(QFontDatabase::FixedFont)
                                                                : item->font(0);
        f.setItalic(d.font() == OperationDetail::Font::Italic);
        item->setFont(0, f);
    }
    const qint64 ms = d.elapsedMs();
    if (ms >= 0 && !d.children().empty())
        item->setText(1, elapsedText(ms));
    int i = 0;
    for (const auto &c : d.children())
        addDetail(item, *c, path + QLatin1Char('/') + QString::number(i++));

    const bool wanted = d.status() == OperationDetail::Status::Executing || d.status() == OperationDetail::Status::Error;
    if (m_expanded.contains(path) || (wanted && !m_collapsed.contains(path)))
        item->setExpanded(true);
}

void DialogApply::refresh()
{
    if (m_running)
        m_elapsed->setText(tr("Elapsed time: %1").arg(elapsedText(m_clock.elapsed())));
    const quint64 gen = OperationDetail::generation();
    if (gen == m_generation)
        return;
    m_generation = gen;

    QMutexLocker lock(&OperationDetail::mutex());
    m_tree->setUpdatesEnabled(false);
    // Signals from programmatic expansion must not be taken as user choices.
    const QSignalBlocker block(m_tree);
    m_tree->clear();
    for (size_t i = 0; i < m_operations.size(); ++i) {
        const OperationDetail &d = m_operations[i]->detail();
        auto *item = new QTreeWidgetItem(m_tree);
        const QString path = QString::number(i);
        item->setData(0, Qt::UserRole, path);
        item->setText(0, m_operations[i]->description());
        item->setIcon(0, statusIcon(d.status()));
        const qint64 ms = d.elapsedMs();
        if (ms >= 0)
            item->setText(1, elapsedText(ms));
        QFont f = item->font(0);
        f.setBold(true);
        item->setFont(0, f);
        int c = 0;
        for (const auto &child : d.children())
            addDetail(item, *child, path + QLatin1Char('/') + QString::number(c++));
        const bool wanted = d.status() == OperationDetail::Status::Executing
                         || d.status() == OperationDetail::Status::Error;
        if (m_expanded.contains(path) || (wanted && !m_collapsed.contains(path)))
            item->setExpanded(true);
        if (int(i) == m_runningIndex && m_running)
            m_tree->scrollToItem(item);
    }
    m_tree->setUpdatesEnabled(true);
}

void DialogApply::saveDetails()
{
    const QString file = QFileDialog::getSaveFileName(this, tr("Save Details"),
                                                      QStringLiteral("qtparted_details.htm"),
                                                      tr("HTML files (*.htm *.html)"));
    if (file.isEmpty())
        return;
    QString html = QStringLiteral("<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\"><title>QtParted Details</title>"
                                  "<style>body{font-family:sans-serif} pre{margin:2px 0}</style></head><body>\n");
    html += QStringLiteral("<p>QtParted %1<br>libparted %2<br>%3</p>\n")
                .arg(QStringLiteral(QTPARTED_VERSION), Parted::version().toHtmlEscaped(),
                     QDateTime::currentDateTime().toString(Qt::ISODate));
    html += QStringLiteral("<ul>\n");
    for (Operation *op : m_operations)
        html += op->detail().toHtml();
    html += QStringLiteral("</ul></body></html>\n");

    QSaveFile f(file);
    if (!f.open(QIODevice::WriteOnly) || f.write(html.toUtf8()) < 0 || !f.commit())
        QMessageBox::warning(this, tr("Save Details"), tr("Could not save %1: %2").arg(file, f.errorString()));
}

void DialogApply::closeEvent(QCloseEvent *event)
{
    if (m_running)
        event->ignore();
    else
        QDialog::closeEvent(event);
}

void DialogApply::reject()
{
    if (!m_running)
        QDialog::reject();
}

} // namespace QtParted

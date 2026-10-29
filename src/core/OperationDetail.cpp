/*
 * QtParted - a Qt frontend to libparted for manipulating disk partitions
 * Copyright (C) 2002-2026 Vanni Brutto <zanac4ever@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "OperationDetail.h"

#include <QCoreApplication>
#include <QMutexLocker>

#include <atomic>

namespace QtParted {

namespace {
std::atomic<quint64> g_generation{ 0 };
thread_local QStringList t_libpartedMessages;
}

OperationDetail::OperationDetail(const QString &description, Status status, Font font)
    : m_description(description), m_status(status), m_font(font)
{
    if (status == Status::Executing)
        m_started = QDateTime::currentDateTime();
}

QRecursiveMutex &OperationDetail::mutex()
{
    static QRecursiveMutex m;
    return m;
}

quint64 OperationDetail::generation()
{
    return g_generation.load();
}

void OperationDetail::touch()
{
    ++g_generation;
}

void OperationDetail::pushLibpartedMessage(const QString &message)
{
    t_libpartedMessages.append(message);
}

QStringList OperationDetail::takeLibpartedMessages()
{
    QStringList m;
    m.swap(t_libpartedMessages);
    return m;
}

OperationDetail &OperationDetail::addChild(const QString &description, Status status, Font font)
{
    QMutexLocker lock(&mutex());
    m_children.push_back(std::make_unique<OperationDetail>(description, status, font));
    touch();
    return *m_children.back();
}

OperationDetail &OperationDetail::lastChild()
{
    QMutexLocker lock(&mutex());
    if (m_children.empty())
        return addChild(QString(), Status::None);
    return *m_children.back();
}

void OperationDetail::setStatus(Status status)
{
    QMutexLocker lock(&mutex());
    if (status == Status::Executing && !m_started.isValid())
        m_started = QDateTime::currentDateTime();
    if ((status == Status::Success || status == Status::Error || status == Status::Warning)
        && m_started.isValid())
        m_finished = QDateTime::currentDateTime();
    m_status = status;
    touch();
}

void OperationDetail::setSuccessAndCaptureErrors(bool success)
{
    const QStringList messages = takeLibpartedMessages();
    QMutexLocker lock(&mutex());
    if (!messages.isEmpty()) {
        OperationDetail &od = addChild(QCoreApplication::translate("OperationDetail", "libparted messages"),
                                       success ? Status::Info : Status::Error);
        for (const QString &m : messages)
            od.addChild(m, Status::None, Font::Italic);
    }
    setStatus(success ? Status::Success : Status::Error);
}

void OperationDetail::appendText(const QString &text)
{
    QMutexLocker lock(&mutex());
    m_description += text;
    touch();
}

void OperationDetail::setDescription(const QString &text)
{
    QMutexLocker lock(&mutex());
    m_description = text;
    touch();
}

void OperationDetail::reset(const QString &description)
{
    QMutexLocker lock(&mutex());
    m_description = description;
    m_status = Status::None;
    m_started = QDateTime();
    m_finished = QDateTime();
    m_children.clear();
    touch();
}

qint64 OperationDetail::elapsedMs() const
{
    QMutexLocker lock(&mutex());
    if (!m_started.isValid())
        return -1;
    const QDateTime end = m_finished.isValid() ? m_finished : QDateTime::currentDateTime();
    return m_started.msecsTo(end);
}

static QString statusText(OperationDetail::Status s)
{
    switch (s) {
    case OperationDetail::Status::Executing:     return QCoreApplication::translate("OperationDetail", "executing");
    case OperationDetail::Status::Success:       return QCoreApplication::translate("OperationDetail", "success");
    case OperationDetail::Status::Error:         return QCoreApplication::translate("OperationDetail", "error");
    case OperationDetail::Status::Info:          return QCoreApplication::translate("OperationDetail", "info");
    case OperationDetail::Status::Warning:       return QCoreApplication::translate("OperationDetail", "warning");
    case OperationDetail::Status::NotApplicable: return QCoreApplication::translate("OperationDetail", "n/a");
    case OperationDetail::Status::None:          break;
    }
    return QString();
}

QString OperationDetail::toHtml() const
{
    QMutexLocker lock(&mutex());
    QString html;
    QString text = m_description.toHtmlEscaped();
    if (m_font == Font::Monospace || m_font == Font::Italic)
        text = QStringLiteral("<pre%1>%2</pre>")
                   .arg(m_font == Font::Italic ? QStringLiteral(" style=\"font-style:italic\"") : QString(), text);
    else
        text.replace(QLatin1Char('\n'), QStringLiteral("<br>"));
    html += QStringLiteral("<li>") + text;
    const QString st = statusText(m_status);
    if (!st.isEmpty() && m_status != Status::Executing) {
        html += QStringLiteral(" &nbsp;<b>[%1]</b>").arg(st);
        const qint64 ms = elapsedMs();
        if (ms >= 0)
            html += QStringLiteral(" <small>%1</small>").arg(QTime(0, 0).addMSecs(int(ms)).toString(QStringLiteral("HH:mm:ss")));
    }
    if (!m_children.empty()) {
        html += QStringLiteral("<ul>");
        for (const auto &c : m_children)
            html += c->toHtml();
        html += QStringLiteral("</ul>");
    }
    html += QStringLiteral("</li>\n");
    return html;
}

QString OperationDetail::toPlainText(int indent) const
{
    QMutexLocker lock(&mutex());
    const QString pad(indent * 2, QLatin1Char(' '));
    QString out;
    const QStringList lines = m_description.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        out += pad + lines[i];
        if (i == 0) {
            const QString st = statusText(m_status);
            if (!st.isEmpty())
                out += QStringLiteral("  [%1]").arg(st);
        }
        out += QLatin1Char('\n');
    }
    for (const auto &c : m_children)
        out += c->toPlainText(indent + 1);
    return out;
}

} // namespace QtParted

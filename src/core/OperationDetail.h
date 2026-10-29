/*
 * QtParted - a Qt frontend to libparted for manipulating disk partitions
 * Copyright (C) 2002-2026 Vanni Brutto <zanac4ever@gmail.com>
 *
 * Modelled on GParted's OperationDetail (https://gparted.org),
 * Copyright (C) 2004 Bart Hakvoort and the GParted contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#pragma once

#include <QDateTime>
#include <QRecursiveMutex>
#include <QString>
#include <QStringList>

#include <memory>
#include <vector>

namespace QtParted {

// A node in the tree describing what an operation did, shown while applying operations
// and saved in the details log.  The tree is written by the worker thread and read by the
// GUI thread; every access must hold OperationDetail::mutex().
class OperationDetail
{
public:
    enum class Status { None, Executing, Success, Error, Info, Warning, NotApplicable };
    enum class Font { Normal, Italic, Monospace };

    explicit OperationDetail(const QString &description = {}, Status status = Status::None,
                             Font font = Font::Normal);
    OperationDetail(const OperationDetail &) = delete;
    OperationDetail &operator=(const OperationDetail &) = delete;

    OperationDetail &addChild(const QString &description, Status status = Status::Executing,
                              Font font = Font::Normal);
    OperationDetail &lastChild();

    void setStatus(Status status);
    // Success/Error, and attach any libparted messages captured since the last call.
    void setSuccessAndCaptureErrors(bool success);
    void appendText(const QString &text);  // for streamed command output
    void reset(const QString &description);

    QString description() const { return m_description; }
    Status status() const { return m_status; }
    Font font() const { return m_font; }
    qint64 elapsedMs() const;
    const std::vector<std::unique_ptr<OperationDetail>> &children() const { return m_children; }

    QString toHtml() const;
    QString toPlainText(int indent = 0) const;

    static QRecursiveMutex &mutex();
    static quint64 generation();  // incremented on every change of any tree

    // libparted messages raised on the current thread (filled by the exception handler)
    static void pushLibpartedMessage(const QString &message);
    static QStringList takeLibpartedMessages();

private:
    static void touch();

    QString m_description;
    Status m_status;
    Font m_font;
    QDateTime m_started;
    QDateTime m_finished;
    std::vector<std::unique_ptr<OperationDetail>> m_children;
};

} // namespace QtParted

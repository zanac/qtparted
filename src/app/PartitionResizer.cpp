/*
 * QtParted - a Qt frontend to libparted for manipulating disk partitions
 * Copyright (C) 2002-2026 Vanni Brutto <zanac4ever@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "PartitionResizer.h"

#include "UiHelpers.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>

#include <algorithm>

namespace QtParted {

namespace {
constexpr int kMargin = 10;
constexpr int kHandle = 6;  // grab distance from an edge, in pixels
}

PartitionResizer::PartitionResizer(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setAccessibleName(tr("Partition size and position"));
}

void PartitionResizer::setTotal(qint64 totalMiB)
{
    m_total = std::max<qint64>(1, totalMiB);
    update();
}

void PartitionResizer::setLimits(qint64 minBeforeMiB, qint64 minSizeMiB, qint64 maxSizeMiB)
{
    m_minBefore = std::max<qint64>(0, minBeforeMiB);
    m_minSize = std::max<qint64>(1, minSizeMiB);
    m_maxSize = std::max(m_minSize, maxSizeMiB);
    update();
}

void PartitionResizer::setValues(qint64 beforeMiB, qint64 sizeMiB)
{
    m_before = beforeMiB;
    m_size = sizeMiB;
    update();
}

void PartitionResizer::setMaxBefore(qint64 maxBeforeMiB)
{
    m_maxBefore = maxBeforeMiB;
    update();
}

void PartitionResizer::setEndRange(qint64 minEndMiB, qint64 maxEndMiB)
{
    m_minEnd = minEndMiB;
    m_maxEnd = maxEndMiB;
    update();
}

void PartitionResizer::setFixedStart(bool fixed)
{
    m_fixedStart = fixed;
    update();
}

void PartitionResizer::setUsed(qint64 usedMiB, qint64 usedStartMiB)
{
    m_used = std::max<qint64>(0, usedMiB);
    m_usedStart = usedStartMiB;
    update();
}

void PartitionResizer::setPartitionColor(const QColor &color)
{
    m_color = color;
    update();
}

void PartitionResizer::setExtendedBackground(bool extended)
{
    m_extended = extended;
    update();
}

QRect PartitionResizer::track() const
{
    return rect().adjusted(kMargin, 8, -kMargin, -8);
}

int PartitionResizer::toX(qint64 mib) const
{
    const QRect t = track();
    return t.left() + int(double(t.width()) * double(mib) / double(m_total));
}

qint64 PartitionResizer::toMiB(int x) const
{
    const QRect t = track();
    return qRound64(double(x - t.left()) * double(m_total) / double(std::max(1, t.width())));
}

QRect PartitionResizer::partitionRect() const
{
    const QRect t = track();
    const int l = toX(m_before);
    const int r = std::max(l + 6, toX(m_before + m_size));
    return QRect(QPoint(l, t.top()), QPoint(std::min(r, t.right()), t.bottom()));
}

PartitionResizer::Drag PartitionResizer::hit(const QPoint &pos) const
{
    const QRect r = partitionRect();
    if (pos.y() < r.top() - 4 || pos.y() > r.bottom() + 4)
        return Drag::None;
    if (std::abs(pos.x() - r.right()) <= kHandle)
        return Drag::Right;
    if (m_fixedStart)
        return Drag::None;
    if (std::abs(pos.x() - r.left()) <= kHandle)
        return Drag::Left;
    if (r.contains(pos))
        return Drag::Move;
    return Drag::None;
}

void PartitionResizer::apply(qint64 before, qint64 size, Drag mode)
{
    const qint64 maxEnd = m_maxEnd < 0 ? m_total : std::min(m_maxEnd, m_total);
    const qint64 maxBefore = m_maxBefore < 0 ? m_total : m_maxBefore;
    const qint64 end = m_before + m_size;
    if (m_fixedStart)
        before = m_before;
    if (mode == Drag::Move) {
        // Same size, only the position changes.
        size = m_size;
        before = std::clamp(before, std::max(m_minBefore, m_minEnd - size), std::min(maxBefore, maxEnd - size));
    } else if (mode == Drag::Left) {
        // The end stays, the start moves.
        before = std::clamp(before, std::max(m_minBefore, end - m_maxSize), std::min(maxBefore, end - m_minSize));
        size = end - before;
    } else {
        before = std::clamp(before, m_minBefore, std::max(m_minBefore, maxBefore));
        size = std::clamp(size, std::max(m_minSize, m_minEnd - before), std::min(m_maxSize, maxEnd - before));
    }
    if (size < 1 || (before == m_before && size == m_size))
        return;
    m_before = before;
    m_size = size;
    update();
    Q_EMIT valuesChanged(m_before, m_size);
}

void PartitionResizer::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    const QPalette &pal = palette();
    const QRect t = track();

    // Free space: grey frame, light (or extended-cyan) background.
    p.fillRect(t, Ui::fsColor(FsType::Unallocated));
    p.fillRect(t.adjusted(2, 2, -2, -2), m_extended ? Ui::fsColor(FsType::Extended).lighter(130)
                                                    : Ui::unusedColor(pal));

    // The partition being created: coloured frame like in the disk map.
    const QRect r = partitionRect();
    p.fillRect(r, m_color);
    const QRect inner = r.adjusted(4, 4, -4, -4);
    p.fillRect(inner, Ui::unusedColor(pal));
    if (m_used > 0) {
        // Used space: from the partition start (file system) or fixed (logical partitions).
        const int ul = m_usedStart < 0 ? inner.left() : std::max(inner.left(), toX(m_usedStart));
        const int ur = m_usedStart < 0 ? std::min(inner.right(), toX(m_before + m_used))
                                       : std::min(inner.right(), toX(m_usedStart + m_used));
        if (ur > ul)
            p.fillRect(QRect(QPoint(ul, inner.top()), QPoint(ur, inner.bottom())),
                       m_usedStart < 0 ? Ui::usedColor(pal) : Ui::fsColor(FsType::Extended).lighter(115));
    }

    // Grab handles.
    p.setPen(Qt::NoPen);
    p.setBrush(pal.color(QPalette::Highlight));
    const int midY = r.center().y();
    for (const int x : { r.left(), r.right() }) {
        if (x == r.left() && m_fixedStart)
            continue;
        p.drawRoundedRect(QRect(x - 3, midY - 10, 6, 20), 2, 2);
    }

    if (hasFocus()) {
        p.setPen(QPen(pal.color(QPalette::Highlight), 1, Qt::DotLine));
        p.setBrush(Qt::NoBrush);
        p.drawRect(rect().adjusted(1, 1, -2, -2));
    }
}

void PartitionResizer::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || !isEnabled())
        return;
    m_drag = hit(event->position().toPoint());
    m_pressX = int(event->position().x());
    m_pressBefore = m_before;
    m_pressSize = m_size;
}

void PartitionResizer::mouseMoveEvent(QMouseEvent *event)
{
    const QPoint pos = event->position().toPoint();
    if (m_drag == Drag::None) {
        const Drag h = hit(pos);
        setCursor(h == Drag::Left || h == Drag::Right ? Qt::SizeHorCursor
                  : h == Drag::Move                    ? Qt::OpenHandCursor
                                                       : Qt::ArrowCursor);
        return;
    }
    const qint64 delta = toMiB(pos.x()) - toMiB(m_pressX);
    const qint64 end = m_pressBefore + m_pressSize;
    switch (m_drag) {
    case Drag::Left:
        apply(m_pressBefore + delta, end - m_pressBefore - delta, Drag::Left);
        break;
    case Drag::Right:
        apply(m_pressBefore, m_pressSize + delta, Drag::Right);
        break;
    case Drag::Move:
        setCursor(Qt::ClosedHandCursor);
        apply(m_pressBefore + delta, m_pressSize, Drag::Move);
        break;
    case Drag::None:
        break;
    }
}

void PartitionResizer::mouseReleaseEvent(QMouseEvent *)
{
    m_drag = Drag::None;
    unsetCursor();
}

void PartitionResizer::keyPressEvent(QKeyEvent *event)
{
    // Arrows move, Shift+arrows resize from the right edge.
    const qint64 step = std::max<qint64>(1, m_total / 100);
    if (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right) {
        const qint64 d = event->key() == Qt::Key_Left ? -step : step;
        if ((event->modifiers() & Qt::ShiftModifier) || m_fixedStart)
            apply(m_before, m_size + d, Drag::Right);
        else
            apply(m_before + d, m_size, Drag::Move);
        return;
    }
    QWidget::keyPressEvent(event);
}

} // namespace QtParted

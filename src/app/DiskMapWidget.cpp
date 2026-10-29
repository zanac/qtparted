/*
 * QtParted - a Qt frontend to libparted for manipulating disk partitions
 * Copyright (C) 2002-2026 Vanni Brutto <zanac4ever@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "DiskMapWidget.h"

#include "UiHelpers.h"

#include <QContextMenuEvent>
#include <QHelpEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>

namespace QtParted {

namespace {
constexpr int kMargin = 6;
constexpr int kBorder = 4;    // coloured frame showing the file system type
constexpr int kMinWidth = 18; // smallest box drawn for tiny partitions
constexpr int kGap = 3;
}

DiskMapWidget::DiskMapWidget(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(false);
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setAccessibleName(tr("Disk map"));
}

QSize DiskMapWidget::sizeHint() const
{
    return QSize(600, 84);
}

QSize DiskMapWidget::minimumSizeHint() const
{
    return QSize(200, 84);
}

void DiskMapWidget::setDevice(const Device &device)
{
    m_device = device;
    layoutBoxes();
    update();
}

void DiskMapWidget::setSelectedKey(const QString &key)
{
    if (m_selectedKey == key)
        return;
    m_selectedKey = key;
    update();
}

QList<DiskMapWidget::Box> DiskMapWidget::layoutRow(const PartitionVector &partitions, const QRect &area,
                                                    Sector total) const
{
    QList<Box> boxes;
    const int n = int(partitions.size());
    if (n == 0 || total <= 0)
        return boxes;
    const int gaps = (n - 1) * kGap;
    const int available = std::max(0, area.width() - gaps);
    const int extra = std::max(0, available - n * kMinWidth);

    int x = area.left();
    for (int i = 0; i < n; ++i) {
        const Partition &p = partitions[size_t(i)];
        int w = kMinWidth + int(double(extra) * double(p.length()) / double(total));
        if (i == n - 1)
            w = area.right() + 1 - x;  // absorb rounding
        Box b;
        b.rect = QRect(x, area.top(), std::max(w, 4), area.height());
        b.partition = p;
        if (p.type == PartitionType::Extended && !p.logicals.empty()) {
            const QRect inner = b.rect.adjusted(kBorder + 1, kBorder + 1, -(kBorder + 1), -(kBorder + 1));
            b.logicals = layoutRow(p.logicals, inner, p.length());
        }
        boxes.append(b);
        x += w + kGap;
    }
    return boxes;
}

void DiskMapWidget::layoutBoxes()
{
    const QRect area = rect().adjusted(kMargin, kMargin, -kMargin, -kMargin);
    m_boxes = layoutRow(m_device.partitions, area, m_device.length);
}

void DiskMapWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    layoutBoxes();
}

void DiskMapWidget::paintBox(QPainter &painter, const Box &box) const
{
    const Partition &p = box.partition;
    const QPalette &pal = palette();
    const QRect r = box.rect;
    const QColor frame = Ui::fsColor(p.fsType);

    if (p.type == PartitionType::Extended) {
        painter.fillRect(r, frame);
        painter.fillRect(r.adjusted(kBorder, kBorder, -kBorder, -kBorder), pal.color(QPalette::Window));
        for (const Box &l : box.logicals)
            paintBox(painter, l);
    } else {
        painter.fillRect(r, frame);
        const QRect inner = r.adjusted(kBorder, kBorder, -kBorder, -kBorder);
        if (inner.width() > 0 && inner.height() > 0) {
            painter.fillRect(inner, Ui::unusedColor(pal));
            if (p.fsType == FsType::Unallocated) {
                painter.fillRect(inner, QColor(frame.red(), frame.green(), frame.blue(), 60));
            } else if (p.usageKnown() && p.length() > 0) {
                const int usedW = int(double(inner.width()) * double(p.sectorsUsed) / double(p.length()));
                painter.fillRect(QRect(inner.left(), inner.top(), usedW, inner.height()), Ui::usedColor(pal));
                if (p.sectorsUnallocated > 0) {
                    const int unW = int(double(inner.width()) * double(p.sectorsUnallocated) / double(p.length()));
                    painter.fillRect(QRect(inner.right() + 1 - unW, inner.top(), unW, inner.height()),
                                     QColor(0xA9, 0xA9, 0xA9, 110));
                }
            }

            // Two lines of text when there is room: path and size.
            const QFontMetrics fm = painter.fontMetrics();
            const QRect textRect = inner.adjusted(3, 2, -3, -2);
            if (textRect.width() > fm.averageCharWidth() * 3) {
                painter.setPen(pal.color(QPalette::Text));
                const QString first = p.displayPath();
                const QString second = formatSize(p.length(), p.sectorSize);
                const int lineH = fm.height();
                const int totalH = textRect.height() >= 2 * lineH ? 2 * lineH : lineH;
                const int top = textRect.top() + (textRect.height() - totalH) / 2;
                painter.drawText(QRect(textRect.left(), top, textRect.width(), lineH), Qt::AlignCenter,
                                 fm.elidedText(first, Qt::ElideMiddle, textRect.width()));
                if (totalH == 2 * lineH)
                    painter.drawText(QRect(textRect.left(), top + lineH, textRect.width(), lineH), Qt::AlignCenter,
                                     fm.elidedText(second, Qt::ElideRight, textRect.width()));
            }
            // In-use partitions get a lock in the corner.
            if (p.busy && inner.width() > 24 && inner.height() > 16) {
                const QIcon lock = Ui::icon("object-locked", QStyle::SP_VistaShield);
                lock.paint(&painter, QRect(inner.right() - 13, inner.top() + 1, 12, 12));
            }
        }
    }

    if (Ui::partitionKey(p) == m_selectedKey) {
        QPen pen(pal.color(QPalette::Highlight), 2, Qt::SolidLine);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(QRectF(r).adjusted(1, 1, -1, -1));
        if (hasFocus()) {
            pen.setStyle(Qt::DotLine);
            pen.setWidth(1);
            pen.setColor(pal.color(QPalette::HighlightedText));
            painter.setPen(pen);
            painter.drawRect(QRectF(r).adjusted(3, 3, -3, -3));
        }
    }
}

void DiskMapWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), palette().color(QPalette::Window));
    if (m_boxes.isEmpty()) {
        painter.setPen(palette().color(QPalette::PlaceholderText));
        painter.drawText(rect(), Qt::AlignCenter, tr("No device selected"));
        return;
    }
    for (const Box &b : m_boxes)
        paintBox(painter, b);
}

const DiskMapWidget::Box *DiskMapWidget::hitTest(const QPoint &pos) const
{
    for (const Box &b : m_boxes) {
        if (!b.rect.contains(pos))
            continue;
        for (const Box &l : b.logicals)
            if (l.rect.contains(pos))
                return &l;
        return &b;
    }
    return nullptr;
}

void DiskMapWidget::mousePressEvent(QMouseEvent *event)
{
    if (const Box *b = hitTest(event->position().toPoint())) {
        const QString key = Ui::partitionKey(b->partition);
        setSelectedKey(key);
        Q_EMIT partitionSelected(key);
    }
    QWidget::mousePressEvent(event);
}

void DiskMapWidget::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (const Box *b = hitTest(event->position().toPoint()))
        Q_EMIT partitionActivated(Ui::partitionKey(b->partition));
}

void DiskMapWidget::contextMenuEvent(QContextMenuEvent *event)
{
    if (const Box *b = hitTest(event->pos())) {
        const QString key = Ui::partitionKey(b->partition);
        setSelectedKey(key);
        Q_EMIT partitionSelected(key);
        Q_EMIT contextMenuRequested(event->globalPos());
    }
}

bool DiskMapWidget::event(QEvent *event)
{
    if (event->type() == QEvent::ToolTip) {
        auto *help = static_cast<QHelpEvent *>(event);
        if (const Box *b = hitTest(help->pos())) {
            const Partition &p = b->partition;
            QString tip = QStringLiteral("<b>%1</b><br>%2 &middot; %3")
                              .arg(p.displayPath().toHtmlEscaped(), fsName(p.fsType).toHtmlEscaped(),
                                   formatSize(p.length(), p.sectorSize));
            if (p.usageKnown())
                tip += QLatin1String("<br>") + tr("Used: %1").arg(formatSize(p.sectorsUsed, p.sectorSize));
            if (!p.mountPoints.isEmpty())
                tip += QLatin1String("<br>") + p.mountPointsText().toHtmlEscaped();
            QToolTip::showText(help->globalPos(), tip, this);
        } else {
            QToolTip::hideText();
        }
        return true;
    }
    return QWidget::event(event);
}

} // namespace QtParted

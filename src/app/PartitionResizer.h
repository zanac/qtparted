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

#include <QColor>
#include <QWidget>

namespace QtParted {

// Graphical editor of a partition inside free space: drag the edges to resize,
// drag the middle to move.  All values are MiB.
class PartitionResizer : public QWidget
{
    Q_OBJECT
public:
    explicit PartitionResizer(QWidget *parent = nullptr);

    void setTotal(qint64 totalMiB);
    void setLimits(qint64 minBeforeMiB, qint64 minSizeMiB, qint64 maxSizeMiB);
    void setValues(qint64 beforeMiB, qint64 sizeMiB);
    // Resize/move: the furthest the start may go, the range the end must stay in,
    // whether the start is fixed, and the used space to draw.  `usedStartMiB` < 0
    // draws the used space from the partition start (a file system), otherwise at
    // that fixed position (the logical partitions of an extended partition).
    void setMaxBefore(qint64 maxBeforeMiB);
    void setEndRange(qint64 minEndMiB, qint64 maxEndMiB);
    void setFixedStart(bool fixed);
    void setUsed(qint64 usedMiB, qint64 usedStartMiB = -1);
    void setPartitionColor(const QColor &color);
    void setExtendedBackground(bool extended);

    qint64 before() const { return m_before; }
    qint64 size() const { return m_size; }

    QSize sizeHint() const override { return QSize(500, 56); }
    QSize minimumSizeHint() const override { return QSize(200, 56); }

Q_SIGNALS:
    void valuesChanged(qint64 beforeMiB, qint64 sizeMiB);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    enum class Drag { None, Left, Right, Move };

    QRect track() const;
    int toX(qint64 mib) const;
    qint64 toMiB(int x) const;
    QRect partitionRect() const;
    Drag hit(const QPoint &pos) const;
    void apply(qint64 before, qint64 size, Drag mode);

    qint64 m_total = 1;
    qint64 m_minBefore = 0;
    qint64 m_maxBefore = -1;  // -1: no limit besides the total
    qint64 m_minEnd = 0;
    qint64 m_maxEnd = -1;     // -1: the total
    bool m_fixedStart = false;
    qint64 m_used = 0;
    qint64 m_usedStart = -1;
    qint64 m_minSize = 1;
    qint64 m_maxSize = 1;
    qint64 m_before = 0;
    qint64 m_size = 1;
    QColor m_color = Qt::darkBlue;
    bool m_extended = false;

    Drag m_drag = Drag::None;
    int m_pressX = 0;
    qint64 m_pressBefore = 0;
    qint64 m_pressSize = 0;
};

} // namespace QtParted

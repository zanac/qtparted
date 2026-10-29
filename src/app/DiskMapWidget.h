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

#include "Device.h"

#include <QWidget>

namespace QtParted {

// The graphical map of a disk: one box per partition, sized by length, with the
// used space shaded and logical partitions drawn inside the extended one.
class DiskMapWidget : public QWidget
{
    Q_OBJECT
public:
    explicit DiskMapWidget(QWidget *parent = nullptr);

    void setDevice(const Device &device);
    void setSelectedKey(const QString &key);
    QString selectedKey() const { return m_selectedKey; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

Q_SIGNALS:
    void partitionSelected(const QString &key);
    void partitionActivated(const QString &key);
    void contextMenuRequested(const QPoint &globalPos);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    bool event(QEvent *event) override;

private:
    struct Box {
        QRect rect;
        Partition partition;
        QList<Box> logicals;
    };

    void layoutBoxes();
    QList<Box> layoutRow(const PartitionVector &partitions, const QRect &area, Sector total) const;
    void paintBox(QPainter &painter, const Box &box) const;
    const Box *hitTest(const QPoint &pos) const;

    Device m_device;
    QList<Box> m_boxes;
    QString m_selectedKey;
};

} // namespace QtParted

/*
 * QtParted - a Qt frontend to libparted for manipulating disk partitions
 * Copyright (C) 2002-2026 Vanni Brutto <zanac4ever@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "DialogManual.h"

#include "UiHelpers.h"

#include <QApplication>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QHBoxLayout>
#include <QVBoxLayout>

namespace QtParted {

DialogManual::DialogManual(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("QtParted Manual"));
    m_browser = new QTextBrowser(this);
    m_browser->setOpenLinks(false);
    m_browser->setSearchPaths({ QStringLiteral(":/manual") });
    QFile f(QStringLiteral(":/manual/manual.md"));
    if (f.open(QIODevice::ReadOnly)) {
        QString md = QString::fromUtf8(f.readAll());
        // The manual is also read on GitHub, where screenshots are in ../screenshots.
        md.replace(QStringLiteral("](../screenshots/"), QStringLiteral("](qrc:/screenshots/"));
        m_browser->setMarkdown(md);
        addHeadingAnchors();
    }
    connect(m_browser, &QTextBrowser::anchorClicked, this, &DialogManual::linkClicked);

    m_status = new QLabel(this);
    m_status->setForegroundRole(QPalette::PlaceholderText);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    QPushButton *top = buttons->addButton(tr("&Contents"), QDialogButtonBox::ActionRole);
    top->setIcon(Ui::icon("go-top", QStyle::SP_ArrowUp));
    connect(top, &QPushButton::clicked, this, [this]() { m_browser->scrollToAnchor(QStringLiteral("contents")); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_browser, 1);
    auto *bottom = new QHBoxLayout;
    bottom->addWidget(m_status, 1);
    bottom->addWidget(buttons);
    layout->addLayout(bottom);
    resize(900, 760);
}

QString DialogManual::headingAnchor(const QString &heading)
{
    QString a;
    for (const QChar c : heading.toLower()) {
        if (c.isLetterOrNumber() || c == QLatin1Char('-') || c == QLatin1Char('_'))
            a += c;
        else if (c == QLatin1Char(' '))
            a += QLatin1Char('-');
    }
    return a;
}

void DialogManual::addHeadingAnchors()
{
    // Qt's Markdown reader gives headings no anchors: add GitHub's, so the links of the
    // table of contents work.
    QTextDocument *doc = m_browser->document();
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        // Screenshots scaled to the page width.
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment frag = it.fragment();
            if (!frag.isValid() || !frag.charFormat().isImageFormat())
                continue;
            QTextImageFormat img = frag.charFormat().toImageFormat();
            if (img.width() <= 0) {
                img.setWidth(760);
                QTextCursor ic(doc);
                ic.setPosition(frag.position());
                ic.setPosition(frag.position() + frag.length(), QTextCursor::KeepAnchor);
                ic.setCharFormat(img);
            }
        }
        if (b.blockFormat().headingLevel() == 0 || b.text().isEmpty())
            continue;
        QTextCharFormat fmt;
        fmt.setAnchor(true);
        fmt.setAnchorNames({ headingAnchor(b.text()) });
        QTextCursor word(b);
        word.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        word.mergeCharFormat(fmt);
    }
}

void DialogManual::linkClicked(const QUrl &url)
{
    if (url.scheme().isEmpty() && url.hasFragment()) {
        m_browser->scrollToAnchor(url.fragment());
        return;
    }
    // No web browser as root: offer the address instead.
    QApplication::clipboard()->setText(url.toString());
    m_status->setText(tr("Link copied to the clipboard: %1").arg(url.toString()));
}

} // namespace QtParted

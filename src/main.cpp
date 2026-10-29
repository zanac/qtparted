/*
 * QtParted - a Qt frontend to libparted for manipulating disk partitions
 * Copyright (C) 2002-2026 Vanni Brutto <zanac4ever@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "FileSystem.h"
#include "Luks.h"
#include "MainWindow.h"
#include "Parted.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QIcon>
#include <QLibraryInfo>
#include <QLocale>
#include <QTimer>
#include <QTranslator>

#include <cstdlib>
#include <cstring>
#include <string>

#ifndef QTPARTED_DATADIR
#define QTPARTED_DATADIR "/usr/share/qtparted"
#endif

// The pkexec launcher forwards a few settings of the user's graphical session
// (pkexec clears the environment).  Only variables that cannot make the root
// process load code from user controlled paths are accepted.  They must be set
// before QApplication is created.
static void applySessionEnvironment(int argc, char **argv)
{
    static const char *const allowed[] = {
        "WAYLAND_DISPLAY", "XDG_RUNTIME_DIR", "XDG_CURRENT_DESKTOP", "XDG_SESSION_TYPE",
        "QT_QPA_PLATFORMTHEME", "QT_SCALE_FACTOR", "QT_SCREEN_SCALE_FACTORS", "QT_AUTO_SCREEN_SCALE_FACTOR",
        "KDE_FULL_SESSION", "KDE_SESSION_VERSION", "XCURSOR_THEME", "XCURSOR_SIZE",
        "LANG", "LANGUAGE", "LC_ALL", "LC_MESSAGES",
    };
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--session-env") != 0)
            continue;
        const char *assignment = argv[i + 1];
        const char *eq = std::strchr(assignment, '=');
        if (!eq)
            continue;
        const std::string name(assignment, size_t(eq - assignment));
        for (const char *a : allowed)
            if (name == a)
                ::setenv(name.c_str(), eq + 1, 1);
    }
}

namespace {

void setApplicationInfo()
{
    QCoreApplication::setOrganizationName(QStringLiteral("QtParted"));
    QCoreApplication::setApplicationName(QStringLiteral("qtparted"));
    QCoreApplication::setApplicationVersion(QStringLiteral(QTPARTED_VERSION));
}

// English first, for the plural forms of the source texts; the user's language
// (installed later, so it takes precedence) on top.  Translations are built into the
// program; a qtparted_<lang>.qm in the data directory can add or override one.
void installTranslators(QTranslator &qt, QTranslator &english, QTranslator &translator)
{
    if (qt.load(QLocale(), QStringLiteral("qtbase"), QStringLiteral("_"),
                QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
        QCoreApplication::installTranslator(&qt);
    if (english.load(QStringLiteral(":/i18n/qtparted_en.qm")))
        QCoreApplication::installTranslator(&english);
    if (translator.load(QLocale(), QStringLiteral("qtparted"), QStringLiteral("_"),
                        QStringLiteral(QTPARTED_DATADIR "/translations"))
        || translator.load(QLocale(), QStringLiteral("qtparted"), QStringLiteral("_"), QStringLiteral(":/i18n")))
        QCoreApplication::installTranslator(&translator);
}

void setupParser(QCommandLineParser &parser)
{
    parser.setApplicationDescription(QCoreApplication::translate("main", "Partition editor based on libparted."));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption sessionEnv(QStringLiteral("session-env"),
                                  QCoreApplication::translate("main", "Setting of the user session (used by the launcher)."),
                                  QStringLiteral("name=value"));
    sessionEnv.setFlags(QCommandLineOption::HiddenFromHelp);
    parser.addOption(sessionEnv);
    parser.addPositionalArgument(QStringLiteral("devices"),
                                 QCoreApplication::translate("main", "Devices or disk image files to edit "
                                                                     "(default: all disks)."),
                                 QStringLiteral("[device...]"));
}

bool onlyInformation(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i)
        for (const char *o : { "-h", "--help", "-?", "--help-all", "-v", "--version" })
            if (std::strcmp(argv[i], o) == 0)
                return true;
    return false;
}

} // namespace

int main(int argc, char **argv)
{
    applySessionEnvironment(argc, argv);

    if (onlyInformation(argc, argv)) {
        // --help and --version work without a display.
        QCoreApplication core(argc, argv);
        setApplicationInfo();
        QTranslator qt, english, translator;
        installTranslators(qt, english, translator);
        QCommandLineParser parser;
        setupParser(parser);
        parser.process(core);  // prints and exits
        return 0;
    }

    QApplication app(argc, argv);
    setApplicationInfo();
    QApplication::setApplicationDisplayName(QStringLiteral("QtParted"));
    QApplication::setDesktopFileName(QStringLiteral("io.github.zanac.qtparted"));
    // Outside Plasma (rescue systems, other desktops) look up missing icons in Breeze.
    if (QIcon::fallbackThemeName().isEmpty() || QIcon::fallbackThemeName() == QLatin1String("hicolor"))
        QIcon::setFallbackThemeName(QStringLiteral("breeze"));

    QTranslator qt, english, translator;
    installTranslators(qt, english, translator);
    QCommandLineParser parser;
    setupParser(parser);
    parser.process(app);

    QtParted::Parted::initialize();
    QtParted::FileSystemRegistry::instance();  // look for the file system tools

    QtParted::MainWindow window(parser.positionalArguments());
    window.show();
    QTimer::singleShot(0, &window, &QtParted::MainWindow::refreshDevices);
    const int rc = app.exec();
    QtParted::PasswordStore::clear();  // wipe LUKS passphrases from memory
    return rc;
}

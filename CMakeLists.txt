cmake_minimum_required(VERSION 3.19)

project(QtParted
    VERSION 1.0.0
    DESCRIPTION "Partition editor for Qt 6 based on libparted"
    HOMEPAGE_URL "https://github.com/zanac/qtparted"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

include(GNUInstallDirs)

option(QTPARTED_BUILD_GUI "Build the graphical application" ON)
option(QTPARTED_BUILD_TESTS "Build the unit tests" ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Core)
if(QTPARTED_BUILD_GUI)
    find_package(Qt6 6.4 REQUIRED COMPONENTS Gui Widgets)
endif()
find_package(PkgConfig REQUIRED)
pkg_check_modules(PARTED REQUIRED IMPORTED_TARGET libparted>=3.1)
# FAT and HFS(+) resizing (part of parted since 3.1)
pkg_check_modules(PARTED_FS_RESIZE REQUIRED IMPORTED_TARGET libparted-fs-resize)
pkg_check_modules(BLKID REQUIRED IMPORTED_TARGET blkid)

# The program binary goes to libexec; `qtparted` in bin is the launcher script that
# obtains root privileges through pkexec, like GParted does.
set(QTPARTED_LIBEXECDIR "${CMAKE_INSTALL_FULL_LIBEXECDIR}/qtparted")

add_compile_definitions(
    QTPARTED_VERSION="${PROJECT_VERSION}"
    QTPARTED_DATADIR="${CMAKE_INSTALL_FULL_DATADIR}/qtparted"
    QT_NO_KEYWORDS
    QT_NO_CAST_FROM_ASCII
    QT_DISABLE_DEPRECATED_UP_TO=0x060400)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

add_subdirectory(src)
if(QTPARTED_BUILD_GUI)
    add_subdirectory(data)
endif()

if(QTPARTED_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

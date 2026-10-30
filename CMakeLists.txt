cmake_minimum_required(VERSION 3.19)

project(QtParted
    VERSION 1.0.2
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
install(FILES README.md ChangeLog AUTHORS DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/qtparted)
if(QTPARTED_BUILD_GUI)
    add_subdirectory(data)
endif()

if(QTPARTED_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# --- Packages: cpack -G DEB / cpack -G RPM -----------------------------------------
set(CPACK_PACKAGE_NAME "qtparted")
set(CPACK_PACKAGE_VENDOR "Vanni Brutto")
set(CPACK_PACKAGE_CONTACT "Vanni Brutto <zanac4ever@gmail.com>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Partition editor for Qt 6 based on libparted")
set(CPACK_PACKAGE_DESCRIPTION "QtParted creates, deletes, resizes, moves, copies, checks and formats disk \
partitions, opens and resizes LUKS encrypted volumes, mounts file systems and activates swap and LVM \
Volume Groups. Changes are planned on screen and applied together, with a detailed log.")
set(CPACK_PACKAGE_HOMEPAGE_URL "${PROJECT_HOMEPAGE_URL}")
set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/COPYING")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_STRIP_FILES ON)
set(CPACK_SOURCE_GENERATOR "TXZ")
set(CPACK_SOURCE_IGNORE_FILES "/\\\\.git/" "/build/" "~$")

set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SECTION "admin")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "pkexec | policykit-1, qt6-svg-plugins | libqt6svg6")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS
    "e2fsprogs, dosfstools, mtools, ntfs-3g, btrfs-progs, xfsprogs, exfatprogs, cryptsetup-bin, dmsetup, lvm2")
set(CPACK_DEBIAN_PACKAGE_SUGGESTS "f2fs-tools, jfsutils, nilfs-tools, reiserfsprogs, udftools, hfsprogs")

set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
set(CPACK_RPM_PACKAGE_LICENSE "GPL-2.0-or-later")
set(CPACK_RPM_PACKAGE_GROUP "Applications/System")
set(CPACK_RPM_PACKAGE_REQUIRES "polkit, qt6-qtsvg")
set(CPACK_RPM_PACKAGE_SUGGESTS "e2fsprogs, dosfstools, mtools, ntfs-3g, btrfs-progs, xfsprogs, exfatprogs, cryptsetup, lvm2")
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
    /usr/share/applications /usr/share/metainfo /usr/share/icons /usr/share/icons/hicolor
    /usr/share/polkit-1 /usr/share/polkit-1/actions /usr/share/man /usr/share/man/man1 /usr/libexec
    /usr/share/doc)
include(CPack)

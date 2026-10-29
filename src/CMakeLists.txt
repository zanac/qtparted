# Core library: no GUI, only QtCore + libparted + libblkid.  Used by the
# application and the tests.
add_library(qtparted-core STATIC
    core/BusyActions.cpp
    core/Command.cpp
    core/FileSystem.cpp
    core/FsMaintenance.cpp
    core/FsType.cpp
    core/Layout.cpp
    core/Luks.cpp
    core/Operation.cpp
    core/OperationDetail.cpp
    core/Parted.cpp
    core/Partition.cpp
    core/Probe.cpp
    core/Session.cpp
    core/Steps.cpp
    core/SystemInfo.cpp
    core/Units.cpp
    core/Worker.cpp
)
target_include_directories(qtparted-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/core)
target_link_libraries(qtparted-core PUBLIC Qt6::Core PkgConfig::PARTED PkgConfig::PARTED_FS_RESIZE PkgConfig::BLKID)

if(NOT QTPARTED_BUILD_GUI)
    return()
endif()

add_executable(qtparted
    main.cpp
    app/DialogApply.cpp
    app/DialogCreateTable.cpp
    app/DialogFlags.cpp
    app/DialogFsSupport.cpp
    app/DialogManual.cpp
    app/DialogName.cpp
    app/DialogNewPartition.cpp
    app/DialogPartitionInfo.cpp
    app/DialogPassword.cpp
    app/DialogResizeMove.cpp
    app/DiskMapWidget.cpp
    app/MainWindow.cpp
    app/PartitionResizer.cpp
    app/PartitionTreeModel.cpp
    app/UiHelpers.cpp
    app/qtparted.qrc
)
set_target_properties(qtparted PROPERTIES AUTORCC ON)
target_include_directories(qtparted PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/app)
target_link_libraries(qtparted PRIVATE qtparted-core Qt6::Widgets)

# Translations, built into the program (":/i18n/qtparted_<lang>.qm").  "cmake --build
# build --target update_translations" refreshes the .ts files from the sources.
find_package(Qt6 QUIET COMPONENTS LinguistTools)
if(Qt6LinguistTools_FOUND)
    file(GLOB QTPARTED_TS_FILES ${PROJECT_SOURCE_DIR}/translations/qtparted_*.ts)
    # The core library's strings too, not only the program's own sources.
    file(GLOB_RECURSE QTPARTED_I18N_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/*.h)
    qt_add_translations(qtparted
        TS_FILES ${QTPARTED_TS_FILES}
        SOURCES ${QTPARTED_I18N_SOURCES}
        RESOURCE_PREFIX /i18n
        LUPDATE_OPTIONS -locations relative -no-obsolete)
else()
    message(WARNING "Qt6 LinguistTools not found: QtParted is built without translations")
endif()

install(TARGETS qtparted RUNTIME DESTINATION ${QTPARTED_LIBEXECDIR})

# Core library: no GUI, only QtCore + libparted + libblkid.  Used by the
# application and the tests.
add_library(qtparted-core STATIC
    core/Command.cpp
    core/FileSystem.cpp
    core/FsType.cpp
    core/Layout.cpp
    core/Operation.cpp
    core/OperationDetail.cpp
    core/Parted.cpp
    core/Partition.cpp
    core/Probe.cpp
    core/Session.cpp
    core/SystemInfo.cpp
    core/Units.cpp
    core/Worker.cpp
)
target_include_directories(qtparted-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/core)
target_link_libraries(qtparted-core PUBLIC Qt6::Core PkgConfig::PARTED PkgConfig::BLKID)

if(NOT QTPARTED_BUILD_GUI)
    return()
endif()

add_executable(qtparted
    main.cpp
    app/DialogApply.cpp
    app/DialogCreateTable.cpp
    app/DialogFlags.cpp
    app/DialogFsSupport.cpp
    app/DialogName.cpp
    app/DialogNewPartition.cpp
    app/DialogPartitionInfo.cpp
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

install(TARGETS qtparted RUNTIME DESTINATION ${QTPARTED_LIBEXECDIR})

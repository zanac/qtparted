configure_file(qtparted.in ${CMAKE_CURRENT_BINARY_DIR}/qtparted @ONLY)
install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/qtparted DESTINATION ${CMAKE_INSTALL_BINDIR})

configure_file(io.github.zanac.qtparted.policy.in ${CMAKE_CURRENT_BINARY_DIR}/io.github.zanac.qtparted.policy @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/io.github.zanac.qtparted.policy
        DESTINATION ${CMAKE_INSTALL_DATADIR}/polkit-1/actions)

install(FILES io.github.zanac.qtparted.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES io.github.zanac.qtparted.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)

foreach(size 16 32 64)
    install(FILES icons/qtparted-${size}.png
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps
            RENAME qtparted.png)
endforeach()

install(FILES ${PROJECT_SOURCE_DIR}/doc/qtparted.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)

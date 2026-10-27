find_package(PkgConfig REQUIRED)
# 1.5 for AdwDialog (import and settings views); 1.4 for AdwToolbarView and AdwOverlaySplitView.
pkg_check_modules(ADW REQUIRED IMPORTED_TARGET libadwaita-1>=1.5 gtk4>=4.12)

add_executable(musicapp
  src/main.c
  src/album_grid.c
  src/cover.c
  src/mpris.c
  src/now_playing.c
  src/playback.c
  src/track_list.c
  src/import_view.c
  src/settings_view.c
  src/add_device.c
  src/add_to_playlist.c
  src/playlist_dialogs.c)
target_link_libraries(musicapp PRIVATE musiccore PkgConfig::ADW m)
musicapp_warnings(musicapp)
# GLib's headers trip -Wconversion / -Wpedantic; keep warnings strict for our code only.
target_compile_options(musicapp PRIVATE -Wno-pedantic)

# Installed layout, for the Flatpak (packaging/flatpak) or any other prefix. The musicapp
# component leaves out the fetched libraries' own install rules.
include(GNUInstallDirs)
include(Licenses)
set(_app_id app.musicapp.Musicapp)
string(TIMESTAMP MUSICAPP_RELEASE_DATE "%Y-%m-%d" UTC)  # honours SOURCE_DATE_EPOCH
configure_file(data/${_app_id}.metainfo.xml.in "${CMAKE_CURRENT_BINARY_DIR}/${_app_id}.metainfo.xml" @ONLY)
install(TARGETS musicapp RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT musicapp)
install(FILES data/${_app_id}.desktop
  DESTINATION "${CMAKE_INSTALL_DATADIR}/applications" COMPONENT musicapp)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${_app_id}.metainfo.xml"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/metainfo" COMPONENT musicapp)
install(FILES "${PROJECT_SOURCE_DIR}/packaging/icons/musicapp.svg"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps" RENAME ${_app_id}.svg
  COMPONENT musicapp)
foreach(_size 128 256 512)
  install(FILES "${PROJECT_SOURCE_DIR}/packaging/icons/musicapp-${_size}.png"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${_size}x${_size}/apps" RENAME ${_app_id}.png
    COMPONENT musicapp)
endforeach()
musicapp_install_licenses("${CMAKE_INSTALL_DATADIR}/licenses/${_app_id}")

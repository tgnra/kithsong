enable_language(RC)

# The manifest, version resource and build_info.h carry PROJECT_VERSION or
# MUSICAPP_BUILD_NUMBER, so they're configured.
set(MUSICAPP_MANIFEST "${CMAKE_CURRENT_BINARY_DIR}/musicapp.manifest")
set(MUSICAPP_ICON "${PROJECT_SOURCE_DIR}/packaging/icons/musicapp.ico")
configure_file(src/musicapp.manifest.in "${MUSICAPP_MANIFEST}" @ONLY)
configure_file(src/version.rc.in "${CMAKE_CURRENT_BINARY_DIR}/version.rc" @ONLY)
configure_file(src/build_info.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/build_info.h" @ONLY)

add_executable(musicapp WIN32
  src/main.c
  src/add_device.c
  src/add_to_playlist.c
  src/album_grid.c
  src/canvas.c
  src/cover.c
  src/device_view.c
  src/firewall.c
  src/import_view.c
  src/media_controls.c
  src/move_library.c
  src/now_playing.c
  src/pair_dialog.c
  src/pick_folder.c
  src/playback.c
  src/playlist_dialogs.c
  src/scroller.c
  src/search_view.c
  src/settings_view.c
  src/sidebar.c
  src/sync_bridge.c
  src/taskbar.c
  src/theme.c
  src/track_list.c
  src/wstr.c
  src/app.rc
  "${CMAKE_CURRENT_BINARY_DIR}/version.rc")
# version.rc lives in the build dir but includes resource.h; build_info.h is generated.
target_include_directories(musicapp PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}/generated")
set_property(SOURCE "${CMAKE_CURRENT_BINARY_DIR}/version.rc" APPEND PROPERTY
  OBJECT_DEPENDS "${MUSICAPP_MANIFEST}" "${MUSICAPP_ICON}")
target_link_libraries(musicapp PRIVATE musiccore comctl32 ole32 oleaut32 uuid shell32 windowscodecs dwmapi uxtheme runtimeobject shcore shlwapi)
target_compile_definitions(musicapp PRIVATE
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 WINVER=0x0A00)
musicapp_warnings(musicapp)

# Self-contained exe: no libgcc/libstdc++ DLLs next to it.
if(MINGW)
  target_link_options(musicapp PRIVATE -static)
endif()

# The portable zip's layout (scripts/release-windows.ps1): the exe and LICENSES/ side by side.
# The musicapp component leaves out the fetched libraries' own install rules.
include(Licenses)
install(TARGETS musicapp RUNTIME DESTINATION . COMPONENT musicapp)
musicapp_install_licenses(LICENSES)

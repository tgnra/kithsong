enable_language(RC)

# The manifest, version resource and build_info.h carry PROJECT_VERSION or
# KITHSONG_BUILD_NUMBER, so they're configured.
set(KITHSONG_MANIFEST "${CMAKE_CURRENT_BINARY_DIR}/kithsong.manifest")
set(KITHSONG_ICON "${PROJECT_SOURCE_DIR}/packaging/icons/kithsong.ico")
configure_file(src/kithsong.manifest.in "${KITHSONG_MANIFEST}" @ONLY)
configure_file(src/version.rc.in "${CMAKE_CURRENT_BINARY_DIR}/version.rc" @ONLY)
configure_file(src/build_info.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/build_info.h" @ONLY)

add_executable(kithsong WIN32
  src/main.c
  src/add_device.c
  src/add_to_playlist.c
  src/album_grid.c
  src/artist_view.c
  src/canvas.c
  src/cover.c
  src/delete_dialog.c
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
  src/uia.c
  src/wstr.c
  src/app.rc
  "${CMAKE_CURRENT_BINARY_DIR}/version.rc")
# version.rc lives in the build dir but includes resource.h; build_info.h is generated.
target_include_directories(kithsong PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}/generated")
set_property(SOURCE "${CMAKE_CURRENT_BINARY_DIR}/version.rc" APPEND PROPERTY
  OBJECT_DEPENDS "${KITHSONG_MANIFEST}" "${KITHSONG_ICON}")
target_link_libraries(kithsong PRIVATE musiccore comctl32 ole32 oleaut32 uuid shell32 windowscodecs dwmapi uxtheme runtimeobject shcore shlwapi uiautomationcore)
target_compile_definitions(kithsong PRIVATE
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 WINVER=0x0A00)
kithsong_warnings(kithsong)

# Self-contained exe: no libgcc/libstdc++ DLLs next to it.
if(MINGW)
  target_link_options(kithsong PRIVATE -static)
endif()

# The portable zip's layout (scripts/release-windows.ps1): the exe and LICENSES/ side by side.
# The kithsong component leaves out the fetched libraries' own install rules.
include(Licenses)
install(TARGETS kithsong RUNTIME DESTINATION . COMPONENT kithsong)
kithsong_install_licenses(LICENSES)

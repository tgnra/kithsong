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

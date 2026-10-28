cmake_minimum_required(VERSION 3.28)
project(kithsong_android C CXX)

# Build the shared core from the repo root. On Android the root project only builds the
# core (no desktop frontends, no tests).
set(KITHSONG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../..")
add_subdirectory("${KITHSONG_ROOT}" kithsong)

add_library(kithsong_jni SHARED jni_bridge.c jni_browse.c jni_scrobbler.c)
target_link_libraries(kithsong_jni PRIVATE musiccore log)

# Playlist and album group editing (PlaylistNatives, AlbumGroupNatives), apart from jni_bridge.c.
target_sources(kithsong_jni PRIVATE jni_playlist_edit.c)

# The core's settings.json (CoreSettings), apart from jni_bridge.c.
target_sources(kithsong_jni PRIVATE jni_settings.c)

# Deleting albums, artists and tracks (DeleteNatives), apart from jni_bridge.c.
target_sources(kithsong_jni PRIVATE jni_delete.c)

# Removing an album or artist from this phone only (SyncHereNatives), apart from jni_bridge.c.
target_sources(kithsong_jni PRIVATE jni_sync_here.c)

# The native libraries' license texts, for the apps' licence screens (build.gradle.kts).
if(KITHSONG_LICENSES_DIR)
  include("${KITHSONG_ROOT}/cmake/Licenses.cmake")
  kithsong_copy_licenses("${KITHSONG_LICENSES_DIR}/${ANDROID_ABI}")
endif()

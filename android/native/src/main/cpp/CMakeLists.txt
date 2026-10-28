cmake_minimum_required(VERSION 3.28)
project(kithsong_android C CXX)

# Build the shared core from the repo root. On Android the root project only builds the
# core (no desktop frontends, no tests).
set(KITHSONG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../..")
add_subdirectory("${KITHSONG_ROOT}" kithsong)

add_library(kithsong_jni SHARED jni_bridge.c jni_browse.c)
target_link_libraries(kithsong_jni PRIVATE musiccore log)

# Playlist editing (PlaylistNatives), apart from jni_bridge.c.
target_sources(kithsong_jni PRIVATE jni_playlist_edit.c)

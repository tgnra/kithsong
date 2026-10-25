cmake_minimum_required(VERSION 3.28)
project(musicapp_android C CXX)

# Build the shared core from the repo root. On Android the root project only builds the
# core (no desktop frontends, no tests).
set(MUSICAPP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../..")
add_subdirectory("${MUSICAPP_ROOT}" musicapp)

add_library(musicapp_jni SHARED jni_bridge.c)
target_link_libraries(musicapp_jni PRIVATE musiccore log)

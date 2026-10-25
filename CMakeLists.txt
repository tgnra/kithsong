cmake_minimum_required(VERSION 3.28)

project(musicapp
  VERSION 0.1.0
  DESCRIPTION "Cross-platform music library, player and LAN sync"
  LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# The static core is linked into a shared JNI library on Android.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Everything is linked statically into the core so the app ships as few files as possible.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Warnings)

# Frontend selection defaults to whatever the host platform supports.
if(WIN32)
  set(_default_windows ON)
  set(_default_gtk OFF)
elseif(UNIX AND NOT ANDROID AND NOT APPLE)
  set(_default_windows OFF)
  set(_default_gtk ON)
else()
  set(_default_windows OFF)
  set(_default_gtk OFF)
endif()

option(MUSICAPP_BUILD_WINDOWS "Build the Win32 desktop frontend" ${_default_windows})
option(MUSICAPP_BUILD_GTK     "Build the GTK4/libadwaita desktop frontend" ${_default_gtk})
option(MUSICAPP_BUILD_TESTS   "Build core unit tests" ${PROJECT_IS_TOP_LEVEL})
set(MUSICAPP_ACOUSTID_KEY "" CACHE STRING "AcoustID application API key (never commit this)")

if(MUSICAPP_BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(third_party)
add_subdirectory(core)

if(MUSICAPP_BUILD_WINDOWS)
  add_subdirectory(desktop/windows)
endif()
if(MUSICAPP_BUILD_GTK)
  add_subdirectory(desktop/linux)
endif()

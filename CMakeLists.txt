cmake_minimum_required(VERSION 3.28)

# The one version for every platform; the Android builds and the release workflow read it too.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" MUSICAPP_VERSION LIMIT_COUNT 1)

project(musicapp
  VERSION ${MUSICAPP_VERSION}
  DESCRIPTION "Cross-platform music library, player and LAN sync"
  LANGUAGES C CXX)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")

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
set(MUSICAPP_LASTFM_KEY "" CACHE STRING "Last.fm API key, for scrobbling (never commit this)")
set(MUSICAPP_LASTFM_SECRET "" CACHE STRING "Last.fm API shared secret (never commit this)")

# Which build of the version this is: the 4th field of the Windows file version and shown in
# the app. CI sets it to the release workflow's run number; local builds are 0.
set(MUSICAPP_BUILD_NUMBER 0 CACHE STRING "Build number, 0-65535")
if(NOT MUSICAPP_BUILD_NUMBER MATCHES "^[0-9]+$" OR MUSICAPP_BUILD_NUMBER GREATER 65535)
  message(FATAL_ERROR "MUSICAPP_BUILD_NUMBER must be a whole number from 0 to 65535, not '${MUSICAPP_BUILD_NUMBER}'")
endif()

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

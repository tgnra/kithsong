# The Windows toolchain: llvm-mingw, a portable clang that targets x86_64 and aarch64 Windows
# from one install. Included by toolchain-windows-*.cmake once they've set KITHSONG_TRIPLE and
# KITHSONG_WINDOWS_CPU. It's found by full path, so it never has to be on PATH.
#
# The version is pinned in llvm-mingw.pin, and scripts/install-llvm-mingw.ps1 installs it to
# ~/Tools. Set the LLVM_MINGW environment variable to use a copy somewhere else.
#
# On Linux it cross-compiles with llvm-mingw's Linux build, which scripts/install-llvm-mingw.sh
# installs to ~/Tools, and runs what it builds (tests, code generators) under Wine.

file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/llvm-mingw.pin" _release LIMIT_COUNT 1)
if(CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "ARM64")
  set(_host aarch64)
else()
  set(_host x86_64)
endif()
if(CMAKE_HOST_WIN32)
  set(_exe .exe)
  set(_install scripts/install-llvm-mingw.ps1)
  set(_default "$ENV{USERPROFILE}/Tools/llvm-mingw-${_release}-ucrt-${_host}")
else()
  set(_exe "")
  set(_install scripts/install-llvm-mingw.sh)
  set(_default "$ENV{HOME}/Tools/llvm-mingw-${_release}-ucrt-ubuntu-22.04-${_host}")
endif()
if(DEFINED ENV{LLVM_MINGW})
  file(TO_CMAKE_PATH "$ENV{LLVM_MINGW}" _root)
else()
  file(TO_CMAKE_PATH "${_default}" _root)
endif()
if(NOT EXISTS "${_root}/bin/${KITHSONG_TRIPLE}-clang${_exe}")
  message(FATAL_ERROR "No llvm-mingw at ${_root}. Run ${_install}, or set LLVM_MINGW to where "
                      "it is.")
endif()

# Set whichever machine this is, as libopus's CMake only takes the name in lower case: with
# Windows' own ARM64 it quietly builds without NEON. Setting these alone would count as a
# cross-compile, so say which it is. A native build has to stay able to run what it builds.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR ${KITHSONG_WINDOWS_CPU})
if(CMAKE_HOST_WIN32 AND (_host STREQUAL KITHSONG_WINDOWS_CPU OR
   (_host STREQUAL "x86_64" AND KITHSONG_WINDOWS_CPU STREQUAL "AMD64")))
  set(CMAKE_CROSSCOMPILING FALSE)
else()
  set(CMAKE_CROSSCOMPILING TRUE)
endif()
if(NOT CMAKE_HOST_WIN32)
  # Windows' headers are found whatever the case of their names there, and some sources rely
  # on it: mingw-case has the ones they name otherwise, each including the real one.
  set(CMAKE_C_STANDARD_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/mingw-case")
  find_program(KITHSONG_WINE NAMES wine wine64)
  if(KITHSONG_WINE)
    set(CMAKE_CROSSCOMPILING_EMULATOR "${KITHSONG_WINE}")
  endif()
endif()

set(CMAKE_C_COMPILER "${_root}/bin/${KITHSONG_TRIPLE}-clang${_exe}")
set(CMAKE_CXX_COMPILER "${_root}/bin/${KITHSONG_TRIPLE}-clang++${_exe}")
set(CMAKE_RC_COMPILER "${_root}/bin/${KITHSONG_TRIPLE}-windres${_exe}")

# Every executable, tests too: with the toolchain off PATH nothing would find libc++.dll or
# libunwind.dll. The UCRT is the only C runtime left to load, and that's part of Windows.
# Threads get the same stack as the main one, and lld's default of 1 MB is half of what
# GNU ld gave them.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -Wl,--stack,2097152")

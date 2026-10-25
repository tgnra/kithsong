# Third-party dependencies. None are committed: all are fetched at configure time, pinned by
# URL + SHA-256. This directory only holds our build glue for them.
# See LICENSES.md at the repo root for the license of each.

include(FetchContent)

# --- Single-file libraries ----------------------------------------------------

# Download only; the targets below are ours.
FetchContent_Declare(sqlite
  URL https://www.sqlite.org/2024/sqlite-amalgamation-3470200.zip
  URL_HASH SHA256=aa73d8748095808471deaa8e6f34aa700e37f2f787f4425744f53fdd15a89c40
  DOWNLOAD_EXTRACT_TIMESTAMP ON
  SOURCE_SUBDIR _none_)
FetchContent_Declare(yyjson
  URL https://github.com/ibireme/yyjson/archive/refs/tags/0.10.0.tar.gz
  URL_HASH SHA256=0d901cb2c45c5586e3f3a4245e58c2252d6b24bf4b402723f6179523d389b165
  DOWNLOAD_EXTRACT_TIMESTAMP ON
  SOURCE_SUBDIR _none_)
FetchContent_Declare(miniaudio
  URL https://github.com/mackron/miniaudio/archive/refs/tags/0.11.21.tar.gz
  URL_HASH SHA256=6afb5c231613d2fab4f1c668b7243ff9a7d6d78a7f5a2692c133f026fe508506
  DOWNLOAD_EXTRACT_TIMESTAMP ON
  SOURCE_SUBDIR _none_)
# dr_libs has no release tags; pinned to a commit.
FetchContent_Declare(dr_libs
  URL https://github.com/mackron/dr_libs/archive/ea33eedde32b3a1baa92bb78fc98c4711f890623.tar.gz
  URL_HASH SHA256=40bec21c1560426978aaf60fad8d5e4b7929caf569deb82f05e77b8c9d10b8fb
  DOWNLOAD_EXTRACT_TIMESTAMP ON
  SOURCE_SUBDIR _none_)
FetchContent_Declare(mdns
  URL https://github.com/mjansson/mdns/archive/refs/tags/1.4.3.tar.gz
  URL_HASH SHA256=be1fd8e35599cb7de179decbd0633c121d11a2dcb9cc193ff5c590bd0d480483
  DOWNLOAD_EXTRACT_TIMESTAMP ON
  SOURCE_SUBDIR _none_)
# Mozilla CA bundle, a dated snapshot from curl.se/ca. Refresh by bumping the date and hash.
FetchContent_Declare(cacert
  URL https://curl.se/ca/cacert-2026-10-20.pem
  URL_HASH SHA256=796f1389bfea13c0ed78f663b765baaff54a508e5483786bd6ece73c5e3e3cbe
  DOWNLOAD_NO_EXTRACT ON
  SOURCE_SUBDIR _none_)
FetchContent_MakeAvailable(sqlite yyjson miniaudio dr_libs mdns cacert)

add_library(mc_sqlite3 STATIC "${sqlite_SOURCE_DIR}/sqlite3.c")
target_include_directories(mc_sqlite3 SYSTEM PUBLIC "${sqlite_SOURCE_DIR}")
target_compile_definitions(mc_sqlite3 PRIVATE
  SQLITE_DQS=0
  SQLITE_THREADSAFE=1
  SQLITE_DEFAULT_MEMSTATUS=0
  SQLITE_DEFAULT_WAL_SYNCHRONOUS=1
  SQLITE_LIKE_DOESNT_MATCH_BLOBS
  SQLITE_OMIT_DEPRECATED
  SQLITE_OMIT_LOAD_EXTENSION
  SQLITE_OMIT_SHARED_CACHE
  SQLITE_ENABLE_FTS5)
if(UNIX)
  find_package(Threads REQUIRED)
  target_link_libraries(mc_sqlite3 PUBLIC Threads::Threads m)
endif()

add_library(mc_yyjson STATIC "${yyjson_SOURCE_DIR}/src/yyjson.c")
target_include_directories(mc_yyjson SYSTEM PUBLIC "${yyjson_SOURCE_DIR}/src")

# Audio output and decoding for core/src/playback. miniaudio only does output and format
# conversion; dr_libs decode. The definitions are public, so every includer sees the same
# configuration.
add_library(mc_miniaudio STATIC miniaudio/miniaudio.c)
target_include_directories(mc_miniaudio SYSTEM PUBLIC "${miniaudio_SOURCE_DIR}")
target_compile_definitions(mc_miniaudio PUBLIC
  MA_NO_DECODING MA_NO_ENCODING MA_NO_GENERATION MA_NO_RESOURCE_MANAGER MA_NO_ENGINE
  MA_NO_NODE_GRAPH MA_ENABLE_ONLY_SPECIFIC_BACKENDS)
if(WIN32)
  target_compile_definitions(mc_miniaudio PUBLIC MA_ENABLE_WASAPI)
elseif(ANDROID)
  target_compile_definitions(mc_miniaudio PUBLIC MA_ENABLE_AAUDIO MA_ENABLE_OPENSL)
  target_link_libraries(mc_miniaudio PUBLIC dl m)
else()
  # PipeWire serves both through its compatibility layers.
  target_compile_definitions(mc_miniaudio PUBLIC MA_ENABLE_PULSEAUDIO MA_ENABLE_ALSA)
  find_package(Threads REQUIRED)
  target_link_libraries(mc_miniaudio PUBLIC Threads::Threads dl m)
endif()

add_library(mc_dr_libs STATIC dr_libs/dr_libs.c)
target_include_directories(mc_dr_libs SYSTEM PUBLIC "${dr_libs_SOURCE_DIR}")
if(UNIX)
  target_link_libraries(mc_dr_libs PUBLIC m)
endif()

# mDNS/DNS-SD for core/src/discovery. Header-only: every function is static inline. On
# Windows it needs ws2_32, which core links.
add_library(mc_mdns INTERFACE)
target_include_directories(mc_mdns SYSTEM INTERFACE "${mdns_SOURCE_DIR}")

# The CA bundle is embedded and passed to curl with CURLOPT_CAINFO_BLOB, so HTTPS never
# depends on the OS trust store.
set(_cacert_pem "${cacert_SOURCE_DIR}/cacert-2026-10-20.pem")
set(_cacert_c "${CMAKE_CURRENT_BINARY_DIR}/cacert_embed.c")
add_custom_command(
  OUTPUT "${_cacert_c}"
  COMMAND "${CMAKE_COMMAND}"
    -DINPUT=${_cacert_pem}
    -DOUTPUT=${_cacert_c}
    -DSYMBOL=mc_cacert_pem
    -P "${PROJECT_SOURCE_DIR}/cmake/EmbedFile.cmake"
  DEPENDS "${_cacert_pem}" "${PROJECT_SOURCE_DIR}/cmake/EmbedFile.cmake"
  COMMENT "Embedding CA bundle")
add_library(mc_cacert STATIC "${_cacert_c}")

# --- Fetched ------------------------------------------------------------------

# TagLib's release tarball does not include its utfcpp submodule.
FetchContent_Declare(utfcpp
  URL https://github.com/nemtrif/utfcpp/archive/refs/tags/v4.0.6.tar.gz
  URL_HASH SHA256=6920a6a5d6a04b9a89b2a89af7132f8acefd46e0c2a7b190350539e9213816c0
  DOWNLOAD_EXTRACT_TIMESTAMP ON
  SOURCE_SUBDIR _none_)  # header-only: download, but don't add_subdirectory
FetchContent_MakeAvailable(utfcpp)
set(utf8cpp_INCLUDE_DIR "${utfcpp_SOURCE_DIR}/source" CACHE PATH "" FORCE)

set(BUILD_BINDINGS OFF CACHE BOOL "" FORCE)  # we use our own C shim, see core/src/tagger/tag_io.cpp
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WITH_ZLIB OFF CACHE BOOL "" FORCE)
FetchContent_Declare(taglib
  URL https://taglib.org/releases/taglib-2.0.2.tar.gz
  URL_HASH SHA256=0de288d7fe34ba133199fd8512f19cc1100196826eafcb67a33b224ec3a59737
  DOWNLOAD_EXTRACT_TIMESTAMP ON
  EXCLUDE_FROM_ALL SYSTEM)

set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(DISABLE_PACKAGE_CONFIG_AND_INSTALL ON CACHE BOOL "" FORCE)
FetchContent_Declare(mbedtls
  URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.2/mbedtls-3.6.2.tar.bz2
  URL_HASH SHA256=8b54fb9bcf4d5a7078028e0520acddefb7900b3e66fec7f7175bb5b7d85ccdca
  DOWNLOAD_EXTRACT_TIMESTAMP ON
  EXCLUDE_FROM_ALL SYSTEM)

FetchContent_MakeAvailable(taglib mbedtls)

# TagLib only exposes install-time include dirs; add the build-tree ones.
target_include_directories(tag SYSTEM INTERFACE
  "$<BUILD_INTERFACE:${taglib_SOURCE_DIR}/taglib>"
  "$<BUILD_INTERFACE:${taglib_SOURCE_DIR}/taglib/toolkit>"
  "$<BUILD_INTERFACE:${taglib_SOURCE_DIR}/taglib/mpeg>"
  "$<BUILD_INTERFACE:${taglib_SOURCE_DIR}/taglib/mpeg/id3v2>"
  "$<BUILD_INTERFACE:${taglib_BINARY_DIR}/taglib>"
  "$<BUILD_INTERFACE:${taglib_BINARY_DIR}>")
target_compile_definitions(tag INTERFACE TAGLIB_STATIC)

# curl: HTTP(S) only, mbedTLS backend, no optional system dependencies.
set(MBEDTLS_INCLUDE_DIR "${mbedtls_SOURCE_DIR}/include" CACHE PATH "" FORCE)
set(MBEDTLS_LIBRARY mbedtls CACHE STRING "" FORCE)
set(MBEDX509_LIBRARY mbedx509 CACHE STRING "" FORCE)
set(MBEDCRYPTO_LIBRARY mbedcrypto CACHE STRING "" FORCE)
foreach(_opt
    CURL_USE_MBEDTLS HTTP_ONLY BUILD_STATIC_LIBS CURL_DISABLE_INSTALL
    CURL_DISABLE_WEBSOCKETS CURL_DISABLE_ALTSVC CURL_DISABLE_NETRC)
  set(${_opt} ON CACHE BOOL "" FORCE)
endforeach()
foreach(_opt
    BUILD_CURL_EXE CURL_USE_OPENSSL CURL_USE_SCHANNEL CURL_USE_LIBPSL CURL_USE_LIBSSH2
    USE_LIBIDN2 USE_NGHTTP2 CURL_ZLIB CURL_BROTLI CURL_ZSTD CURL_USE_PKGCONFIG
    CURL_ENABLE_EXPORT_TARGET BUILD_LIBCURL_DOCS BUILD_MISC_DOCS ENABLE_CURL_MANUAL
    PICKY_COMPILER BUILD_TESTING)
  set(${_opt} OFF CACHE BOOL "" FORCE)
endforeach()
set(CURL_CA_BUNDLE "none" CACHE STRING "" FORCE)  # we pass the embedded bundle per request
set(CURL_CA_PATH "none" CACHE STRING "" FORCE)
FetchContent_Declare(curl
  URL https://curl.se/download/curl-8.11.1.tar.xz
  URL_HASH SHA256=c7ca7db48b0909743eaef34250da02c19bc61d4f1dcedd6603f109409536ab56
  DOWNLOAD_EXTRACT_TIMESTAMP ON
  EXCLUDE_FROM_ALL SYSTEM)
FetchContent_MakeAvailable(curl)

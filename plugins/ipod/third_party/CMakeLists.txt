# The plugin's encoders, fetched at configure time and pinned by URL + SHA-256 like
# third_party/CMakeLists.txt. Built static into the plugin only, never into the app.

include(FetchContent)

# Apple's ALAC reference codec (Apache-2.0). No release tags; pinned to a commit.
FetchContent_Declare(alac
  URL https://github.com/macosforge/alac/archive/c38887c5c5e64a4b31108733bd79ca9b2496d987.tar.gz
  URL_HASH SHA256=98635ece42fb1c3fceb75eaa4b5164d866e09f0195b3e7ec4085f1123c5e272f
  DOWNLOAD_EXTRACT_TIMESTAMP ON
  SOURCE_SUBDIR _none_)
# LAME 3.100 (LGPL-2.0-or-later). Its autotools build isn't used; the targets below are ours.
FetchContent_Declare(lame
  URL https://downloads.sourceforge.net/project/lame/lame/3.100/lame-3.100.tar.gz
  URL_HASH SHA256=ddfe36cab873794038ae2c1210557ad34857a4b6bdc515785d1da9e175b1da1e
  DOWNLOAD_EXTRACT_TIMESTAMP ON
  SOURCE_SUBDIR _none_)
FetchContent_MakeAvailable(alac lame)

# The encoder and, for the tests' bit-exact round trip, the decoder.
set(_alac "${alac_SOURCE_DIR}/codec")
add_library(ipod_alac STATIC
  "${_alac}/ALACEncoder.cpp" "${_alac}/ALACDecoder.cpp" "${_alac}/ALACBitUtilities.c"
  "${_alac}/EndianPortable.c" "${_alac}/ag_enc.c" "${_alac}/ag_dec.c" "${_alac}/dp_enc.c"
  "${_alac}/dp_dec.c" "${_alac}/matrix_enc.c" "${_alac}/matrix_dec.c")
target_include_directories(ipod_alac SYSTEM PUBLIC "${_alac}")
if(NOT MSVC)
  target_compile_options(ipod_alac PRIVATE -w)
endif()

# libmp3lame without the mpglib decoder, the frontend or the SIMD paths (vector/, i386/),
# so it builds the same everywhere. lame_config.h stands in for configure's config.h.
set(_lame "${lame_SOURCE_DIR}/libmp3lame")
add_library(ipod_lame STATIC
  "${_lame}/VbrTag.c" "${_lame}/bitstream.c" "${_lame}/encoder.c" "${_lame}/fft.c"
  "${_lame}/gain_analysis.c" "${_lame}/id3tag.c" "${_lame}/lame.c" "${_lame}/newmdct.c"
  "${_lame}/presets.c" "${_lame}/psymodel.c" "${_lame}/quantize.c" "${_lame}/quantize_pvt.c"
  "${_lame}/reservoir.c" "${_lame}/set_get.c" "${_lame}/tables.c" "${_lame}/takehiro.c"
  "${_lame}/util.c" "${_lame}/vbrquantize.c" "${_lame}/version.c")
target_include_directories(ipod_lame SYSTEM PUBLIC "${lame_SOURCE_DIR}/include")
target_include_directories(ipod_lame PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/lame" "${_lame}")
target_compile_definitions(ipod_lame PRIVATE HAVE_CONFIG_H)
if(NOT MSVC)
  target_compile_options(ipod_lame PRIVATE -w)
endif()
if(UNIX)
  target_link_libraries(ipod_lame PUBLIC m)
endif()

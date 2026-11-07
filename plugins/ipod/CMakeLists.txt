# iPod sync plugin (docs/plugins.md, "The iPod plugin").
#
# kithipod is the plugin's self-contained library: SysInfo, the iTunesDB reader and writer,
# Play Counts and On-The-Go, file placement, artwork, kithsong.json, and the ALAC and MP3
# encoders. It doesn't link kithcore and knows nothing of the plugin ABI; the plugin's
# shared library, kithsong-ipod (plugin/), wraps it. Its encoders (Apple's ALAC, LAME) are
# built into the plugin only, never the app; see the Plugins section of LICENSES.md.

add_subdirectory(third_party)

add_library(kithipod STATIC
  src/util.c
  src/sysinfo.c
  src/itunesdb.c
  src/playcounts.c
  src/device.c
  src/kithjson.c
  src/artwork.c
  src/image.c
  src/m4a.c
  src/alac_shim.cpp
  src/id3.c
  src/encoder.c)
target_include_directories(kithipod PUBLIC include PRIVATE src)
target_link_libraries(kithipod PRIVATE ipod_alac ipod_lame mc_yyjson mc_stb)
if(WIN32)
  target_link_libraries(kithipod PRIVATE bcrypt)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(kithipod PRIVATE Threads::Threads m)
endif()
kithsong_warnings(kithipod)

# The plugin: kithsong-ipod.dll / kithsong-ipod.so, exporting kith_plugin_init alone. It
# includes kithcore/plugin.h and nothing else of core's, and doesn't link kithcore.
add_library(kithsong-ipod MODULE plugin/plugin.c plugin/encode.c)
target_include_directories(kithsong-ipod PRIVATE "${PROJECT_SOURCE_DIR}/core/include" src)
target_compile_definitions(kithsong-ipod PRIVATE KITH_IPOD_VERSION="${PROJECT_VERSION}")
target_link_libraries(kithsong-ipod PRIVATE kithipod mbedcrypto)
set_target_properties(kithsong-ipod PROPERTIES
  PREFIX ""
  C_VISIBILITY_PRESET hidden
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  LIBRARY_OUTPUT_DIRECTORY "${KITH_PLUGIN_OUTPUT_DIR}"
  RUNTIME_OUTPUT_DIRECTORY "${KITH_PLUGIN_OUTPUT_DIR}")
if(WIN32 AND NOT MSVC)
  # libc++ and libunwind linked in, as the app's are: nothing to find beside the plugin.
  target_link_options(kithsong-ipod PRIVATE -static)
elseif(UNIX)
  # Only kith_plugin_init is exported, not the static libraries' symbols.
  target_link_options(kithsong-ipod PRIVATE -Wl,--exclude-libs,ALL -Wl,--no-undefined)
endif()
kithsong_warnings(kithsong-ipod)

# LAME's licence goes with the plugin (LICENSES.md), and Apple's ALAC licence with it.
include(FetchContent)
FetchContent_GetProperties(lame)
FetchContent_GetProperties(alac)
set(_licences
  "${lame_SOURCE_DIR}/COPYING|kithsong-ipod.LAME-COPYING.txt"
  "${alac_SOURCE_DIR}/LICENSE|kithsong-ipod.ALAC-LICENSE.txt")
foreach(_l IN LISTS _licences)
  string(REPLACE "|" ";" _pair "${_l}")
  list(GET _pair 0 _from)
  list(GET _pair 1 _to)
  if(EXISTS "${_from}")
    add_custom_command(TARGET kithsong-ipod POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_from}" "${KITH_PLUGIN_OUTPUT_DIR}/${_to}"
      VERBATIM)
    install(FILES "${_from}" DESTINATION "${KITH_PLUGIN_INSTALL_DIR}" RENAME "${_to}"
      COMPONENT kithsong-ipod EXCLUDE_FROM_ALL)
  else()
    message(WARNING "iPod plugin: no ${_from} to ship as ${_to}")
  endif()
endforeach()
install(TARGETS kithsong-ipod
  LIBRARY DESTINATION "${KITH_PLUGIN_INSTALL_DIR}"
  RUNTIME DESTINATION "${KITH_PLUGIN_INSTALL_DIR}"
  COMPONENT kithsong-ipod EXCLUDE_FROM_ALL)

if(KITHSONG_BUILD_TESTS)
  add_subdirectory(tests)
endif()

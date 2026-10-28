# The license texts LICENSES.md asks every distributed package to carry: LICENSES.md itself
# plus each fetched dependency's own license file, renamed <dependency>-<file>.txt.
# SQLite (public domain) and the CA bundle (MPL-2.0, named in LICENSES.md) ship none.
include_guard(GLOBAL)
include(FetchContent)
set(_kithsong_licenses_md "${CMAKE_CURRENT_LIST_DIR}/../LICENSES.md")

# <dependency>:<file>:<name>:<licence>, the last two as the Android app's licence list shows
# them (LICENSES.md's names).
set(_kithsong_license_files
    "yyjson:LICENSE:yyjson:MIT"
    "miniaudio:LICENSE:miniaudio:Public domain / MIT-0"
    "dr_libs:LICENSE:dr_libs:Public domain / MIT-0"
    "stb:LICENSE:stb:Public domain / MIT"
    "mdns:LICENSE:mdns:Public domain (Unlicense)"
    "taglib:COPYING.MPL:TagLib:MPL-1.1"
    "utfcpp:LICENSE:utfcpp:BSL-1.0"
    "curl:COPYING:curl:curl"
    "mbedtls:LICENSE:Mbed TLS:Apache-2.0"
    "ogg:COPYING:libogg:BSD-3-Clause"
    "opus:COPYING:libopus:BSD-3-Clause"
    "opusenc:COPYING:libopusenc:BSD-3-Clause"
    "opusfile:COPYING:opusfile:BSD-3-Clause")

# Calls <callback>(source renamed name licence) for each license file, after checking it exists.
macro(_kithsong_each_license callback)
  foreach(_entry IN LISTS _kithsong_license_files)
    string(REPLACE ":" ";" _parts "${_entry}")
    list(GET _parts 0 _dep)
    list(GET _parts 1 _file)
    list(GET _parts 2 _name)
    list(GET _parts 3 _license)
    FetchContent_GetProperties(${_dep} SOURCE_DIR _src)
    if(NOT EXISTS "${_src}/${_file}")
      message(FATAL_ERROR "No ${_file} in ${_dep}'s sources (${_src}); update cmake/Licenses.cmake.")
    endif()
    cmake_language(CALL ${callback} "${_src}/${_file}" "${_dep}-${_file}.txt" "${_name}" "${_license}")
  endforeach()
endmacro()

# Installs them into <destination> (the desktop packages).
function(kithsong_install_licenses destination)
  install(FILES "${_kithsong_licenses_md}" DESTINATION "${destination}" COMPONENT kithsong)
  function(_kithsong_install_license src renamed name license)
    install(FILES "${src}" DESTINATION "${destination}" RENAME "${renamed}" COMPONENT kithsong)
  endfunction()
  _kithsong_each_license(_kithsong_install_license)
endfunction()

# Copies them into <directory> at configure time, with an index.tsv of "<name>\t<file>\t<licence>" lines
# (the Android apps' assets; see android/native/build.gradle.kts).
function(kithsong_copy_licenses directory)
  file(MAKE_DIRECTORY "${directory}")
  file(COPY_FILE "${_kithsong_licenses_md}" "${directory}/LICENSES.md" ONLY_IF_DIFFERENT)
  set_property(GLOBAL PROPERTY _kithsong_license_index "")
  function(_kithsong_copy_license src renamed name license)
    file(COPY_FILE "${src}" "${directory}/${renamed}" ONLY_IF_DIFFERENT)
    set_property(GLOBAL APPEND_STRING PROPERTY _kithsong_license_index "${name}\t${renamed}\t${license}\n")
  endfunction()
  _kithsong_each_license(_kithsong_copy_license)
  get_property(_index GLOBAL PROPERTY _kithsong_license_index)
  file(CONFIGURE OUTPUT "${directory}/index.tsv" CONTENT "${_index}" @ONLY NEWLINE_STYLE UNIX)
endfunction()

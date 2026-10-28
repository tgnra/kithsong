# Installs the license texts LICENSES.md asks every distributed package to carry: LICENSES.md
# itself plus each fetched dependency's own license file, renamed <dependency>-<file>.txt.
# SQLite (public domain) and the CA bundle (MPL-2.0, named in LICENSES.md) ship none.
include_guard(GLOBAL)
include(FetchContent)

function(kithsong_install_licenses destination)
  install(FILES "${PROJECT_SOURCE_DIR}/LICENSES.md" DESTINATION "${destination}" COMPONENT kithsong)
  foreach(_entry
      yyjson:LICENSE miniaudio:LICENSE dr_libs:LICENSE mdns:LICENSE taglib:COPYING.MPL
      utfcpp:LICENSE curl:COPYING mbedtls:LICENSE ogg:COPYING opus:COPYING opusenc:COPYING
      opusfile:COPYING)
    string(REPLACE ":" ";" _parts "${_entry}")
    list(GET _parts 0 _dep)
    list(GET _parts 1 _file)
    FetchContent_GetProperties(${_dep} SOURCE_DIR _src)
    if(NOT EXISTS "${_src}/${_file}")
      message(FATAL_ERROR "No ${_file} in ${_dep}'s sources (${_src}); update cmake/Licenses.cmake.")
    endif()
    install(FILES "${_src}/${_file}" DESTINATION "${destination}" RENAME "${_dep}-${_file}.txt"
            COMPONENT kithsong)
  endforeach()
endfunction()

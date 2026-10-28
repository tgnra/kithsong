find_package(PkgConfig REQUIRED)
# 1.5 for AdwDialog (import and settings views); 1.4 for AdwToolbarView and AdwOverlaySplitView.
pkg_check_modules(ADW REQUIRED IMPORTED_TARGET libadwaita-1>=1.5 gtk4>=4.12)

add_executable(kithsong
  src/main.c
  src/album_grid.c
  src/cover.c
  src/mpris.c
  src/now_playing.c
  src/playback.c
  src/track_list.c
  src/import_view.c
  src/settings_view.c
  src/move_library.c
  src/add_device.c
  src/devices.c
  src/recents.c
  src/search_view.c
  src/device_view.c
  src/pair_dialog.c
  src/sync_bridge.c
  src/add_to_playlist.c
  src/playlist_dialogs.c)
target_link_libraries(kithsong PRIVATE musiccore PkgConfig::ADW m)
kithsong_warnings(kithsong)
# GLib's headers trip -Wconversion / -Wpedantic; keep warnings strict for our code only.
target_compile_options(kithsong PRIVATE -Wno-pedantic)

# Installed layout, for the Flatpak (packaging/flatpak) or any other prefix. The kithsong
# component leaves out the fetched libraries' own install rules.
include(GNUInstallDirs)
include(Licenses)
set(_app_id app.kithsong.Kithsong)
string(TIMESTAMP KITHSONG_RELEASE_DATE "%Y-%m-%d" UTC)  # honours SOURCE_DATE_EPOCH

# The .desktop file's and the metainfo's text is strings/en.json's (linux.app_summary,
# app_description, app_keywords), plus a translated line for each strings/<lang>.json that
# has one. Editing a strings file reconfigures.

# The text of section.key in a strings file's JSON, or "" if it has none.
function(_kithsong_string json section key out)
  string(JSON _type ERROR_VARIABLE _err TYPE "${json}" ${section} ${key})
  set(_text "")
  if(_type STREQUAL "STRING")
    string(JSON _text GET "${json}" ${section} ${key})
  elseif(_type STREQUAL "OBJECT")
    string(JSON _text ERROR_VARIABLE _err GET "${json}" ${section} ${key} text)
    if(_err)
      set(_text "")
    endif()
  endif()
  set(${out} "${_text}" PARENT_SCOPE)
endfunction()

# text as a .desktop file's value (backslashes and newlines escaped) and as XML text.
function(_kithsong_escape text desktop_out xml_out)
  string(REPLACE "\\" "\\\\" _desktop "${text}")
  string(REPLACE "\n" "\\n" _desktop "${_desktop}")
  string(REPLACE "&" "&amp;" _xml "${text}")
  string(REPLACE "<" "&lt;" _xml "${_xml}")
  string(REPLACE ">" "&gt;" _xml "${_xml}")
  set(${desktop_out} "${_desktop}" PARENT_SCOPE)
  set(${xml_out} "${_xml}" PARENT_SCOPE)
endfunction()

file(GLOB _string_files CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/strings/*.json")
list(SORT _string_files)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_string_files})
# The translations, as whole lines for the templates.
set(KITHSONG_COMMENT_L10N "")
set(KITHSONG_KEYWORDS_L10N "")
set(KITHSONG_SUMMARY_L10N "")
set(KITHSONG_DESCRIPTION_L10N "")
foreach(_file IN LISTS _string_files)
  get_filename_component(_lang "${_file}" NAME_WE)
  string(REPLACE "-" "_" _locale "${_lang}")  # pt-BR -> pt_BR, as both files name locales
  file(READ "${_file}" _json)
  _kithsong_string("${_json}" linux app_summary _summary)
  _kithsong_string("${_json}" linux app_description _description)
  _kithsong_string("${_json}" linux app_keywords _keywords)
  _kithsong_escape("${_summary}" _summary_desktop _summary_xml)
  _kithsong_escape("${_description}" _description_desktop _description_xml)
  _kithsong_escape("${_keywords}" _keywords_desktop _keywords_xml)
  if(_lang STREQUAL "en")
    _kithsong_string("${_json}" shared app_name KITHSONG_APP_NAME)
    set(KITHSONG_COMMENT "${_summary_desktop}")
    set(KITHSONG_KEYWORDS "${_keywords_desktop}")
    set(KITHSONG_SUMMARY "${_summary_xml}")
    set(KITHSONG_DESCRIPTION "${_description_xml}")
    continue()
  endif()
  if(NOT _summary STREQUAL "")
    string(APPEND KITHSONG_COMMENT_L10N "Comment[${_locale}]=${_summary_desktop}\n")
    string(APPEND KITHSONG_SUMMARY_L10N "  <summary xml:lang=\"${_locale}\">${_summary_xml}</summary>\n")
  endif()
  if(NOT _keywords STREQUAL "")
    string(APPEND KITHSONG_KEYWORDS_L10N "Keywords[${_locale}]=${_keywords_desktop}\n")
  endif()
  if(NOT _description STREQUAL "")
    string(APPEND KITHSONG_DESCRIPTION_L10N "    <p xml:lang=\"${_locale}\">${_description_xml}</p>\n")
  endif()
endforeach()

configure_file(data/${_app_id}.desktop.in "${CMAKE_CURRENT_BINARY_DIR}/${_app_id}.desktop" @ONLY)
configure_file(data/${_app_id}.metainfo.xml.in "${CMAKE_CURRENT_BINARY_DIR}/${_app_id}.metainfo.xml" @ONLY)
install(TARGETS kithsong RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT kithsong)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${_app_id}.desktop"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/applications" COMPONENT kithsong)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${_app_id}.metainfo.xml"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/metainfo" COMPONENT kithsong)
install(FILES "${PROJECT_SOURCE_DIR}/packaging/icons/kithsong.svg"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps" RENAME ${_app_id}.svg
  COMPONENT kithsong)
foreach(_size 128 256 512)
  install(FILES "${PROJECT_SOURCE_DIR}/packaging/icons/kithsong-${_size}.png"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${_size}x${_size}/apps" RENAME ${_app_id}.png
    COMPONENT kithsong)
endforeach()
kithsong_install_licenses("${CMAKE_INSTALL_DATADIR}/licenses/${_app_id}")

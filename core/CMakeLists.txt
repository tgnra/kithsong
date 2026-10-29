set(KITHSONG_CONTACT "" CACHE STRING
  "Contact URL or email sent in the MusicBrainz/AcoustID User-Agent (required by their terms)")
if(KITHSONG_CONTACT STREQUAL "")
  message(WARNING "KITHSONG_CONTACT is not set. MusicBrainz may block requests without contact "
                  "info; configure with -DKITHSONG_CONTACT=<url or email>.")
  set(_contact "no contact configured")
else()
  set(_contact "${KITHSONG_CONTACT}")
endif()
block(SCOPE_FOR VARIABLES)
  set(KITHSONG_CONTACT "${_contact}")
  configure_file(src/config.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/config.h" @ONLY)
endblock()
include("${PROJECT_SOURCE_DIR}/cmake/LastfmKey.cmake")
kithsong_lastfm_key_header("${CMAKE_CURRENT_BINARY_DIR}/generated/lastfm_key.h")

# Embeds a .sql file as a NUL-terminated C string named symbol; the generated source's path
# is returned in out_var.
function(musiccore_embed_sql out_var src symbol)
  get_filename_component(_name "${src}" NAME_WE)
  set(_c "${CMAKE_CURRENT_BINARY_DIR}/generated/${_name}.c")
  add_custom_command(
    OUTPUT "${_c}"
    COMMAND "${CMAKE_COMMAND}"
      -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/${src}
      -DOUTPUT=${_c}
      -DSYMBOL=${symbol}
      -P "${PROJECT_SOURCE_DIR}/cmake/EmbedFile.cmake"
    DEPENDS "${src}" "${PROJECT_SOURCE_DIR}/cmake/EmbedFile.cmake"
    COMMENT "Embedding ${_name}.sql")
  set(${out_var} "${_c}" PARENT_SCOPE)
endfunction()

musiccore_embed_sql(_schema_c src/library/schema_v1.sql mc_schema_v1_sql)
musiccore_embed_sql(_sync_schema_c src/sync/sync_schema_v1.sql mc_sync_schema_v1_sql)

# UI strings: strings/*.json becomes enum mc_str_id (mc_strings_gen.h, public, for the
# frontends) and a table per language (mc_strings_gen.c). See strings/README.md.
set(_strings_dir "${PROJECT_SOURCE_DIR}/strings")
set(_strings_out "${CMAKE_CURRENT_BINARY_DIR}/strings")
file(GLOB _strings_json CONFIGURE_DEPENDS "${_strings_dir}/*.json")
add_custom_command(
  OUTPUT "${_strings_out}/mc_strings_gen.h" "${_strings_out}/mc_strings_gen.c"
  COMMAND "${CMAKE_COMMAND}"
    -DIN=${_strings_dir}/en.json
    -DOUT_DIR=${_strings_out}
    -DMODE=c
    -P "${PROJECT_SOURCE_DIR}/cmake/gen_strings.cmake"
  DEPENDS ${_strings_json} "${PROJECT_SOURCE_DIR}/cmake/gen_strings.cmake"
  COMMENT "Generating UI strings")

add_library(musiccore STATIC
  src/core.c
  src/settings.c
  src/util/platform.c
  src/util/uid.c
  src/util/hlc.c
  src/util/changelog.c
  src/util/fracindex.c
  src/util/i18n.c
  src/util/text.c
  "${_strings_out}/mc_strings_gen.h"
  "${_strings_out}/mc_strings_gen.c"
  src/library/library.c
  src/library/library_move.c
  src/library/delete.c
  "${_schema_c}"
  src/playlist/playlist.c
  src/playlist/playlist_art.c
  src/smart/smartlist.c
  src/search/search.c
  src/playback/decoder.c
  src/playback/player.c
  src/playback/shuffle.c
  src/tagger/tag_io.cpp
  src/tagger/musicbrainz.c
  src/tagger/match.c
  src/tagger/search_keys.c
  src/tagger/coverart.c
  src/tagger/import_db.c
  src/tagger/dupes.c
  src/tagger/organize.c
  src/tagger/unzip.c
  src/tagger/tagger.c
  src/net/http.c
  src/scrobble/lastfm.c
  src/scrobble/lastfm_key.c
  src/scrobble/scrobbler.c

  src/net/socket.c
  src/transcode/transcode.c
  src/discovery/discovery.c
  src/sync/sync.c
  src/sync/sync_db.c
  src/sync/frame.c
  src/sync/noise.c
  src/sync/channel.c
  src/sync/pair.c
  src/sync/pairing.c
  src/sync/profile.c
  src/sync/here.c
  src/sync/archive.c
  src/sync/merge.c
  src/sync/rows.c
  src/sync/session.c
  src/sync/plan.c
  src/sync/transfer.c
  src/sync/reach.c
  "${_sync_schema_c}")
add_library(musiccore::musiccore ALIAS musiccore)

target_include_directories(musiccore
  PUBLIC include "${_strings_out}"
  PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(musiccore PRIVATE
  mc_sqlite3 mc_yyjson mc_cacert mc_miniaudio mc_dr_libs mc_stb mc_miniz mc_mdns mc_opusenc mc_opusfile tag
  CURL::libcurl mbedcrypto)
if(WIN32)
  target_compile_definitions(musiccore PRIVATE _WIN32_WINNT=0x0A00)
  # ws2_32 for sockets (sync and discovery), iphlpapi for discovery's interface list,
  # shell32 for the Recycle Bin (mc_trash).
  target_link_libraries(musiccore PRIVATE bcrypt ws2_32 iphlpapi shell32)
endif()
kithsong_warnings(musiccore)

if(KITHSONG_BUILD_TESTS)
  add_subdirectory(tests)
endif()

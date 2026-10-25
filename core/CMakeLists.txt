set(MUSICAPP_CONTACT "" CACHE STRING
  "Contact URL or email sent in the MusicBrainz/AcoustID User-Agent (required by their terms)")
if(MUSICAPP_CONTACT STREQUAL "")
  message(WARNING "MUSICAPP_CONTACT is not set. MusicBrainz may block requests without contact "
                  "info; configure with -DMUSICAPP_CONTACT=<url or email>.")
  set(_contact "no contact configured")
else()
  set(_contact "${MUSICAPP_CONTACT}")
endif()
block(SCOPE_FOR VARIABLES)
  set(MUSICAPP_CONTACT "${_contact}")
  configure_file(src/config.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/config.h" @ONLY)
endblock()

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

add_library(musiccore STATIC
  src/core.c
  src/settings.c
  src/util/platform.c
  src/util/uid.c
  src/util/hlc.c
  src/util/changelog.c
  src/library/library.c
  "${_schema_c}"
  src/playlist/playlist.c
  src/playback/player.c
  src/tagger/tag_io.cpp
  src/tagger/musicbrainz.c
  src/tagger/match.c
  src/tagger/coverart.c
  src/tagger/import_db.c
  src/tagger/organize.c
  src/tagger/tagger.c
  src/net/http.c
  src/transcode/transcode.c
  src/discovery/discovery.c
  src/sync/sync.c
  src/sync/sync_db.c
  "${_sync_schema_c}")
add_library(musiccore::musiccore ALIAS musiccore)

target_include_directories(musiccore
  PUBLIC include
  PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(musiccore PRIVATE
  mc_sqlite3 mc_yyjson mc_cacert mc_miniaudio mc_dr_libs tag CURL::libcurl mbedcrypto)
if(WIN32)
  target_compile_definitions(musiccore PRIVATE _WIN32_WINNT=0x0A00)
  target_link_libraries(musiccore PRIVATE bcrypt)
endif()
musicapp_warnings(musiccore)

if(MUSICAPP_BUILD_TESTS)
  add_subdirectory(tests)
endif()

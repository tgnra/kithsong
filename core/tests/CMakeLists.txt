# Unit tests. Each test_*.c is its own executable; tests that need scratch files get the
# test binary directory as argv[1].
function(kithsong_test name)
  add_executable(${name} ${name}.c)
  target_link_libraries(${name} PRIVATE musiccore mc_sqlite3)
  # White-box tests may include core's private headers.
  target_include_directories(${name} PRIVATE "${PROJECT_SOURCE_DIR}/core/src")
  kithsong_warnings(${name})
  add_test(NAME ${name} COMMAND ${name} "${CMAKE_CURRENT_BINARY_DIR}")
  # A safety net for a test that hangs, e.g. on a network call; slow ones raise it.
  set_tests_properties(${name} PROPERTIES TIMEOUT 120)
endfunction()

kithsong_test(test_util)
kithsong_test(test_library)
kithsong_test(test_library_move)
kithsong_test(test_smart)
kithsong_test(test_search)
kithsong_test(test_playlist_art)
target_link_libraries(test_playlist_art PRIVATE mc_stb mbedcrypto)  # makes covers, reads grids back
kithsong_test(test_sync_db)
target_link_libraries(test_sync_db PRIVATE mbedcrypto)  # checks the key pair independently
kithsong_test(test_noise)
target_link_libraries(test_noise PRIVATE mbedcrypto)  # known-answer tests of the primitives
kithsong_test(test_channel)
kithsong_test(test_rows)
kithsong_test(test_merge)
kithsong_test(test_session)
set_tests_properties(test_session PROPERTIES TIMEOUT 300)
kithsong_test(test_sync_profile)
set_tests_properties(test_sync_profile PROPERTIES TIMEOUT 300)
kithsong_test(test_sync_name)
set_tests_properties(test_sync_name PROPERTIES TIMEOUT 300)
kithsong_test(test_sync_timeout)
kithsong_test(test_sync_rate)
kithsong_test(test_sync_reach)
kithsong_test(test_sync_confirm)
set_tests_properties(test_sync_confirm PROPERTIES TIMEOUT 300)
kithsong_test(test_sync_files)
target_link_libraries(test_sync_files PRIVATE mbedcrypto)  # hashes the files it compares
set_tests_properties(test_sync_files PROPERTIES TIMEOUT 300)
kithsong_test(test_tags)
kithsong_test(test_match)
kithsong_test(test_import_db)
kithsong_test(test_import_offline)
kithsong_test(test_dupes)
kithsong_test(test_delete)
kithsong_test(test_organize)
kithsong_test(test_transcode)
target_link_libraries(test_transcode PRIVATE mc_opusfile)  # reads the output back directly
kithsong_test(test_scrobbler)
target_link_libraries(test_scrobbler PRIVATE mbedcrypto mc_yyjson)  # checks the signature independently
target_include_directories(test_scrobbler PRIVATE "${PROJECT_BINARY_DIR}/core/generated")  # config.h
kithsong_test(test_player)
kithsong_test(test_shuffle)

set_tests_properties(test_player PROPERTIES SKIP_RETURN_CODE 77)  # no audio device
kithsong_test(test_discovery)
target_link_libraries(test_discovery PRIVATE mc_mdns)  # sends packets as a fake peer
set_tests_properties(test_discovery PROPERTIES SKIP_RETURN_CODE 77)  # no multicast

# Hit musicbrainz.org; run by hand, not part of ctest.
add_executable(manual_mb_ping manual_mb_ping.c)
target_link_libraries(manual_mb_ping PRIVATE musiccore)
# Signs in and scrobbles for real; see the comment at its top.
add_executable(manual_scrobble manual_scrobble.c)
target_link_libraries(manual_scrobble PRIVATE musiccore)
target_include_directories(manual_scrobble PRIVATE "${PROJECT_SOURCE_DIR}/core/src")
add_executable(manual_import manual_import.c)

target_link_libraries(manual_import PRIVATE musiccore)
target_include_directories(manual_import PRIVATE "${PROJECT_SOURCE_DIR}/core/src")

# UI strings: the app's table, then language selection and fallback against a small table
# with translations (strings/ here), built with the same generator and lookup code.
kithsong_test(test_strings)
set(_strings_fixture "${CMAKE_CURRENT_BINARY_DIR}/strings_fixture")
file(GLOB _strings_fixture_json CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/strings/*.json")
add_custom_command(
  OUTPUT "${_strings_fixture}/mc_strings_gen.h" "${_strings_fixture}/mc_strings_gen.c"
  COMMAND "${CMAKE_COMMAND}"
    -DIN=${CMAKE_CURRENT_SOURCE_DIR}/strings/en.json
    -DOUT_DIR=${_strings_fixture}
    -DMODE=c
    -P "${PROJECT_SOURCE_DIR}/cmake/gen_strings.cmake"
  DEPENDS ${_strings_fixture_json} "${PROJECT_SOURCE_DIR}/cmake/gen_strings.cmake"
  COMMENT "Generating the test UI strings")
add_executable(test_strings_lang test_strings_lang.c "${PROJECT_SOURCE_DIR}/core/src/util/i18n.c"
  "${_strings_fixture}/mc_strings_gen.h" "${_strings_fixture}/mc_strings_gen.c")
target_include_directories(test_strings_lang PRIVATE
  "${_strings_fixture}" "${PROJECT_SOURCE_DIR}/core/include" "${PROJECT_SOURCE_DIR}/core/src")
kithsong_warnings(test_strings_lang)
add_test(NAME test_strings_lang COMMAND test_strings_lang)

# The generator refuses broken input: each strings_bad/<case>/en.json must fail with the
# message given (one word: CMake wraps long messages).
foreach(_case_msg IN ITEMS
    "dup_section|sections"
    "dup_key|appears"
    "placeholder|malformed"
    "plural_other|\"other\""
    "gap|gaps"
    "translation|isn't"
    "field|field")
  string(REPLACE "|" ";" _case_msg "${_case_msg}")
  list(GET _case_msg 0 _case)
  list(GET _case_msg 1 _msg)
  add_test(NAME gen_strings_rejects_${_case}
    COMMAND "${CMAKE_COMMAND}"
      -DIN=${CMAKE_CURRENT_SOURCE_DIR}/strings_bad/${_case}/en.json
      -DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/strings_bad/${_case}
      -DMODE=c
      -P "${PROJECT_SOURCE_DIR}/cmake/gen_strings.cmake")
  set_tests_properties(gen_strings_rejects_${_case} PROPERTIES
    PASS_REGULAR_EXPRESSION "${_msg}")
endforeach()

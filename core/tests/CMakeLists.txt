# Unit tests. Each test_*.c is its own executable; tests that need scratch files get the
# test binary directory as argv[1].
function(musicapp_test name)
  add_executable(${name} ${name}.c)
  target_link_libraries(${name} PRIVATE musiccore mc_sqlite3)
  # White-box tests may include core's private headers.
  target_include_directories(${name} PRIVATE "${PROJECT_SOURCE_DIR}/core/src")
  musicapp_warnings(${name})
  add_test(NAME ${name} COMMAND ${name} "${CMAKE_CURRENT_BINARY_DIR}")
  # A safety net for a test that hangs, e.g. on a network call; slow ones raise it.
  set_tests_properties(${name} PROPERTIES TIMEOUT 120)
endfunction()

musicapp_test(test_util)
musicapp_test(test_library)
musicapp_test(test_smart)
musicapp_test(test_sync_db)
target_link_libraries(test_sync_db PRIVATE mbedcrypto)  # checks the key pair independently
musicapp_test(test_noise)
target_link_libraries(test_noise PRIVATE mbedcrypto)  # known-answer tests of the primitives
musicapp_test(test_channel)
musicapp_test(test_rows)
musicapp_test(test_merge)
musicapp_test(test_session)
set_tests_properties(test_session PROPERTIES TIMEOUT 300)
musicapp_test(test_sync_profile)
set_tests_properties(test_sync_profile PROPERTIES TIMEOUT 300)
musicapp_test(test_sync_name)
set_tests_properties(test_sync_name PROPERTIES TIMEOUT 300)
musicapp_test(test_sync_timeout)
musicapp_test(test_tags)
musicapp_test(test_match)
musicapp_test(test_import_db)
musicapp_test(test_organize)
musicapp_test(test_transcode)
target_link_libraries(test_transcode PRIVATE mc_opusfile)  # reads the output back directly
musicapp_test(test_player)
set_tests_properties(test_player PROPERTIES SKIP_RETURN_CODE 77)  # no audio device
musicapp_test(test_discovery)
target_link_libraries(test_discovery PRIVATE mc_mdns)  # sends packets as a fake peer
set_tests_properties(test_discovery PROPERTIES SKIP_RETURN_CODE 77)  # no multicast

# Hit musicbrainz.org; run by hand, not part of ctest.
add_executable(manual_mb_ping manual_mb_ping.c)
target_link_libraries(manual_mb_ping PRIVATE musiccore)
add_executable(manual_import manual_import.c)
target_link_libraries(manual_import PRIVATE musiccore)
target_include_directories(manual_import PRIVATE "${PROJECT_SOURCE_DIR}/core/src")

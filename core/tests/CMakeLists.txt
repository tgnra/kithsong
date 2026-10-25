# Unit tests. Each test_*.c is its own executable; tests that need scratch files get the
# test binary directory as argv[1].
function(musicapp_test name)
  add_executable(${name} ${name}.c)
  target_link_libraries(${name} PRIVATE musiccore mc_sqlite3)
  # White-box tests may include core's private headers.
  target_include_directories(${name} PRIVATE "${PROJECT_SOURCE_DIR}/core/src")
  musicapp_warnings(${name})
  add_test(NAME ${name} COMMAND ${name} "${CMAKE_CURRENT_BINARY_DIR}")
endfunction()

musicapp_test(test_util)
musicapp_test(test_library)
musicapp_test(test_tags)

# Hits musicbrainz.org; run by hand, not part of ctest.
add_executable(manual_mb_ping manual_mb_ping.c)
target_link_libraries(manual_mb_ping PRIVATE musiccore)

# kithipod tests. Each test_ipod_*.c is its own executable and gets a scratch directory
# (its own, under the test binary directory) as argv[1].
function(kithipod_test name)
  add_executable(${name} ${name}.c fixtures.c)
  target_link_libraries(${name} PRIVATE kithipod)
  # White-box: the tests use the library's private helpers and the ALAC decoder.
  target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
  kithsong_warnings(${name})
  set(_dir "${CMAKE_CURRENT_BINARY_DIR}/scratch/${name}")
  add_test(NAME ${name} COMMAND ${name} "${_dir}")
  set_tests_properties(${name} PROPERTIES TIMEOUT 120)
endfunction()

kithipod_test(test_ipod_sysinfo)
kithipod_test(test_ipod_itunesdb)
kithipod_test(test_ipod_playcounts)
kithipod_test(test_ipod_device)
target_link_libraries(test_ipod_device PRIVATE mc_stb)  # makes a cover picture
kithipod_test(test_ipod_encode)
target_link_libraries(test_ipod_encode PRIVATE ipod_alac mc_stb)
if(UNIX)
  find_package(Threads REQUIRED)
  target_link_libraries(test_ipod_encode PRIVATE Threads::Threads m)
endif()

# The plugin as the app runs it: kithsong-ipod loaded through core's loader from the folder
# it's built into, and a fake iPod synced through mc_tethered (tethered devices' tests in
# core/tests share their helpers with it).
add_executable(test_ipod_plugin test_ipod_plugin.c "${PROJECT_SOURCE_DIR}/core/tests/safe_trash.c")
target_compile_definitions(test_ipod_plugin PRIVATE
  MC_TEST_TRASH_DIR="${CMAKE_CURRENT_BINARY_DIR}/test-trash/test_ipod_plugin"
  IPOD_PLUGIN_DIR="${KITH_PLUGIN_OUTPUT_DIR}")
target_include_directories(test_ipod_plugin PRIVATE
  "${PROJECT_SOURCE_DIR}/core/src" "${PROJECT_SOURCE_DIR}/core/tests")
# kithipod makes the MP3 source and reads the iPod back; libopusenc makes the Opus one.
target_link_libraries(test_ipod_plugin PRIVATE kithcore mc_sqlite3 mbedcrypto kithipod mc_opusenc)
add_dependencies(test_ipod_plugin kithsong-ipod)
kithsong_warnings(test_ipod_plugin)
add_test(NAME test_ipod_plugin COMMAND test_ipod_plugin "${CMAKE_CURRENT_BINARY_DIR}/scratch/test_ipod_plugin")
set_tests_properties(test_ipod_plugin PROPERTIES TIMEOUT 300)

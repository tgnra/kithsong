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

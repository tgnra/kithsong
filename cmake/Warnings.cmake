# kithsong_warnings(<target>): project warning flags. Only applied to our own code,
# never to third_party.
function(kithsong_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4)
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion
      $<$<COMPILE_LANGUAGE:C>:-Wstrict-prototypes -Wmissing-prototypes>)
  endif()
endfunction()

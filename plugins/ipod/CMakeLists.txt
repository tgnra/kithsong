# iPod sync plugin (docs/plugins.md, "The iPod plugin").
#
# kithipod is the plugin's self-contained library: SysInfo, the iTunesDB reader and writer,
# Play Counts and On-The-Go, file placement, artwork, kithsong.json, and the ALAC and MP3
# encoders. It doesn't link kithcore and knows nothing of the plugin ABI; the plugin's
# shared library wraps it. Its encoders (Apple's ALAC, LAME) are built into the plugin
# only, never the app; see the Plugins section of LICENSES.md.

add_subdirectory(third_party)

add_library(kithipod STATIC
  src/util.c
  src/sysinfo.c
  src/itunesdb.c
  src/playcounts.c
  src/device.c
  src/kithjson.c
  src/artwork.c
  src/image.c
  src/m4a.c
  src/alac_shim.cpp
  src/id3.c
  src/encoder.c)
target_include_directories(kithipod PUBLIC include PRIVATE src)
target_link_libraries(kithipod PRIVATE ipod_alac ipod_lame mc_yyjson mc_stb)
if(WIN32)
  target_link_libraries(kithipod PRIVATE bcrypt)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(kithipod PRIVATE Threads::Threads m)
endif()
kithsong_warnings(kithipod)


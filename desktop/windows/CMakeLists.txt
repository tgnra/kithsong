enable_language(RC)

add_executable(musicapp WIN32
  src/main.c
  src/album_grid.c
  src/canvas.c
  src/import_view.c
  src/pick_folder.c
  src/sidebar.c
  src/theme.c
  src/app.rc)
target_link_libraries(musicapp PRIVATE musiccore comctl32 ole32 uuid shell32 windowscodecs dwmapi uxtheme)
target_compile_definitions(musicapp PRIVATE
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 WINVER=0x0A00)
musicapp_warnings(musicapp)

# Self-contained exe: no libgcc/libstdc++ DLLs next to it.
if(MINGW)
  target_link_options(musicapp PRIVATE -static)
endif()

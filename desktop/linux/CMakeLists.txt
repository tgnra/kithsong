find_package(PkgConfig REQUIRED)
# 1.4 for AdwToolbarView and AdwOverlaySplitView.
pkg_check_modules(ADW REQUIRED IMPORTED_TARGET libadwaita-1>=1.4 gtk4>=4.12)

add_executable(musicapp src/main.c)
target_link_libraries(musicapp PRIVATE musiccore PkgConfig::ADW)
musicapp_warnings(musicapp)
# GLib's headers trip -Wconversion / -Wpedantic; keep warnings strict for our code only.
target_compile_options(musicapp PRIVATE -Wno-pedantic)

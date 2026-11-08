# Plugins: shared libraries core loads at run time (docs/plugins.md). Built only when
# KITH_BUILD_PLUGINS is on; they ship separately from the app.
#
# In the build tree they go where the app built beside them looks first (the loader's
# default folders, kithcore/plugins.h), so a plugin built here is found by the app built
# here: <build>/desktop/windows/plugins next to kithsong.exe, and on Linux
# <build>/desktop/lib/kithsong/plugins, as the app is <build>/desktop/linux/kithsong.
# Installed, they go to <prefix>/plugins (Windows) or <prefix>/lib/kithsong/plugins.
if(WIN32)
  set(KITH_PLUGIN_OUTPUT_DIR "${PROJECT_BINARY_DIR}/desktop/windows/plugins")
  set(KITH_PLUGIN_INSTALL_DIR "plugins")
else()
  set(KITH_PLUGIN_OUTPUT_DIR "${PROJECT_BINARY_DIR}/desktop/lib/kithsong/plugins")
  set(KITH_PLUGIN_INSTALL_DIR "lib/kithsong/plugins")
endif()

add_subdirectory(ipod)

#include "radio_menu.h"

#include <stdio.h>

#include "kithcore/radio.h"
#include "kithcore/radio_seeds.h"
#include "wstr.h"

static void append(HMENU menu, UINT cmd, const char *seed) {
  AppendMenuW(menu, MF_STRING | (seed[0] ? 0 : MF_GRAYED), cmd, mc_wstr_mnemonic(MC_STR_START_RADIO));
}

void radio_menu_append_track(HMENU menu, UINT cmd, mc_library *lib, const char *uid, char *seed) {
  seed[0] = '\0';
  mc_radio_options opts = {.seed_track = uid};
  if (lib && mc_radio_seed_check(lib, &opts) == MC_OK) snprintf(seed, MC_UID_STRLEN, "%s", uid);
  append(menu, cmd, seed);
}

void radio_menu_append_station(HMENU menu, UINT cmd, mc_library *lib, const char *uid, char *seed) {
  seed[0] = '\0';
  mc_radio_options opts = {.seed_track = uid};
  if (lib && mc_radio_seed_check(lib, &opts) == MC_OK) snprintf(seed, MC_UID_STRLEN, "%s", uid);
  AppendMenuW(menu, MF_STRING | (seed[0] ? 0 : MF_GRAYED), cmd, mc_wstr_mnemonic(MC_STR_PLAY));
}

void radio_menu_append_album(HMENU menu, UINT cmd, mc_library *lib, const char *uid, char *seed) {
  seed[0] = '\0';
  if (lib) mc_radio_seed_for_album(lib, uid, seed);
  append(menu, cmd, seed);
}

void radio_menu_append_artist(HMENU menu, UINT cmd, mc_library *lib, const char *uid, char *seed) {
  seed[0] = '\0';
  if (lib) mc_radio_seed_for_artist(lib, uid, seed);
  append(menu, cmd, seed);
}

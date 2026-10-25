/* Playback. The miniaudio / dr_libs implementations will be compiled in this directory
 * (one translation unit with MINIAUDIO_IMPLEMENTATION etc.) when playback lands. */
#include "musiccore/playback.h"

mc_status mc_player_new(mc_player_state_cb cb, void *user, mc_player **out) {
  (void)cb;
  (void)user;
  if (out) *out = NULL;
  return MC_ERR_UNIMPLEMENTED;
}

void mc_player_free(mc_player *player) {
  (void)player;
}

mc_status mc_player_play_file(mc_player *player, const char *path) {
  (void)player;
  (void)path;
  return MC_ERR_UNIMPLEMENTED;
}

mc_status mc_player_pause(mc_player *player) {
  (void)player;
  return MC_ERR_UNIMPLEMENTED;
}

mc_status mc_player_resume(mc_player *player) {
  (void)player;
  return MC_ERR_UNIMPLEMENTED;
}

mc_status mc_player_stop(mc_player *player) {
  (void)player;
  return MC_ERR_UNIMPLEMENTED;
}

mc_status mc_player_seek_ms(mc_player *player, int64_t position_ms) {
  (void)player;
  (void)position_ms;
  return MC_ERR_UNIMPLEMENTED;
}

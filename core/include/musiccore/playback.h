/* Audio playback, built on miniaudio with bundled decoders (dr_flac, dr_mp3, ...). */
#ifndef MUSICCORE_PLAYBACK_H
#define MUSICCORE_PLAYBACK_H

#include "musiccore/musiccore.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mc_player mc_player;

typedef enum mc_player_state {
  MC_PLAYER_STOPPED,
  MC_PLAYER_PLAYING,
  MC_PLAYER_PAUSED
} mc_player_state;

/* State changes are reported on the audio thread; frontends must marshal to their UI thread. */
typedef void (*mc_player_state_cb)(mc_player_state state, void *user);

mc_status mc_player_new(mc_player_state_cb cb, void *user, mc_player **out);
void mc_player_free(mc_player *player);

mc_status mc_player_play_file(mc_player *player, const char *path);
mc_status mc_player_pause(mc_player *player);
mc_status mc_player_resume(mc_player *player);
mc_status mc_player_stop(mc_player *player);
mc_status mc_player_seek_ms(mc_player *player, int64_t position_ms);

#ifdef __cplusplus
}
#endif

#endif

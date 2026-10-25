/* Audio playback: a play queue, played gaplessly.
 *
 * Built on miniaudio for output (WASAPI, PulseAudio/ALSA, AAudio/OpenSL ES), dr_libs for
 * decoding FLAC, MP3 (with the LAME encoder delay and padding trimmed), WAV and AIFF, and
 * opusfile for Opus (pre-skip and end padding trimmed). Other formats fail with
 * MC_ERR_FORMAT and are skipped.
 *
 * A player thread decodes ahead into a buffer the audio device reads from. When a track
 * ends it carries straight on into the next one in the same buffer, so there is no gap
 * between tracks, even when their sample rates differ: everything is converted to one
 * output format. The audio device is opened on first play.
 *
 * Unlike most handles, a player may be used from any thread.
 */
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

typedef enum mc_player_event {
  MC_PLAYER_EVENT_STATE, /* playing, paused or stopped */
  MC_PLAYER_EVENT_TRACK, /* another queue item started, or the queue was replaced */
  MC_PLAYER_EVENT_ERROR  /* an item couldn't be played and was skipped, or there's no audio device */
} mc_player_event;

/* Called on the player's thread, never from inside a player call, so it may call the player
 * (except mc_player_free). Events that happen together may come as one. Frontends marshal
 * it to their UI thread and read mc_player_status_get() there. */
typedef void (*mc_player_cb)(void *user, mc_player_event event);

mc_status mc_player_new(mc_player_cb cb, void *user, mc_player **out);
/* Stops playback and closes the audio device. */
void mc_player_free(mc_player *player);

/* Replaces the queue with count files (UTF-8 paths, copied) and plays item start. */
mc_status mc_player_set_queue(mc_player *player, const char *const *paths, int count, int start);
/* Plays queue item index from its start. */
mc_status mc_player_skip_to(mc_player *player, int index);
/* The next item. After the last one, stops with the first item current, as when the queue
 * plays out by itself. */
mc_status mc_player_next(mc_player *player);
/* Back to the start of the item, or to the previous item when within its first 3 seconds. */
mc_status mc_player_previous(mc_player *player);

mc_status mc_player_pause(mc_player *player);
/* Carries on from where it was paused, or when stopped, plays the current item. */
mc_status mc_player_resume(mc_player *player);
/* Pauses while playing, otherwise resumes: what a play/pause key does. */
mc_status mc_player_toggle(mc_player *player);
/* Stops; the current item stays current, and plays from its start on resume. */
mc_status mc_player_stop(mc_player *player);
mc_status mc_player_seek_ms(mc_player *player, int64_t position_ms);

typedef struct mc_player_status {
  mc_player_state state;
  int index;           /* current queue item, -1 if the queue is empty */
  int count;           /* items in the queue */
  int64_t position_ms; /* in the current item, as heard */
  int64_t duration_ms; /* of the current item, 0 if unknown (or not open yet) */
} mc_player_status;

/* A consistent snapshot. Cheap enough to call on a UI timer. */
void mc_player_status_get(mc_player *player, mc_player_status *out);

#ifdef __cplusplus
}
#endif

#endif

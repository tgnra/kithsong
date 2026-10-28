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
 * The player also decides when a play counts (mc_play_counts()), for play counts and
 * scrobbling alike. Time listened is what was heard: pausing doesn't add to it, and seeking
 * neither adds to it nor starts a new play. Starting an item again (a skip to it, previous(),
 * a seek to 0, resuming after a stop) does.

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
  MC_PLAYER_EVENT_ERROR, /* an item couldn't be played and was skipped, or there's no audio device */
  MC_PLAYER_EVENT_PLAYED /* a play counts: take it with mc_player_take_played() */
} mc_player_event;

/* Called on the player's thread, never from inside a player call, so it may call the player
 * (except mc_player_free). Events that happen together may come as one. Frontends marshal
 * it to their UI thread and read mc_player_status_get() there. */
typedef void (*mc_player_cb)(void *user, mc_player_event event);

mc_status mc_player_new(mc_player_cb cb, void *user, mc_player **out);
/* Stops playback and closes the audio device. */
void mc_player_free(mc_player *player);

/* Replaces the queue with count files (UTF-8 paths, copied) and plays item start, or with
 * start -1, the first item (shuffled, any item). Plays not yet taken with
 * mc_player_take_played() are dropped, as their indexes were into the old queue: take them
 * first. */
mc_status mc_player_set_queue(mc_player *player, const char *const *paths, int count, int start);
/* Plays queue item index from its start. Shuffled, the rest are shuffled afresh after it. */
mc_status mc_player_skip_to(mc_player *player, int index);
/* The next item. After the last one, stops with the first item current, as when the queue
 * plays out by itself. */
mc_status mc_player_next(mc_player *player);
/* Back to the start of the item, or to the previous item when within its first 3 seconds. */
mc_status mc_player_previous(mc_player *player);

/* Shuffle (off to begin with). The queue then plays in a shuffled order (a Miller Shuffle,
 * see src/playback/shuffle.h) that next() and previous() follow too, and never ends: once
 * every item has played, it goes on in a new shuffle, which doesn't start with the item
 * just played. Turning it on carries on from the current item with the rest of the queue
 * shuffled; turning it off carries on from the current item in queue order. (Should the
 * next item be buffered already, in a track's last second, the new order starts after it
 * instead.) Set it before mc_player_set_queue() to start a queue shuffled. */
mc_status mc_player_set_shuffle(mc_player *player, int on);

mc_status mc_player_pause(mc_player *player);
/* Carries on from where it was paused, or when stopped, plays the current item. */
mc_status mc_player_resume(mc_player *player);
/* Pauses while playing, otherwise resumes: what a play/pause key does. */
mc_status mc_player_toggle(mc_player *player);
/* Stops; the current item stays current, and plays from its start on resume. */
mc_status mc_player_stop(mc_player *player);
mc_status mc_player_seek_ms(mc_player *player, int64_t position_ms);

/* The volume, 0 to 100 (100 to begin with; out of range is clamped). It's a gain on the
 * output, on a perceptual curve: the cube of volume / 100, so 50 is about -18 dB, 10 about
 * -60 dB and 0 silence. Changes fade over a few milliseconds, so they don't click. The OS's
 * own volume is left alone. Frontends keep it with mc_settings_set_volume(). */
mc_status mc_player_set_volume(mc_player *player, int volume);
/* Muted (off to begin with) is silence, keeping the volume for when it's unmuted. */
mc_status mc_player_set_muted(mc_player *player, int muted);

typedef struct mc_player_status {
  mc_player_state state;
  int index;           /* current queue item, -1 if the queue is empty */
  int count;           /* items in the queue */
  int shuffle;         /* see mc_player_set_shuffle(); there's always a next item then */
  int64_t position_ms; /* in the current item, as heard */
  int64_t duration_ms; /* of the current item, 0 if unknown (or not open yet) */
  int volume;          /* see mc_player_set_volume() */
  int muted;
} mc_player_status;

/* A consistent snapshot. Cheap enough to call on a UI timer. */
void mc_player_status_get(mc_player *player, mc_player_status *out);

/* Takes the oldest play that has counted since the last call: its queue item and when the
 * play began (wall clock ms). The frontend counts it (mc_library_record_play()) and scrobbles
 * it (mc_scrobbler_scrobble()). Call until MC_ERR_NOT_FOUND on each MC_PLAYER_EVENT_PLAYED;
 * the last few plays are kept, so none are lost to events arriving together. */
mc_status mc_player_take_played(mc_player *player, int *index, int64_t *started_ms);

/* Whether a play counts: once it has been listened to for half the track or four minutes,
 * whichever is less (four minutes when the duration is unknown, 0). The same rule as
 * Last.fm's for scrobbling. Never fails. */
int mc_play_counts(int64_t duration_ms, int64_t listened_ms);

#ifdef __cplusplus
}
#endif

#endif

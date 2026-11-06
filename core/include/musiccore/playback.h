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
 * It keeps the play log's rows too (musiccore/library.h), one for every item started, heard
 * or not: a skip after a second gets one as a full play does, and an item that can't be
 * played gets one that says so. A row is finished when its play ends, and the frontend passes
 * it on (mc_player_take_ended()), so an app that dies without closing loses the play under
 * way, and only that one.
 *
 * A radio play (mc_player_play_radio()) is a queue that grows: a thread of the player's keeps a
 * few tracks queued ahead from a station (musiccore/radio.h), so neither the audio nor a
 * frontend ever waits on it.
 *
 * Up next is what people add to (mc_player_play_next(); apps call it the queue, where this file's
 * queue is the list being played): it plays before the rest of that list, which then carries on,
 * shuffled or not, a station's too.
 *
 * Unlike most handles, a player may be used from any thread.
 */
#ifndef MUSICCORE_PLAYBACK_H
#define MUSICCORE_PLAYBACK_H

#include "musiccore/musiccore.h"

typedef struct mc_library mc_library;

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
  MC_PLAYER_EVENT_PLAYED, /* a play counts: take it with mc_player_take_played() */
  MC_PLAYER_EVENT_ENDED,  /* a play ended, counted or not: take it with mc_player_take_ended() */
  MC_PLAYER_EVENT_QUEUE,  /* the queue got new items, or a station stopped: see mc_player_play_radio() */
  MC_PLAYER_EVENT_UP_NEXT /* up next changed, by a call or by a track of it starting */
} mc_player_event;

/* Called on the player's thread, never from inside a player call, so it may call the player
 * (except mc_player_free). Events that happen together may come as one. Frontends marshal
 * it to their UI thread and read mc_player_status_get() there. */
typedef void (*mc_player_cb)(void *user, mc_player_event event);

mc_status mc_player_new(mc_player_cb cb, void *user, mc_player **out);
/* Stops playback and closes the audio device. */
void mc_player_free(mc_player *player);

/* What a queue is, for the play log. All of it is copied. */
typedef struct mc_play_origin {
  const char *const *track_uids; /* one per path; NULL or "" for an item that isn't logged */
  /* MC_PLAY_CONTEXT_UNKNOWN if there's no telling. A queue started shuffled with start -1 is
   * logged as MC_PLAY_CONTEXT_SHUFFLE whatever this says (but for radio), with context_uid
   * still saying what was shuffled. */
  mc_play_context context;
  const char *context_uid;       /* the album, playlist, group or artist; NULL if none */
} mc_play_origin;

/* Replaces the queue with count files (UTF-8 paths, copied) and plays item start, or with
 * start -1, the first item (shuffled, any item). Plays not yet taken with
 * mc_player_take_played() are dropped, as their indexes were into the old queue: take them
 * first. origin says what's being played; with NULL, nothing of the queue is logged. Up next
 * stays, and plays after item start. */
mc_status mc_player_set_queue(mc_player *player, const char *const *paths, int count, int start,
                              const mc_play_origin *origin);
/* Like mc_player_set_queue(), but stopped: item start (or with -1, the first item, which
 * shuffled is any) is current, and plays from its start on mc_player_resume(). So a frontend
 * can show what will play before it does, e.g. a shuffled queue's first item. */
mc_status mc_player_load_queue(mc_player *player, const char *const *paths, int count, int start,
                               const mc_play_origin *origin);
/* Plays a station from a track: the queue becomes that track alone, which starts at once, and
 * a station from it then keeps a few tracks queued ahead for as long as this queue stays, paused
 * or stopped too. Anything that replaces the queue ends it, as does mc_player_stop_radio(), or a
 * run of items that won't open: a folder that's gone, say. Plays are logged as
 * MC_PLAY_CONTEXT_RADIO with the seed as context_uid.
 *
 * library_dir is mc_library_dir(). The player reads it on its own thread with a connection of
 * its own, once, so this returns at once and needs no handle kept open. Check the seed first
 * with mc_radio_seed_check(): one that can't be used here plays by itself with nothing after
 * it. seed_path is its file (mc_track_info.path).
 *
 * The queue grows by itself, so a frontend that keeps something per item looks up the new ones
 * on each MC_PLAYER_EVENT_QUEUE, from its count to mc_player_status.count, with
 * mc_player_queue_uid(). It comes from the player's thread like every event, so it can lag the
 * items by as long as that thread is busy: seconds, if it's opening the audio device for the
 * first play. It does come, and mc_player_status.count is right meanwhile. Items are only ever
 * added at the end, and indexes stay good. Next from the last item, as the station has yet to
 * catch up, plays what it adds. A station's queue plays
 * in the station's order whatever shuffle says; shuffle applies again to the next queue. */
mc_status mc_player_play_radio(mc_player *player, const char *library_dir, const char *seed_path,
                               const char *seed_uid);
/* As mc_player_play_radio(), for a station that starts from another track than its seed, as the
 * Home page's do. The queue becomes the track start_uid, whose file is start_path, and it's the
 * seed that plays are logged under, as what the station is. mc_radio_seed_check() the start. */
mc_status mc_player_play_station(mc_player *player, const char *library_dir, const char *start_path,
                                 const char *start_uid, const char *seed_uid);
/* Stops adding to the queue, which plays out what it has. For when the library closes, as the
 * station's tracks are that library's. */
mc_status mc_player_stop_radio(mc_player *player);
/* Queue item index's track uid, into out, room for MC_UID_STRLEN; "" for one that isn't logged.
 * MC_ERR_NOT_FOUND if there's no such item. */
mc_status mc_player_queue_uid(mc_player *player, int index, char *out);

/* Queue item index's file is now at path (copied), an edit having moved it. Nothing stops: what's
 * playing or buffered reads on from the file it has open, and whatever opens it from here on, a
 * seek or its turn to play, uses path. MC_ERR_NOT_FOUND if there's no such item. */
mc_status mc_player_queue_set_path(mc_player *player, int index, const char *path);

/* Plays queue item index from its start. Shuffled, the rest are shuffled afresh after it. */
mc_status mc_player_skip_to(mc_player *player, int index);
/* The next item: up next's first track if there is one. After the last one, stops with the
 * first item current, as when the queue plays out by itself. */
mc_status mc_player_next(mc_player *player);
/* Back to the start of the item, or to the previous item when within its first 3 seconds. In a
 * track of up next that's the block's track before it, and the tracks go back up next from there.
 * With none before it, it's the list's item it followed, and the track is up next again. */
mc_status mc_player_previous(mc_player *player);

/* Shuffle (off to begin with). The queue then plays in a shuffled order (a Miller Shuffle,
 * see src/playback/shuffle.h) that next() and previous() follow too, and never ends: once
 * every item has played, it goes on in a new shuffle, which doesn't start with the item
 * just played. Turning it on carries on from the current item with the rest of the queue
 * shuffled; turning it off carries on from the current item in queue order. (Should the
 * next item be buffered already, in a track's last second, the new order starts after it
 * instead.) Set it before mc_player_set_queue() to start a queue shuffled. */
mc_status mc_player_set_shuffle(mc_player *player, int on);

/* The play order around the current item, which next() and previous() follow: out[i] gets
 * the queue item at place from + i, counted from the current item's (0 is the current item,
 * -1 the one before it, 1 the next), or -1 where there's none: before the first place, after
 * the last item in queue order, or with an empty queue. Shuffled, the order goes on in new
 * rounds, so an item can come up more than once. Up next's tracks come first after the current
 * item; the items before it are the list's. For showing what plays next, or telling a media
 * framework the order (Android's Media3 timeline). */
mc_status mc_player_order(mc_player *player, int from, int count, int *out);

/* A track to add to up next. All of it is copied. */
typedef struct mc_up_next_track {
  const char *path;
  const char *uid;           /* NULL or "" for one that isn't logged */
  const char *title, *artist; /* for its row when it's an entry of its own; may be NULL */
  int duration_ms;           /* 0 if unknown */
} mc_up_next_track;

/* What to add. Frontends leave out tracks with no file here: a path that won't open is skipped
 * when its turn comes. */
typedef struct mc_up_next_add {
  const mc_up_next_track *tracks;
  int count;
  /* What's added as a whole, an album say, is one entry, a block: its row has title and artist
   * (which may be NULL). With a NULL title, each track is an entry of its own, for a track by
   * itself or for mc_settings_queue_tracks(). */
  const char *title, *artist;
  mc_play_context context;   /* what the play log says of these tracks, as mc_play_origin does */
  const char *context_uid;
} mc_up_next_add;

/* What there is to add as a whole. Nothing of radio goes in up next. */
typedef enum mc_up_next_kind {
  MC_UP_NEXT_TRACK,
  MC_UP_NEXT_ALBUM,
  MC_UP_NEXT_ARTIST,
  MC_UP_NEXT_PLAYLIST,
  MC_UP_NEXT_SMART_PLAYLIST,
  MC_UP_NEXT_ALBUM_GROUP,
  MC_UP_NEXT_SMART_ALBUM_GROUP,
} mc_up_next_kind;

/* What Play Next and Play Last add for something in the library: its tracks with a file on this
 * device, as *out, for mc_player_play_next() or mc_player_play_last(). Anything but a track is one
 * block called name, unless name is NULL or as_tracks (mc_settings_queue_tracks()) says a track
 * each. A block's artist is an album's credit, what a playlist or album group is (that string,
 * in the language set now), and NULL for an artist's, whose row says how many tracks are left.
 * MC_ERR_NOT_FOUND if it has no track with a file here, or isn't there. Free with
 * mc_up_next_add_free(). */
mc_status mc_up_next_add_load(mc_library *lib, mc_up_next_kind kind, const char *uid, const char *name, int as_tracks,
                              mc_up_next_add **out);
void mc_up_next_add_free(mc_up_next_add *add);

/* Adds to the front of up next, the tracks in the order given, or to the end. Nothing starts:
 * what's playing plays on, and with nothing playing, mc_player_resume() or mc_player_next() starts
 * up next. MC_ERR_INVALID_ARG for no tracks.
 *
 * Tracks of up next are items like any other: they get an index on being added, and a frontend
 * looks them up as it does a station's (MC_PLAYER_EVENT_QUEUE, mc_player_queue_uid()), which is
 * why mc_player_status.count goes up. They keep their order whatever shuffle says. A track leaves
 * up next when it's heard, or when its file won't open; an entry leaves with its last track. Up
 * next isn't saved, and isn't changed by what's played from elsewhere. It's a list of this
 * library's files, so frontends clear it when the library changes. */
mc_status mc_player_play_next(mc_player *player, const mc_up_next_add *add);
mc_status mc_player_play_last(mc_player *player, const mc_up_next_add *add);

/* An entry as up next has it now: what's left of it. */
typedef struct mc_up_next_entry {
  uint32_t id;         /* stays while it's up next, and isn't used again */
  char *title;
  char *artist;        /* NULL if there's none */
  int block;           /* added as a whole */
  int tracks;          /* tracks left */
  int64_t duration_ms; /* of those, as far as they're known */
  int first_item;      /* the first of them, as a queue item */
  mc_play_context context;
  char context_uid[MC_UID_STRLEN];
} mc_up_next_entry;

typedef struct mc_up_next_list {
  mc_up_next_entry *items;
  int count;
} mc_up_next_list;

/* Up next now, in the order it plays. Free with mc_up_next_list_free(). */
mc_status mc_player_up_next(mc_player *player, mc_up_next_list **out);
void mc_up_next_list_free(mc_up_next_list *list);
/* Entry id goes to place to (0 first, out of range is clamped), all of it as one. What's playing
 * isn't touched by this or the next two: only what comes after it. MC_ERR_NOT_FOUND if it's
 * gone. */
mc_status mc_player_up_next_move(mc_player *player, uint32_t id, int to);
/* Entry id leaves up next, all that's left of it. MC_ERR_NOT_FOUND if it's gone. */
mc_status mc_player_up_next_remove(mc_player *player, uint32_t id);
/* Empties up next. Playback goes on. */
mc_status mc_player_up_next_clear(mc_player *player);
/* Entry id's row has a new title and artist (may be NULL), an edit having changed them.
 * MC_ERR_NOT_FOUND if it's gone. */
mc_status mc_player_up_next_rename(mc_player *player, uint32_t id, const char *title, const char *artist);

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
  int count;           /* items in the queue, those of up next among them */
  int shuffle;         /* see mc_player_set_shuffle(); there's always a next item then */
  int radio;           /* a station is adding to the queue; there's always a next item then too */
  int64_t position_ms; /* in the current item, as heard */
  int64_t duration_ms; /* of the current item, 0 if unknown (or not open yet) */
  int volume;          /* see mc_player_set_volume() */
  int muted;
  int up_next;         /* entries of up next */
  unsigned up_next_changes; /* goes up with every change, for telling if a list is out of date */
} mc_player_status;

/* A consistent snapshot. Cheap enough to call on a UI timer. */
void mc_player_status_get(mc_player *player, mc_player_status *out);

/* Takes the oldest play that has counted since the last call: its queue item and when the
 * play began (wall clock ms). The frontend counts it (mc_library_record_play()) and scrobbles
 * it (mc_scrobbler_scrobble()). Call until MC_ERR_NOT_FOUND on each MC_PLAYER_EVENT_PLAYED;
 * the last few plays are kept, so none are lost to events arriving together. */
mc_status mc_player_take_played(mc_player *player, int *index, int64_t *started_ms);

/* Takes the oldest play that has ended since the last call, for mc_library_log_play(). Call
 * until MC_ERR_NOT_FOUND on each MC_PLAYER_EVENT_ENDED. Unlike plays that count these say
 * which track they were, so they outlive the queue; if nobody takes them, the last 64 are
 * kept. */
mc_status mc_player_take_ended(mc_player *player, mc_play_event *out);

/* Ends the play under way there and then, as stopped, so mc_player_take_ended() has it at
 * once: for before mc_player_free() or closing the library, which leave no time to wait for
 * an event. Playback isn't touched; should it go on, so does the log, in a row of its own. */
mc_status mc_player_end_play(mc_player *player);

/* Whether a play counts: once it has been listened to for half the track or four minutes,
 * whichever is less (four minutes when the duration is unknown, 0). The same rule as
 * Last.fm's for scrobbling. Never fails. */
int mc_play_counts(int64_t duration_ms, int64_t listened_ms);

#ifdef __cplusplus
}
#endif

#endif

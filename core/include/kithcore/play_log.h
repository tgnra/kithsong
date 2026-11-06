/* A play log row, which the player fills in and the library keeps (mc_player_take_ended(),
 * mc_library_log_play()). Included by musiccore.h, ahead of both of theirs. */
#ifndef KITHCORE_PLAY_LOG_H
#define KITHCORE_PLAY_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

/* How a queue came to be played. Stored by name, so values can be added; a row with a name
 * this version doesn't know reads as unknown. Android's CorePlayer.PlayContext follows this
 * order. */
typedef enum mc_play_context {
  MC_PLAY_CONTEXT_UNKNOWN,     /* nobody said; always a valid value */
  MC_PLAY_CONTEXT_MANUAL,      /* one track picked by itself, with nothing to go on to */
  MC_PLAY_CONTEXT_ALBUM,       /* an album's tracks, from its start or a track picked in it */
  MC_PLAY_CONTEXT_PLAYLIST,    /* a playlist's or a smart list's */
  MC_PLAY_CONTEXT_ALBUM_GROUP, /* a group's albums */
  MC_PLAY_CONTEXT_ARTIST,      /* an artist's tracks */
  MC_PLAY_CONTEXT_SHUFFLE,     /* the player's: any of these started shuffled, no track picked */
  MC_PLAY_CONTEXT_RADIO        /* a station's, chosen by the app: mc_player_play_radio() */
} mc_play_context;

/* How a play ended. Android's CorePlayer.PlayEnd follows this order. */
typedef enum mc_play_end {
  MC_PLAY_END_UNKNOWN,  /* only read, from a newer version's row */
  MC_PLAY_END_FINISHED, /* played to its end */
  MC_PLAY_END_SKIPPED,  /* next, previous, another item picked, or started again */
  MC_PLAY_END_STOPPED,  /* stopped, or the app closed */
  MC_PLAY_END_REPLACED, /* a new queue took over */
  MC_PLAY_END_FAILED    /* couldn't be played, so listened_ms is 0 */
} mc_play_end;

typedef struct mc_play_event {
  char track_uid[MC_UID_STRLEN];
  int64_t started_ms;  /* wall clock, when it was first heard (or started, if it never was) */
  int64_t listened_ms; /* time heard: pauses don't add to it, and seeks skip what they skip */
  mc_play_end end;
  mc_play_context context;
  char context_uid[MC_UID_STRLEN]; /* the album, playlist, group, artist or a station's seed track; "" if none */
} mc_play_event;

#ifdef __cplusplus
}
#endif

#endif

/* Playing the queue on another device: the player's remote mode, which Google Cast
 * (cast/cast.c) drives. Internal to core.
 *
 * While remote, the player stays what decides what plays: the queue, the shuffled order,
 * stations and up next all go on as they do here, and every mc_player_* call works as
 * always. But nothing is decoded or heard here. Instead the player tells the remote what to
 * do (ops below): load the current item at a place, playing or paused; queue the item after
 * it, which the remote fetches ahead and goes on to by itself, so tracks follow each other
 * without a gap of any length; play, pause, seek, stop. If what comes next changes (up next,
 * shuffle, a station adding), the queued item is replaced.
 *
 * The remote reports back what it does (mc_player_remote_*), and the player takes that as
 * what's playing: mc_player_status_get() shows the remote's state and position, pausing or
 * seeking from the remote's own controls shows here too, and its going on to the queued item
 * moves the player on. Time listened is the time the remote reported playing (buffering and
 * pausing don't add to it), so plays count (mc_player_take_played()) and the play log fills
 * in (mc_player_take_ended()) as they would here, rows opening and closing at the same
 * moments. The handovers themselves (mc_player_remote_begin() and _end()) open a row only
 * once the item is heard, so moving playback leaves none for what wasn't then played.
 *
 * Every item and queued item the player gives the remote has an id, unique for the player,
 * which the remote's reports give back, so a late report about an item that's been replaced
 * is told apart and dropped.
 *
 * Threading. Ops are called on the player's thread with the player's lock held, so they must
 * only take note and return, never call the player. The reports may be called from any
 * thread, but not from inside an op. */
#ifndef MC_PLAYBACK_REMOTE_H
#define MC_PLAYBACK_REMOTE_H

#include <stdint.h>

#include "kithcore/playback.h"

typedef struct mc_remote_track {
  int index;           /* the queue item */
  const char *path;    /* its file, valid during the call */
  int64_t duration_ms; /* 0 if unknown */
} mc_remote_track;

typedef struct mc_remote_ops {
  /* Replaces whatever the remote has with track, at position_ms, playing or paused. */
  void (*load)(void *user, unsigned id, const mc_remote_track *track, int64_t position_ms, int play);
  /* The item to go on to after the loaded one: replaces any queued before. NULL for none. */
  void (*queue_next)(void *user, unsigned id, const mc_remote_track *next);
  void (*play)(void *user);
  void (*pause)(void *user);
  void (*seek)(void *user, int64_t position_ms);
  /* Stops and unloads: nothing is playing (the player stopped, or its queue went). */
  void (*stop)(void *user);
  /* The player is being freed (mc_player_free()) while still remote: the remote must stop
   * using it before this returns. Called on the freeing thread, without the player's lock,
   * and before anything is freed; reports already under way may finish meanwhile. */
  void (*detached)(void *user);
} mc_remote_ops;

typedef enum mc_remote_state {
  MC_REMOTE_LOADING, /* loading or buffering: not heard, but meant to play */
  MC_REMOTE_PLAYING,
  MC_REMOTE_PAUSED
} mc_remote_state;

/* Moves playback to the remote. What was playing here ends as a play (stopped), and the
 * current item is loaded on the remote at the same place, playing or paused as it was; a
 * play that hadn't yet counted goes on counting there. A stopped player stays stopped, with
 * nothing loaded until it plays. MC_ERR_IN_USE if it's remote already. */
mc_status mc_player_remote_begin(mc_player *player, const mc_remote_ops *ops, void *user);
/* Moves playback back here: the play under way ends (stopped), and the player is left on the
 * item the remote was on, at the place it had got to, paused (or stopped, if it was). The
 * remote isn't told anything: that's up to whoever ends it. No ops are called once this
 * returns. */
void mc_player_remote_end(mc_player *player);

/* The remote is on item id, in state, position_ms into it. An id that is the queued item's
 * means the remote has gone on to it by itself. Playing or paused, as the remote now says, is
 * taken as wanted, from whoever asked the remote for it. */
void mc_player_remote_status(mc_player *player, unsigned id, mc_remote_state state, int64_t position_ms);
/* Item id played to its end with nothing queued after it on the remote: the player goes on to
 * its next item, if there is one by now, else stops at the start, as when a queue plays out. */
void mc_player_remote_finished(mc_player *player, unsigned id);
/* The remote couldn't play item id: it's skipped, as a file that won't open is here, unless
 * items keep failing, in which case the player stops at the start. */
void mc_player_remote_failed(mc_player *player, unsigned id);
/* What the remote played was stopped from elsewhere: the player pauses where it had got to,
 * and loads it again if played. */
void mc_player_remote_dropped(mc_player *player);
/* The remote lost what it had (it restarted, say) through no one's choice: it's loaded again
 * at the place it had got to, playing or paused as wanted. */
void mc_player_remote_reload(mc_player *player);

#endif

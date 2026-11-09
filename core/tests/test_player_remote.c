/* The player's remote mode (playback/remote.h), against a fake remote that records what it's
 * told, with silent WAVs. No audio device is needed, as nothing plays here: the handover,
 * loading and queueing the next item, the remote's own pause and its going on to the queued
 * item, counting plays and the play log from what the remote reports, Next, up next replacing
 * what's queued, seeking, an item the remote can't play, being stopped from elsewhere, the
 * queue playing out, the handover back, and freeing the player while remote. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kithcore/kithcore.h"
#include "playback/remote.h"
#include "test.h"
#include "util/platform.h"

typedef enum op_kind { OP_LOAD, OP_QUEUE, OP_PLAY, OP_PAUSE, OP_SEEK, OP_STOP, OP_DETACHED } op_kind;

typedef struct op {
  op_kind kind;
  unsigned id;
  int index; /* -1 for none */
  int64_t ms;
  int play;
} op;

static mc_mutex lock;
static op ops[256];
static int nops, taken;

static void record(op o) {
  mc_mutex_lock(&lock);
  if (nops < 256) ops[nops++] = o;
  mc_mutex_unlock(&lock);
}

static void on_load(void *user, unsigned id, const mc_remote_track *t, int64_t ms, int play) {
  (void)user;
  record((op){OP_LOAD, id, t->index, ms, play});
}
static void on_queue(void *user, unsigned id, const mc_remote_track *t) {
  (void)user;
  record((op){OP_QUEUE, id, t ? t->index : -1, 0, 0});
}
static void on_play(void *user) {
  (void)user;
  record((op){OP_PLAY, 0, -1, 0, 0});
}
static void on_pause(void *user) {
  (void)user;
  record((op){OP_PAUSE, 0, -1, 0, 0});
}
static void on_seek(void *user, int64_t ms) {
  (void)user;
  record((op){OP_SEEK, 0, -1, ms, 0});
}
static void on_stop(void *user) {
  (void)user;
  record((op){OP_STOP, 0, -1, 0, 0});
}
static void on_detached(void *user) {
  mc_player *player = *(mc_player **)user;
  record((op){OP_DETACHED, 0, -1, 0, 0});
  mc_player_remote_end(player);
}

static const mc_remote_ops remote_ops = {on_load, on_queue, on_play, on_pause, on_seek, on_stop, on_detached};

/* Waits up to a second for the next op not yet taken to be of kind, and takes it: 1 if it
 * came, into *out. Ops of other kinds before it are taken too, and fail the check. */
static int next_op(op_kind kind, op *out) {
  uint64_t end = mc_mono_ms() + 1000;
  while (mc_mono_ms() < end) {
    mc_mutex_lock(&lock);
    if (taken < nops) {
      op o = ops[taken++];
      mc_mutex_unlock(&lock);
      if (o.kind == kind) {
        if (out) *out = o;
        return 1;
      }
      fprintf(stderr, "unexpected op %d (index %d) while waiting for %d\n", (int)o.kind, o.index, (int)kind);
      return 0;
    }
    mc_mutex_unlock(&lock);
    mc_sleep_ms(2);
  }
  return 0;
}

/* Whether no op comes within ms. */
static int no_op(unsigned ms) {
  mc_sleep_ms(ms);
  mc_mutex_lock(&lock);
  int none = taken == nops;
  if (!none) fprintf(stderr, "unexpected op %d (index %d)\n", (int)ops[taken].kind, ops[taken].index);
  taken = nops;
  mc_mutex_unlock(&lock);
  return none;
}

/* Polls until the player shows the state and index, or a second passes. */
static int wait_status(mc_player *player, mc_player_state state, int index) {
  uint64_t end = mc_mono_ms() + 1000;
  mc_player_status st;
  do {
    mc_player_status_get(player, &st);
    if (st.state == state && st.index == index) return 1;
    mc_sleep_ms(2);
  } while (mc_mono_ms() < end);
  fprintf(stderr, "status: state %d index %d, wanted %d %d\n", (int)st.state, st.index, (int)state, index);
  return 0;
}

static void put16(FILE *f, unsigned v) {
  fputc((int)(v & 0xff), f);
  fputc((int)(v >> 8 & 0xff), f);
}

static void put32(FILE *f, uint32_t v) {
  put16(f, v & 0xffff);
  put16(f, v >> 16);
}

/* A silent 16-bit PCM WAV. */
static void write_wav(const char *path, unsigned ms) {
  FILE *f = mc_fopen(path, "wb");
  if (!f) return;
  unsigned rate = 8000;
  uint32_t frames = rate * ms / 1000, bytes = frames * 2;
  fwrite("RIFF", 1, 4, f);
  put32(f, 36 + bytes);
  fwrite("WAVEfmt ", 1, 8, f);
  put32(f, 16);
  put16(f, 1);
  put16(f, 1);
  put32(f, rate);
  put32(f, rate * 2);
  put16(f, 2);
  put16(f, 16);
  fwrite("data", 1, 4, f);
  put32(f, bytes);
  for (uint32_t i = 0; i < bytes; i++) fputc(0, f);
  fclose(f);
}

static char *join(const char *dir, const char *name) {
  size_t n = strlen(dir) + strlen(name) + 2;
  char *s = malloc(n);
  if (s) snprintf(s, n, "%s/%s", dir, name);
  return s;
}

/* Takes the ended plays, into rows (up to max); returns how many. */
static int take_rows(mc_player *player, mc_play_event *rows, int max) {
  int n = 0;
  mc_play_event e;
  while (mc_player_take_ended(player, &e) == MC_OK)
    if (n < max) rows[n++] = e;
  return n;
}

int main(int argc, char **argv) {
  const char *dir = argc > 1 ? argv[1] : ".";
  mc_mutex_init(&lock);
  /* Long enough that nothing ends by itself here: the remote says when things end. */
  char *a = join(dir, "remote_a.wav"), *b = join(dir, "remote_b.wav"), *c = join(dir, "remote_c.wav"),
       *u = join(dir, "remote_u.wav"), *missing = join(dir, "remote_missing.wav");
  write_wav(a, 2000);
  write_wav(b, 2000);
  write_wav(c, 2000);
  write_wav(u, 2000);
  const char *uids[] = {"aaaaaaaaaaaaaaaaaaaaaaaaaa", "bbbbbbbbbbbbbbbbbbbbbbbbbb", "cccccccccccccccccccccccccc"};
  const char *paths[] = {a, b, c};
  mc_play_origin origin = {uids, MC_PLAY_CONTEXT_ALBUM, "album"};

  mc_player *player = NULL;
  CHECK(mc_player_new(NULL, NULL, &player) == MC_OK);
  CHECK(mc_player_load_queue(player, paths, 3, 0, &origin) == MC_OK);
  CHECK(wait_status(player, MC_PLAYER_STOPPED, 0));

  /* Handed over stopped, nothing is loaded until it plays. */
  CHECK(mc_player_remote_begin(player, &remote_ops, &player) == MC_OK);
  CHECK(mc_player_remote_begin(player, &remote_ops, &player) == MC_ERR_IN_USE);
  CHECK(no_op(100));

  /* Playing loads the current item, playing, and then queues the next. */
  op o, q;
  CHECK(mc_player_resume(player) == MC_OK);
  CHECK(next_op(OP_LOAD, &o) && o.index == 0 && o.ms == 0 && o.play);
  CHECK(next_op(OP_QUEUE, &q) && q.index == 1 && q.id != o.id);
  unsigned a_id = o.id, b_id = q.id;

  /* Until the remote says it plays, nothing is heard. Then the position runs. */
  mc_player_status st;
  mc_player_status_get(player, &st);
  CHECK(st.state == MC_PLAYER_PLAYING && st.index == 0 && st.position_ms == 0 && st.duration_ms == 2000);
  mc_player_remote_status(player, a_id, MC_REMOTE_PLAYING, 0);
  mc_sleep_ms(1300);
  mc_player_status_get(player, &st);
  CHECK(st.position_ms >= 1300 && st.position_ms < 1700);
  int index;
  int64_t started;
  CHECK(mc_player_take_played(player, &index, &started) == MC_OK && index == 0); /* half of 2 s heard */

  /* Paused on the remote, the player follows, and the position holds where it said. */
  mc_player_remote_status(player, a_id, MC_REMOTE_PAUSED, 1400);
  mc_player_status_get(player, &st);
  CHECK(st.state == MC_PLAYER_PAUSED && st.position_ms == 1400);
  mc_sleep_ms(100);
  mc_player_status_get(player, &st);
  CHECK(st.position_ms == 1400);
  CHECK(no_op(50)); /* nothing to tell the remote: it said so itself */

  /* A stale report, of an id that isn't loaded, changes nothing. */
  mc_player_remote_status(player, 9999, MC_REMOTE_PLAYING, 0);
  mc_player_status_get(player, &st);
  CHECK(st.state == MC_PLAYER_PAUSED);

  /* Resumed here, the remote is told. */
  CHECK(mc_player_resume(player) == MC_OK);
  CHECK(next_op(OP_PLAY, NULL));
  mc_player_remote_status(player, a_id, MC_REMOTE_PLAYING, 1400);

  /* The remote goes on to the queued item by itself: so does the player, and the next is
   * queued. The first track's row has ended as finished. */
  mc_sleep_ms(50);
  mc_player_remote_status(player, b_id, MC_REMOTE_PLAYING, 0);
  CHECK(wait_status(player, MC_PLAYER_PLAYING, 1));
  CHECK(next_op(OP_QUEUE, &q) && q.index == 2);
  unsigned c_id = q.id;
  mc_play_event rows[8];
  mc_sleep_ms(300); /* the player's thread passes it on */
  int n = take_rows(player, rows, 8);
  CHECK(n == 1 && !strcmp(rows[0].track_uid, uids[0]) && rows[0].end == MC_PLAY_END_FINISHED);
  CHECK(n == 1 && rows[0].listened_ms >= 1300 && rows[0].listened_ms < 1800 && rows[0].context == MC_PLAY_CONTEXT_ALBUM);

  /* Up next replaces what's queued. */
  mc_up_next_track t = {u, "uuuuuuuuuuuuuuuuuuuuuuuuuu", "U", NULL, 2000};
  mc_up_next_add add = {&t, 1, NULL, NULL, MC_PLAY_CONTEXT_UNKNOWN, NULL};
  CHECK(mc_player_play_next(player, &add) == MC_OK);
  CHECK(next_op(OP_QUEUE, &q) && q.index == 3 && q.id != c_id);

  /* A seek in what's loaded is a seek on the remote. */
  CHECK(mc_player_seek_ms(player, 500) == MC_OK);
  CHECK(next_op(OP_SEEK, &o) && o.ms == 500);
  CHECK(no_op(100)); /* what's queued stays */
  mc_player_remote_status(player, b_id, MC_REMOTE_PLAYING, 500);

  /* Next loads what the player has next: up next's track. Then the list's next is queued. */
  CHECK(mc_player_next(player) == MC_OK);
  CHECK(next_op(OP_LOAD, &o) && o.index == 3 && o.play);
  CHECK(next_op(OP_QUEUE, &q) && q.index == 2);
  unsigned u_id = o.id;
  mc_sleep_ms(300);
  n = take_rows(player, rows, 8);
  CHECK(n == 1 && !strcmp(rows[0].track_uid, uids[1]) && rows[0].end == MC_PLAY_END_SKIPPED);

  /* The remote can't play it: on to the next, which is loaded. */
  mc_player_remote_failed(player, u_id);
  CHECK(next_op(OP_LOAD, &o) && o.index == 2 && o.play);
  CHECK(no_op(100)); /* nothing after the last item */
  c_id = o.id;
  n = take_rows(player, rows, 8);
  CHECK(n == 1 && rows[0].end == MC_PLAY_END_FAILED);
  mc_player_remote_status(player, c_id, MC_REMOTE_PLAYING, 0);

  /* Stopped from elsewhere: paused here at the place, and loaded again on play. */
  mc_sleep_ms(200);
  mc_player_remote_dropped(player);
  mc_player_status_get(player, &st);
  CHECK(st.state == MC_PLAYER_PAUSED && st.index == 2 && st.position_ms >= 150);
  CHECK(mc_player_resume(player) == MC_OK);
  CHECK(next_op(OP_LOAD, &o) && o.index == 2 && o.ms >= 150 && o.play);
  c_id = o.id;
  mc_player_remote_status(player, c_id, MC_REMOTE_PLAYING, o.ms);

  /* The last item played out on the remote: stopped at the start, as here. */
  mc_player_remote_finished(player, c_id);
  CHECK(wait_status(player, MC_PLAYER_STOPPED, 0));
  CHECK(no_op(100)); /* the remote has stopped by itself already */

  /* Played again: a file that's gone is skipped, as here. */
  const char *with_missing[] = {missing, a};
  CHECK(mc_player_set_queue(player, with_missing, 2, 0, NULL) == MC_OK);
  CHECK(next_op(OP_LOAD, &o) && o.index == 1);
  mc_player_remote_status(player, o.id, MC_REMOTE_PLAYING, 300);
  mc_player_status_get(player, &st);
  CHECK(st.index == 1 && st.position_ms >= 300);

  /* Stopping stops the remote. */
  CHECK(mc_player_stop(player) == MC_OK);
  CHECK(next_op(OP_STOP, NULL));
  CHECK(wait_status(player, MC_PLAYER_STOPPED, 1));

  /* Back here, the player keeps the item and the place. */
  CHECK(mc_player_resume(player) == MC_OK);
  CHECK(next_op(OP_LOAD, &o) && o.index == 1);
  mc_player_remote_status(player, o.id, MC_REMOTE_PLAYING, 700);
  mc_player_remote_end(player);
  mc_player_status_get(player, &st);
  CHECK(st.index == 1 && st.position_ms >= 700 && st.state == MC_PLAYER_PAUSED);
  CHECK(no_op(100));

  /* Freed while remote: the remote hears of it first. */
  CHECK(mc_player_remote_begin(player, &remote_ops, &player) == MC_OK);
  mc_player_free(player);
  mc_mutex_lock(&lock);
  int detached = 0;
  for (int i = 0; i < nops; i++) detached |= ops[i].kind == OP_DETACHED;
  mc_mutex_unlock(&lock);
  CHECK(detached);

  free(a);
  free(b);
  free(c);
  free(u);
  free(missing);
  return TEST_RESULT();
}

#include "kithcore/home_picks.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "kithcore/radio_stations.h"
#include "library/library_internal.h"
#include "util/platform.h"

/* Played a Lot Recently and Not Played in a While go by the play log, read back as far as
 * A_WHILE_DAYS, and fall back on a guess from track_plays for what the log can't see.
 *
 * The log has every start, so a play only counts as real if it didn't fail and enough of it was
 * heard: played_for_real(). Of those, a station's aren't a choice: the app picked the track, which
 * says little about wanting its album. So radio plays put nothing in Played a Lot Recently. They
 * were still heard, though, so they do keep an album out of Not Played in a While, and count as
 * having been played before for it.
 *
 * track_plays keeps one count per track for all time and the time of its last play, so all it can
 * say of "played a lot recently" is "many of its tracks had their last play lately": a track
 * played once this week after a hundred plays years ago looks the same as one on repeat. It's the
 * only record of plays from before the log began, so that guess stands for an album or playlist
 * last played before then, and for no other: after that the log knows better, and track_plays
 * can't tell a station's play from a chosen one. With an empty log that's all of them, and with
 * one older than the windows it's none. Plays from the log rank ahead of guesses of their kind. */
enum {
  LATELY_DAYS = 14,    /* the window of Played a Lot Recently */
  LATELY_PLAYS = 3,    /* real plays in it that make an album or playlist belong there */
  LATELY_TRACKS = 3,   /* the guess at that: how many tracks had their last play in it */
  A_WHILE_DAYS = 90,   /* nothing played this recently, for Not Played in a While */
  A_WHILE_TRACKS = 2,  /* tracks ever played: fewer is more likely a stray track from shuffle */
  REAL_PLAY_MS = 30000, /* heard for this long, or half the track if that's less, is a real play */
  ROW_ENOUGH = 8,      /* fewer than this in a row and its rules relax */
  RANDOM_PLAYLISTS = MC_HOME_ROW_MAX / 4, /* albums would crowd them out, there being many more */
};

/* Why something could be in Played a Lot Recently, best first. The first two always get in, the
 * rest only while the row is short of ROW_ENOUGH, so a quiet fortnight doesn't empty it. */
typedef enum lately_tier {
  LATELY_REPEATED,    /* LATELY_PLAYS real plays in the window */
  LATELY_GUESS_MANY,  /* from before the log: LATELY_TRACKS tracks last played in the window */
  LATELY_ANY,         /* a real play in the window */
  LATELY_GUESS_ANY,   /* from before the log: a track last played in the window */
  A_WHILE_ANY,        /* a real play since A_WHILE_DAYS ago */
  A_WHILE_GUESS_ANY,  /* from before the log: a track last played since then */
  LATELY_NONE
} lately_tier;

#define DAY_MS (24 * 60 * 60 * 1000LL)

/* What the rows are picked from, listed once per load. */
typedef struct uid_entry {
  const char *uid;
  int index;
} uid_entry;

/* What the log and track_plays say of an album or playlist. */
typedef struct heard {
  mc_recent_kind kind; /* MC_RECENT_ALBUM or MC_RECENT_PLAYLIST */
  char uid[MC_UID_STRLEN];
  /* From the log: real plays of its tracks, other than a station's. */
  int plays_lately;   /* in the last LATELY_DAYS */
  int plays_a_while;  /* in the last A_WHILE_DAYS, those included */
  int64_t chosen_ms;  /* the newest of them; 0 if none */
  /* The guess, all 0 if the log began before its last play. */
  int guess_tracks;   /* its tracks last played in the last LATELY_DAYS */
  int64_t guess_ms;   /* its last play */
  /* From both. */
  int tracks_played;  /* its tracks with a play in track_plays */
  int64_t heard_ms;   /* the newest play of any kind, a station's too; 0 if too old to have a time */
  lately_tier tier;
} heard;

typedef struct sources {
  mc_library *lib;
  unsigned seed;
  int64_t now_ms;
  mc_album_list *albums;
  uid_entry *album_index; /* by uid */
  mc_artist_list *artists;
  uid_entry *artist_index; /* by uid */
  mc_playlist_list *playlists;
  mc_album_group_list *groups;
  mc_smart_list *smart;
  heard *heard; /* each album and playlist with a play, by uid */
  int heard_count;
} sources;

static int entry_cmp(const void *a, const void *b) {
  return strcmp(((const uid_entry *)a)->uid, ((const uid_entry *)b)->uid);
}

static int find_entry(const uid_entry *index, int n, const char *uid) {
  uid_entry key = {uid, 0};
  const uid_entry *at = index ? bsearch(&key, index, (size_t)n, sizeof *index, entry_cmp) : NULL;
  return at ? at->index : -1;
}

static const mc_album_info *find_album(const sources *s, const char *uid) {
  int i = s->albums ? find_entry(s->album_index, s->albums->count, uid) : -1;
  return i >= 0 ? &s->albums->items[i] : NULL;
}

static const mc_artist_info *find_artist(const sources *s, const char *uid) {
  int i = s->artists ? find_entry(s->artist_index, s->artists->count, uid) : -1;
  return i >= 0 ? &s->artists->items[i] : NULL;
}

static const mc_playlist_info *find_playlist(const sources *s, const char *uid) {
  for (int i = 0; s->playlists && i < s->playlists->count; i++)
    if (!strcmp(s->playlists->items[i].uid, uid)) return &s->playlists->items[i];
  return NULL;
}

static const mc_album_group_info *find_group(const sources *s, const char *uid) {
  for (int i = 0; s->groups && i < s->groups->count; i++)
    if (!strcmp(s->groups->items[i].uid, uid)) return &s->groups->items[i];
  return NULL;
}

/* Whether a log row was really listened to. Half of a short track will do, so an interlude can
 * count but seeking to the end of a long track can't. A row's end only rules out failures: a good
 * listen that was then skipped is still a play, and the time heard tells it from a skip. */
static int played_for_real(const mc_play_event *e, int64_t duration_ms) {
  if (e->end == MC_PLAY_END_FAILED) return 0;
  int64_t needed = duration_ms > 0 && duration_ms / 2 < REAL_PLAY_MS ? duration_ms / 2 : REAL_PLAY_MS;
  return e->listened_ms >= needed;
}

/* One track's real plays in the log. */
typedef struct track_tally {
  char uid[MC_UID_STRLEN];
  int plays_lately, plays_a_while;
  int64_t chosen_ms, heard_ms;
} track_tally;

static int tally_cmp(const void *a, const void *b) {
  return strcmp(((const track_tally *)a)->uid, ((const track_tally *)b)->uid);
}

static int event_track_cmp(const void *a, const void *b) {
  return strcmp(((const mc_play_log_item *)a)->event.track_uid, ((const mc_play_log_item *)b)->event.track_uid);
}

static int64_t newest(int64_t a, int64_t b) {
  return a > b ? a : b;
}

/* A new entry for uid, for fold_heard() to add to any others of it. NULL if out of memory. */
static heard *new_heard(sources *s, int *cap, mc_recent_kind kind, const char *uid) {
  if (s->heard_count == *cap) {
    int more = *cap ? *cap * 2 : 64;
    heard *items = realloc(s->heard, (size_t)more * sizeof *items);
    if (!items) return NULL;
    s->heard = items;
    *cap = more;
  }
  heard *h = &s->heard[s->heard_count++];
  memset(h, 0, sizeof *h);
  h->kind = kind;
  snprintf(h->uid, sizeof h->uid, "%s", uid);
  return h;
}

static void credit(sources *s, int *cap, mc_recent_kind kind, const char *uid, const track_tally *t) {
  heard *h = new_heard(s, cap, kind, uid);
  if (!h) return;
  h->plays_lately = t->plays_lately;
  h->plays_a_while = t->plays_a_while;
  h->chosen_ms = t->chosen_ms;
  h->heard_ms = t->heard_ms;
}

/* Tallies items[0..n), the rows of one track, and credits its album. Returns whether it has a
 * real play. */
static int tally_track(sources *s, int *cap, sqlite3_stmt *track, const mc_play_log_item *items, int n,
                       int64_t lately_ms, track_tally *t) {
  sqlite3_bind_text(track, 1, items[0].event.track_uid, -1, SQLITE_STATIC);
  int found = sqlite3_step(track) == SQLITE_ROW;
  int64_t duration_ms = found ? sqlite3_column_int64(track, 1) : 0;
  memset(t, 0, sizeof *t);
  snprintf(t->uid, sizeof t->uid, "%s", items[0].event.track_uid);
  for (int i = 0; i < n; i++) {
    const mc_play_event *e = &items[i].event;
    if (!played_for_real(e, duration_ms)) continue;
    t->heard_ms = newest(t->heard_ms, e->started_ms);
    if (e->context == MC_PLAY_CONTEXT_RADIO) continue;
    t->plays_a_while++;
    t->plays_lately += e->started_ms >= lately_ms;
    t->chosen_ms = newest(t->chosen_ms, e->started_ms);
  }
  const char *album = found ? (const char *)sqlite3_column_text(track, 0) : NULL;
  if (album && t->heard_ms) credit(s, cap, MC_RECENT_ALBUM, album, t);
  sqlite3_reset(track);
  return t->heard_ms != 0;
}

/* Credits the playlists holding tally's tracks, tally being sorted. One pass over the entries, as
 * they aren't indexed by track; a track that's in a playlist twice is credited once. */
static void credit_playlists(sources *s, int *cap, const track_tally *tally, int n) {
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_library_db(s->lib), "SELECT DISTINCT playlist_uid, track_uid FROM playlist_entries", -1,
                         &stmt, NULL) != SQLITE_OK)
    return;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    track_tally key;
    snprintf(key.uid, sizeof key.uid, "%s", (const char *)sqlite3_column_text(stmt, 1));
    const track_tally *t = bsearch(&key, tally, (size_t)n, sizeof *tally, tally_cmp);
    if (t) credit(s, cap, MC_RECENT_PLAYLIST, (const char *)sqlite3_column_text(stmt, 0), t);
  }
  sqlite3_finalize(stmt);
}

/* The real plays since A_WHILE_DAYS ago, into s->heard: a bounded read however long the log gets.
 * A track is credited to its album and playlists as they are now, not by the row's context_uid,
 * so it doesn't matter how it was played or if that album has been merged away since. No upper
 * limit, as another device's clock may be ahead. */
static void read_log(sources *s, int *cap) {
  mc_play_log *log = NULL;
  if (mc_library_list_play_events(s->lib, s->now_ms - A_WHILE_DAYS * DAY_MS, INT64_MAX, &log) != MC_OK) return;
  sqlite3_stmt *track = NULL;
  track_tally *tally = log->count ? malloc((size_t)log->count * sizeof *tally) : NULL;
  if (tally && sqlite3_prepare_v2(mc_library_db(s->lib), "SELECT album_uid, duration_ms FROM tracks WHERE uid = ?1", -1,
                                  &track, NULL) == SQLITE_OK) {
    qsort(log->items, (size_t)log->count, sizeof *log->items, event_track_cmp);
    int tracks = 0, run;
    int64_t lately_ms = s->now_ms - LATELY_DAYS * DAY_MS;
    for (int i = 0; i < log->count; i += run) {
      for (run = 1; i + run < log->count && !event_track_cmp(&log->items[i], &log->items[i + run]); run++) {}
      tracks += tally_track(s, cap, track, &log->items[i], run, lately_ms, &tally[tracks]);
    }
    credit_playlists(s, cap, tally, tracks);
  }
  sqlite3_finalize(track);
  free(tally);
  mc_play_log_free(log);
}

/* When the log began: its oldest row of any kind, as a skip shows it was running as well as a
 * play does. INT64_MAX with no log, so every last play is from before it. */
static int64_t log_began_ms(mc_library *lib) {
  sqlite3_stmt *stmt;
  int64_t began = INT64_MAX;
  if (sqlite3_prepare_v2(mc_library_db(lib), "SELECT min(started_ms) FROM play_events", -1, &stmt, NULL) != SQLITE_OK)
    return began;
  if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL)
    began = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  return began;
}

/* What track_plays says of everything played, the guess only where the log can't see. */
static void read_track_plays(sources *s, int *cap) {
  mc_played_list *played = NULL;
  if (mc_library_list_played(s->lib, s->now_ms - LATELY_DAYS * DAY_MS, &played) != MC_OK) return;
  int64_t began = log_began_ms(s->lib);
  for (int i = 0; i < played->count; i++) {
    const mc_played_info *p = &played->items[i];
    heard *h = new_heard(s, cap, p->kind, p->uid);
    if (!h) break;
    h->tracks_played = p->tracks_played;
    h->heard_ms = p->last_played_ms;
    if (p->last_played_ms >= began) continue;
    h->guess_tracks = p->tracks_since;
    h->guess_ms = p->last_played_ms;
  }
  mc_played_list_free(played);
}

static int heard_uid_cmp(const void *a, const void *b) {
  const heard *x = a, *y = b;
  int c = strcmp(x->uid, y->uid);
  return c ? c : (int)x->kind - (int)y->kind;
}

static lately_tier tier_of(const heard *h, int64_t a_while_ms) {
  if (h->plays_lately >= LATELY_PLAYS) return LATELY_REPEATED;
  if (h->guess_tracks >= LATELY_TRACKS) return LATELY_GUESS_MANY;
  if (h->plays_lately > 0) return LATELY_ANY;
  if (h->guess_tracks > 0) return LATELY_GUESS_ANY;
  if (h->plays_a_while > 0) return A_WHILE_ANY;
  if (h->guess_ms >= a_while_ms) return A_WHILE_GUESS_ANY;
  return LATELY_NONE;
}

/* Adds up each album's and playlist's entries, one for each of its tracks in the log and one from
 * track_plays, into one. */
static void fold_heard(sources *s) {
  if (!s->heard_count) return;
  qsort(s->heard, (size_t)s->heard_count, sizeof *s->heard, heard_uid_cmp);
  int n = 1;
  for (int i = 1; i < s->heard_count; i++) {
    const heard *h = &s->heard[i];
    heard *to = &s->heard[n - 1];
    if (heard_uid_cmp(to, h)) {
      s->heard[n++] = *h;
      continue;
    }
    to->plays_lately += h->plays_lately;
    to->plays_a_while += h->plays_a_while;
    to->chosen_ms = newest(to->chosen_ms, h->chosen_ms);
    to->guess_tracks += h->guess_tracks;
    to->guess_ms = newest(to->guess_ms, h->guess_ms);
    to->tracks_played += h->tracks_played;
    to->heard_ms = newest(to->heard_ms, h->heard_ms);
  }
  s->heard_count = n;
  for (int i = 0; i < n; i++) s->heard[i].tier = tier_of(&s->heard[i], s->now_ms - A_WHILE_DAYS * DAY_MS);
}

static void load_sources(mc_library *lib, unsigned seed, sources *s) {
  memset(s, 0, sizeof *s);
  s->lib = lib;
  s->seed = seed;
  s->now_ms = (int64_t)time(NULL) * 1000;
  mc_library_list_albums(lib, &s->albums);
  mc_library_list_artists_here(lib, &s->artists);
  mc_library_list_playlists(lib, MC_LIST_PLAYABLE, &s->playlists);
  mc_library_list_album_groups(lib, MC_LIST_PLAYABLE, &s->groups);
  mc_library_list_smart(lib, MC_LIST_PLAYABLE, &s->smart);
  int cap = 0;
  read_log(s, &cap);
  read_track_plays(s, &cap);
  fold_heard(s);

  int albums = s->albums ? s->albums->count : 0, artists = s->artists ? s->artists->count : 0;
  s->album_index = albums ? malloc((size_t)albums * sizeof *s->album_index) : NULL;
  s->artist_index = artists ? malloc((size_t)artists * sizeof *s->artist_index) : NULL;
  for (int i = 0; s->album_index && i < albums; i++) s->album_index[i] = (uid_entry){s->albums->items[i].uid, i};
  for (int i = 0; s->artist_index && i < artists; i++) s->artist_index[i] = (uid_entry){s->artists->items[i].uid, i};
  if (s->album_index) qsort(s->album_index, (size_t)albums, sizeof *s->album_index, entry_cmp);
  if (s->artist_index) qsort(s->artist_index, (size_t)artists, sizeof *s->artist_index, entry_cmp);
}

static void free_sources(sources *s) {
  mc_album_list_free(s->albums);
  mc_artist_list_free(s->artists);
  mc_playlist_list_free(s->playlists);
  mc_album_group_list_free(s->groups);
  mc_smart_list_free(s->smart);
  free(s->heard);
  free(s->album_index);
  free(s->artist_index);
}

/* Building a row. */

typedef struct row {
  mc_home_pick *items;
  int count;
} row;

static int in_row(const row *r, const char *uid) {
  for (int i = 0; i < r->count; i++)
    if (!strcmp(r->items[i].uid, uid)) return 1;
  return 0;
}

/* A new tile, or NULL if the row is full. */
static mc_home_pick *add(row *r, mc_recent_kind kind, const char *uid) {
  if (r->count >= MC_HOME_ROW_MAX) return NULL;
  if (!r->items) r->items = calloc(MC_HOME_ROW_MAX, sizeof *r->items);
  if (!r->items) return NULL;
  mc_home_pick *p = &r->items[r->count++];
  p->kind = kind;
  snprintf(p->uid, sizeof p->uid, "%s", uid);
  return p;
}

static const char *or_untitled(const char *name) {
  return name && *name ? name : mc_str(MC_STR_UNTITLED);
}

static void add_album(row *r, const mc_album_info *a) {
  mc_home_pick *p = add(r, MC_RECENT_ALBUM, a->uid);
  if (!p) return;
  p->title = mc_strdup(or_untitled(a->title));
  p->sub = mc_strdup(a->artist && *a->artist ? a->artist : mc_str(MC_STR_UNKNOWN_ARTIST));
  p->cover_dir = a->has_art ? mc_strdup(a->folder) : NULL;
}

static void add_playlist(row *r, const mc_playlist_info *pl) {
  mc_home_pick *p = add(r, MC_RECENT_PLAYLIST, pl->uid);
  if (!p) return;
  p->title = mc_strdup(or_untitled(pl->name));
  p->sub = mc_format_alloc(mc_str_plural(MC_STR_TRACKS, pl->track_count), MC_D(pl->track_count));
}

static void add_smart(row *r, mc_recent_kind kind, const char *uid, const char *name) {
  mc_home_pick *p = add(r, kind, uid);
  if (!p) return;
  p->smart = 1;
  p->title = mc_strdup(or_untitled(name));
  p->sub = mc_strdup(mc_str(kind == MC_RECENT_PLAYLIST ? MC_STR_SMART_PLAYLIST : MC_STR_SMART_ALBUM_GROUP));
}

/* h's album or playlist, if there's something of it to play here. */
static void add_played(row *r, const sources *s, const heard *h) {
  if (in_row(r, h->uid)) return;
  if (h->kind == MC_RECENT_ALBUM) {
    const mc_album_info *a = find_album(s, h->uid);
    if (a && a->folder) add_album(r, a);
  } else {
    const mc_playlist_info *pl = find_playlist(s, h->uid);
    if (pl) add_playlist(r, pl);
  }
}

/* A number from uid and the seed: sorting by it is a shuffle that stays put for one seed,
 * whatever else comes and goes. FNV-1a. */
static unsigned uid_hash(const char *uid, unsigned seed) {
  unsigned h = 2166136261u ^ seed;
  for (; *uid; uid++) h = (h ^ (unsigned char)*uid) * 16777619u;
  return h ^ h >> 15;
}

/* Sorted copies of what's been played. */
typedef int (*heard_cmp)(const void *, const void *);

static const heard **sorted_heard(const sources *s, heard_cmp cmp) {
  int n = s->heard_count;
  const heard **order = n ? malloc((size_t)n * sizeof *order) : NULL;
  for (int i = 0; order && i < n; i++) order[i] = &s->heard[i];
  if (order) qsort(order, (size_t)n, sizeof *order, cmp);
  return order;
}

static int newest_cmp(int64_t a, int64_t b) {
  return a < b ? 1 : a > b ? -1 : 0;
}

/* By tier, then the most plays or tracks, then the last played. */
static int lately_cmp(const void *a, const void *b) {
  const heard *x = *(const heard *const *)a, *y = *(const heard *const *)b;
  if (x->tier != y->tier) return (int)x->tier - (int)y->tier;
  if (x->plays_lately != y->plays_lately) return y->plays_lately - x->plays_lately;
  if (x->plays_a_while != y->plays_a_while) return y->plays_a_while - x->plays_a_while;
  if (x->guess_tracks != y->guess_tracks) return y->guess_tracks - x->guess_tracks;
  int c = newest_cmp(newest(x->chosen_ms, x->guess_ms), newest(y->chosen_ms, y->guess_ms));
  return c ? c : heard_uid_cmp(x, y);
}

/* Last heard longest ago first, those with no time being the oldest of all. */
static int oldest_cmp(const void *a, const void *b) {
  const heard *x = *(const heard *const *)a, *y = *(const heard *const *)b;
  int c = -newest_cmp(x->heard_ms, y->heard_ms);
  return c ? c : heard_uid_cmp(x, y);
}

/* --- The rows --- */

/* The library's stations (kithcore/radio_stations.h), each with its seed's album cover, or
 * failing that its artist's usual picture. The row stays out for a library with none. */
static void pick_radio(const sources *s, row *r) {
  mc_radio_station_list *list = NULL;
  if (mc_radio_stations_list(s->lib, s->now_ms, &list) != MC_OK) return;
  for (int i = 0; i < list->count; i++) {
    const mc_radio_station_info *st = &list->items[i];
    mc_home_pick *p = add(r, MC_RECENT_ARTIST, st->artist_uid);
    if (!p) break;
    const mc_artist_info *artist = find_artist(s, st->artist_uid);
    p->station = 1;
    memcpy(p->seed_uid, st->seed_track, sizeof p->seed_uid);
    memcpy(p->start_uid, st->start_track, sizeof p->start_uid);
    p->title = mc_strdup(st->name);
    p->sub = mc_strdup(st->sub);
    p->seed_title = mc_strdup(st->seed_title);
    const char *cover = st->cover_dir ? st->cover_dir : artist ? artist->cover_dir : NULL;
    p->cover_dir = cover ? mc_strdup(cover) : NULL;
  }
  mc_radio_station_list_free(list);
}

/* A station from the recents, as pick_radio() has one: its seed's album cover, or failing that
 * its artist's usual picture. */
static void add_station(const sources *s, row *r, const mc_recent_info *it) {
  mc_track_list *seed = NULL;
  mc_library_get_track(s->lib, it->uid, &seed);
  const mc_track_info *t = seed && seed->count ? &seed->items[0] : NULL;
  mc_home_pick *p = add(r, MC_RECENT_ARTIST, t ? t->artist_uid : "");
  if (p) {
    const mc_artist_info *artist = t ? find_artist(s, t->artist_uid) : NULL;
    p->station = 1;
    snprintf(p->seed_uid, sizeof p->seed_uid, "%s", it->uid);
    p->title = mc_strdup(or_untitled(it->name));
    p->sub = mc_strdup(it->sub ? it->sub : mc_str(MC_STR_HOME_STATION_SUB));
    p->seed_title = mc_strdup(it->seed_title ? it->seed_title : "");
    const char *cover = t && t->cover_dir ? t->cover_dir : artist ? artist->cover_dir : NULL;
    p->cover_dir = cover ? mc_strdup(cover) : NULL;
  }
  mc_track_list_free(seed);
}

/* As the sidebar's recents, but as many as fit: albums and playlists, stations, and artists and
 * album groups, each with its usual picture. */
static void pick_recents(const sources *s, row *r) {
  mc_recent_list *list = NULL;
  if (mc_library_list_recents(s->lib, MC_HOME_ROW_MAX, MC_LIST_PLAYABLE | MC_LIST_STATIONS, &list) != MC_OK) return;
  for (int i = 0; i < list->count; i++) {
    const mc_recent_info *it = &list->items[i];
    if (it->kind == MC_RECENT_STATION) {
      add_station(s, r, it);
      continue;
    }
    if (it->is_smart) {
      add_smart(r, it->kind, it->uid, it->name);
      continue;
    }
    const mc_album_info *album = it->kind == MC_RECENT_ALBUM ? find_album(s, it->uid) : NULL;
    const mc_playlist_info *playlist = it->kind == MC_RECENT_PLAYLIST ? find_playlist(s, it->uid) : NULL;
    if (album) {
      add_album(r, album);
    } else if (playlist) {
      add_playlist(r, playlist);
    } else { /* an artist, a group, or a single, which the album list leaves out */
      mc_home_pick *p = add(r, it->kind, it->uid);
      if (!p) break;
      p->title = mc_strdup(or_untitled(it->name));
      const mc_artist_info *artist = it->kind == MC_RECENT_ARTIST ? find_artist(s, it->uid) : NULL;
      const mc_album_group_info *group = it->kind == MC_RECENT_ALBUM_GROUP ? find_group(s, it->uid) : NULL;
      if (artist) {
        p->sub = mc_format_alloc(mc_str_plural(MC_STR_COUNT_ALBUMS, artist->album_count), MC_D(artist->album_count));
        p->cover_dir = mc_strdup(artist->cover_dir);
      } else if (group) {
        p->sub = mc_format_alloc(mc_str_plural(MC_STR_COUNT_ALBUMS, group->album_count), MC_D(group->album_count));
      } else {
        p->sub = mc_strdup(it->artist ? it->artist : "");
      }
    }
  }
  mc_recent_list_free(list);
}

/* Albums and playlists on repeat in the last LATELY_DAYS, the most plays first, then with too
 * few of them whatever else lately_tier has, in its order. See the note at the top. */
static void pick_played_lately(const sources *s, row *r) {
  const heard **order = sorted_heard(s, lately_cmp);
  int n = order ? s->heard_count : 0;
  for (int i = 0; i < n && order[i]->tier != LATELY_NONE; i++) {
    if (order[i]->tier > LATELY_GUESS_MANY && r->count >= ROW_ENOUGH) break;
    add_played(r, s, order[i]);
  }
  free(order);
}

/* Albums and playlists with plays but none in the last A_WHILE_DAYS, the longest ago first,
 * those with a few tracks played before those with one. With too few of them, those played
 * longest ago that aren't in Played a Lot Recently, down to LATELY_DAYS ago. A skip or a failure
 * is no play here either, but a station's is: see the note at the top. */
static void pick_not_played_lately(const sources *s, const row *lately, row *r) {
  const heard **order = sorted_heard(s, oldest_cmp);
  int n = order ? s->heard_count : 0;
  int64_t a_while = s->now_ms - A_WHILE_DAYS * DAY_MS, lately_ms = s->now_ms - LATELY_DAYS * DAY_MS;
  for (int i = 0; i < n && order[i]->heard_ms < a_while; i++)
    if (order[i]->tracks_played >= A_WHILE_TRACKS) add_played(r, s, order[i]);
  for (int i = 0; i < n && order[i]->heard_ms < a_while; i++) add_played(r, s, order[i]);
  for (int i = 0; r->count < ROW_ENOUGH && i < n && order[i]->heard_ms < lately_ms; i++)
    if (!in_row(lately, order[i]->uid)) add_played(r, s, order[i]);
  free(order);
}

typedef struct candidate {
  unsigned hash;
  const mc_album_info *album; /* one of these three */
  const mc_playlist_info *playlist;
  const mc_smart_info *smart;
} candidate;

static int candidate_cmp(const void *a, const void *b) {
  unsigned x = ((const candidate *)a)->hash, y = ((const candidate *)b)->hash;
  return x < y ? -1 : x > y;
}

/* Albums and playlists to play here that aren't in either of those rows; they may be in
 * Recents. The seed picks them, so they only change when the app starts. A quarter of the row
 * is kept for playlists, if there are that many, then they're dealt in with the albums. */
static void pick_random(const sources *s, const row *lately, const row *a_while, row *r) {
  int albums = s->albums ? s->albums->count : 0, playlists = s->playlists ? s->playlists->count : 0;
  int smart = s->smart ? s->smart->count : 0, n = 0;
  candidate *all = albums + playlists + smart ? calloc((size_t)(albums + playlists + smart), sizeof *all) : NULL;
  if (!all) return;
  for (int i = 0; i < albums + playlists + smart; i++) {
    const mc_album_info *a = i < albums ? &s->albums->items[i] : NULL;
    const mc_playlist_info *p = !a && i < albums + playlists ? &s->playlists->items[i - albums] : NULL;
    const mc_smart_info *sm = !a && !p ? &s->smart->items[i - albums - playlists] : NULL;
    if ((a && !a->folder) || (p && !p->track_count) || (sm && sm->subject != MC_SMART_TRACKS)) continue;
    const char *uid = a ? a->uid : p ? p->uid : sm->uid;
    if (in_row(lately, uid) || in_row(a_while, uid)) continue;
    all[n++] = (candidate){uid_hash(uid, s->seed), a, p, sm};
  }
  qsort(all, (size_t)n, sizeof *all, candidate_cmp);

  /* The chosen go to the front of all, in order of their hashes again for dealing out. */
  int chosen = 0, lists = 0;
  for (int i = 0; i < n && lists < RANDOM_PLAYLISTS; i++) {
    if (all[i].album) continue;
    candidate c = all[i];
    memmove(&all[chosen + 1], &all[chosen], (size_t)(i - chosen) * sizeof *all);
    all[chosen++] = c;
    lists++;
  }
  chosen = n < MC_HOME_ROW_MAX ? n : MC_HOME_ROW_MAX;
  qsort(all, (size_t)chosen, sizeof *all, candidate_cmp);
  for (int i = 0; i < chosen; i++) {
    if (all[i].album) add_album(r, all[i].album);
    else if (all[i].playlist) add_playlist(r, all[i].playlist);
    else add_smart(r, MC_RECENT_PLAYLIST, all[i].smart->uid, all[i].smart->name);
  }
  free(all);
}

enum mc_str_id mc_home_row_title(mc_home_row_id id) {
  switch (id) {
    case MC_HOME_ROW_RADIO: return MC_STR_HOME_ROW_RADIO;
    case MC_HOME_ROW_RECENTS: return MC_STR_SECTION_RECENTS;
    case MC_HOME_ROW_PLAYED_LATELY: return MC_STR_HOME_ROW_PLAYED_LATELY;
    case MC_HOME_ROW_NOT_PLAYED_LATELY: return MC_STR_HOME_ROW_NOT_PLAYED_LATELY;
    default: return MC_STR_HOME_ROW_RANDOM;
  }
}

void mc_home_picks_load(mc_library *lib, unsigned seed, mc_home_picks *out) {
  memset(out, 0, sizeof *out);
  if (!lib) return;
  sources s;
  load_sources(lib, seed, &s);

  row rows[MC_HOME_ROW_COUNT] = {0};
  pick_radio(&s, &rows[MC_HOME_ROW_RADIO]);
  pick_recents(&s, &rows[MC_HOME_ROW_RECENTS]);
  pick_played_lately(&s, &rows[MC_HOME_ROW_PLAYED_LATELY]);
  pick_not_played_lately(&s, &rows[MC_HOME_ROW_PLAYED_LATELY], &rows[MC_HOME_ROW_NOT_PLAYED_LATELY]);
  pick_random(&s, &rows[MC_HOME_ROW_PLAYED_LATELY], &rows[MC_HOME_ROW_NOT_PLAYED_LATELY], &rows[MC_HOME_ROW_RANDOM]);
  for (int i = 0; i < MC_HOME_ROW_COUNT; i++) {
    out->rows[i] = rows[i].items;
    out->counts[i] = rows[i].count;
    out->has_music |= i != MC_HOME_ROW_RADIO && rows[i].count > 0;
  }
  out->has_plays = s.heard_count > 0;
  free_sources(&s);
}

void mc_home_picks_free(mc_home_picks *picks) {
  for (int i = 0; i < MC_HOME_ROW_COUNT; i++) {
    for (int k = 0; k < picks->counts[i]; k++) {
      mc_free(picks->rows[i][k].title);
      mc_free(picks->rows[i][k].sub);
      mc_free(picks->rows[i][k].seed_title);
      mc_free(picks->rows[i][k].cover_dir);
    }
    free(picks->rows[i]);
  }
  memset(picks, 0, sizeof *picks);
}

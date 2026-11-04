#include "home_picks.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>

#include "wstr.h"

/* The guesses, until the play event log can say what was really played when. track_plays keeps
 * one count per track for all time and the time of its last play, so "played a lot recently"
 * can only mean "many of its tracks had their last play lately": a track played once this week
 * after a hundred plays years ago looks the same as one on repeat. mc_played_info.plays isn't
 * windowed either, so it only ranks the artists for Radio, which is about all time anyway. */
enum {
  LATELY_DAYS = 14,    /* a track last played this recently counts towards Played a Lot Recently */
  LATELY_TRACKS = 3,   /* how many of them make an album or playlist belong there */
  A_WHILE_DAYS = 90,   /* nothing played this recently, for Not Played in a While */
  A_WHILE_TRACKS = 2,  /* tracks ever played: fewer is more likely a stray track from shuffle */
  ROW_ENOUGH = 8,      /* fewer than this in a row and its rules relax */
  RANDOM_PLAYLISTS = HOME_ROW_MAX / 4, /* albums would crowd them out, there being many more */
};

#define DAY_MS (24 * 60 * 60 * 1000LL)

/* What the rows are picked from, listed once per load. */
typedef struct uid_entry {
  const char *uid;
  int index;
} uid_entry;

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
  mc_played_list *played;
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
  mc_library_list_played(lib, s->now_ms - LATELY_DAYS * DAY_MS, &s->played);

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
  mc_played_list_free(s->played);
  free(s->album_index);
  free(s->artist_index);
}

/* Building a row. */

typedef struct row {
  home_pick *items;
  int count;
} row;

static BOOL in_row(const row *r, const char *uid) {
  for (int i = 0; i < r->count; i++)
    if (!strcmp(r->items[i].uid, uid)) return TRUE;
  return FALSE;
}

/* A new tile, or NULL if the row is full. */
static home_pick *add(row *r, mc_recent_kind kind, const char *uid) {
  if (r->count >= HOME_ROW_MAX) return NULL;
  if (!r->items) r->items = calloc(HOME_ROW_MAX, sizeof *r->items);
  if (!r->items) return NULL;
  home_pick *p = &r->items[r->count++];
  p->kind = kind;
  snprintf(p->uid, sizeof p->uid, "%s", uid);
  return p;
}

static const char *or_untitled(const char *name) {
  return name && *name ? name : mc_str(MC_STR_UNTITLED);
}

static void add_album(row *r, const mc_album_info *a) {
  home_pick *p = add(r, MC_RECENT_ALBUM, a->uid);
  if (!p) return;
  p->title = wstr_dup_from_utf8(or_untitled(a->title));
  p->sub = wstr_dup_from_utf8(a->artist && *a->artist ? a->artist : mc_str(MC_STR_UNKNOWN_ARTIST));
  p->cover_dir = a->has_art ? wstr_dup_from_utf8(a->folder) : NULL;
}

static void add_playlist(row *r, const mc_playlist_info *pl) {
  home_pick *p = add(r, MC_RECENT_PLAYLIST, pl->uid);
  if (!p) return;
  wchar_t sub[64];
  mc_wformat(sub, 64, mc_str_plural(MC_STR_TRACKS, pl->track_count), MC_D(pl->track_count));
  p->title = wstr_dup_from_utf8(or_untitled(pl->name));
  p->sub = _wcsdup(sub);
}

static void add_smart(row *r, mc_recent_kind kind, const char *uid, const char *name) {
  home_pick *p = add(r, kind, uid);
  if (!p) return;
  p->smart = TRUE;
  p->title = wstr_dup_from_utf8(or_untitled(name));
  p->sub = _wcsdup(mc_wstr(kind == MC_RECENT_PLAYLIST ? MC_STR_SMART_PLAYLIST : MC_STR_SMART_ALBUM_GROUP));
}

/* played's album or playlist, if there's something of it to play here. */
static void add_played(row *r, const sources *s, const mc_played_info *p) {
  if (in_row(r, p->uid)) return;
  if (p->kind == MC_RECENT_ALBUM) {
    const mc_album_info *a = find_album(s, p->uid);
    if (a && a->folder) add_album(r, a);
  } else {
    const mc_playlist_info *pl = find_playlist(s, p->uid);
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
typedef int (*played_cmp)(const void *, const void *);

static const mc_played_info **sorted_played(const sources *s, played_cmp cmp) {
  int n = s->played ? s->played->count : 0;
  const mc_played_info **order = n ? malloc((size_t)n * sizeof *order) : NULL;
  for (int i = 0; order && i < n; i++) order[i] = &s->played->items[i];
  if (order) qsort(order, (size_t)n, sizeof *order, cmp);
  return order;
}

static int newest_cmp(int64_t a, int64_t b) {
  return a < b ? 1 : a > b ? -1 : 0;
}

/* Most tracks played lately first, then the last played. */
static int lately_cmp(const void *a, const void *b) {
  const mc_played_info *x = *(const mc_played_info *const *)a, *y = *(const mc_played_info *const *)b;
  if (x->tracks_since != y->tracks_since) return y->tracks_since - x->tracks_since;
  return newest_cmp(x->last_played_ms, y->last_played_ms);
}

/* Last played longest ago first, those with no time being the oldest of all. */
static int oldest_cmp(const void *a, const void *b) {
  const mc_played_info *x = *(const mc_played_info *const *)a, *y = *(const mc_played_info *const *)b;
  return -newest_cmp(x->last_played_ms, y->last_played_ms);
}

/* --- The rows --- */

/* Placeholders until there's radio. Stations are named after the artists played most, each with
 * their usual picture; the artists with most albums make up the numbers, which is all of them
 * in a library that hasn't been played yet. */
typedef struct ranked_artist {
  const mc_artist_info *artist;
  int64_t plays;
  unsigned tie;
} ranked_artist;

static int ranked_cmp(const void *a, const void *b) {
  const ranked_artist *x = a, *y = b;
  if (x->plays != y->plays) return x->plays < y->plays ? 1 : -1;
  if (x->artist->album_count != y->artist->album_count) return y->artist->album_count - x->artist->album_count;
  return x->tie < y->tie ? -1 : x->tie > y->tie;
}

static void pick_radio(const sources *s, row *r) {
  int n = s->artists ? s->artists->count : 0;
  ranked_artist *ranked = n ? calloc((size_t)n, sizeof *ranked) : NULL;
  if (!ranked) return;
  for (int i = 0; i < n; i++) {
    ranked[i].artist = &s->artists->items[i];
    ranked[i].tie = uid_hash(s->artists->items[i].uid, s->seed);
  }
  for (int i = 0; s->played && i < s->played->count; i++) {
    const mc_played_info *p = &s->played->items[i];
    const mc_album_info *a = p->kind == MC_RECENT_ALBUM ? find_album(s, p->uid) : NULL;
    int at = a && a->artist_uid[0] ? find_entry(s->artist_index, n, a->artist_uid) : -1;
    if (at >= 0) ranked[at].plays += p->plays; /* found by index: ranked is still in list order */
  }
  qsort(ranked, (size_t)n, sizeof *ranked, ranked_cmp);
  for (int i = 0; i < n; i++) {
    const mc_artist_info *a = ranked[i].artist;
    if (!a->cover_dir) continue; /* a station needs a cover */
    home_pick *p = add(r, MC_RECENT_ARTIST, a->uid);
    if (!p) break;
    wchar_t title[256];
    mc_wformat(title, 256, mc_str(MC_STR_WINDOWS_HOME_STATION), MC_S(or_untitled(a->name)));
    p->station = TRUE;
    p->title = _wcsdup(title);
    p->sub = _wcsdup(mc_wstr(MC_STR_WINDOWS_HOME_STATION_SUB));
    p->cover_dir = wstr_dup_from_utf8(a->cover_dir);
  }
  free(ranked);
}

/* As the sidebar's recents, but as many as fit: albums and playlists, and artists and album
 * groups, each with its usual picture. */
static void pick_recents(const sources *s, row *r) {
  mc_recent_list *list = NULL;
  if (mc_library_list_recents(s->lib, HOME_ROW_MAX, MC_LIST_PLAYABLE, &list) != MC_OK) return;
  for (int i = 0; i < list->count; i++) {
    const mc_recent_info *it = &list->items[i];
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
      home_pick *p = add(r, it->kind, it->uid);
      if (!p) break;
      p->title = wstr_dup_from_utf8(or_untitled(it->name));
      wchar_t sub[64] = L"";
      const mc_artist_info *artist = it->kind == MC_RECENT_ARTIST ? find_artist(s, it->uid) : NULL;
      const mc_album_group_info *group = it->kind == MC_RECENT_ALBUM_GROUP ? find_group(s, it->uid) : NULL;
      if (artist) {
        mc_wformat(sub, 64, mc_str_plural(MC_STR_COUNT_ALBUMS, artist->album_count), MC_D(artist->album_count));
        p->cover_dir = wstr_dup_from_utf8(artist->cover_dir);
      } else if (group) {
        mc_wformat(sub, 64, mc_str_plural(MC_STR_COUNT_ALBUMS, group->album_count), MC_D(group->album_count));
      } else if (it->artist) {
        wstr_from_utf8(sub, 64, it->artist);
      }
      p->sub = _wcsdup(sub);
    }
  }
  mc_recent_list_free(list);
}

/* Albums and playlists with several tracks played in the last LATELY_DAYS, most tracks first.
 * With too few of them, those with any, then any played since A_WHILE_DAYS ago, so a quiet
 * fortnight doesn't empty the row. See the note on the guesses at the top. */
static void pick_played_lately(const sources *s, row *r) {
  const mc_played_info **order = sorted_played(s, lately_cmp);
  int n = order ? s->played->count : 0;
  for (int i = 0; i < n && order[i]->tracks_since >= LATELY_TRACKS; i++) add_played(r, s, order[i]);
  for (int i = 0; r->count < ROW_ENOUGH && i < n && order[i]->tracks_since > 0; i++) add_played(r, s, order[i]);
  int64_t since = s->now_ms - A_WHILE_DAYS * DAY_MS;
  for (int i = 0; r->count < ROW_ENOUGH && i < n; i++)
    if (order[i]->last_played_ms >= since) add_played(r, s, order[i]);
  free(order);
}

/* Albums and playlists with plays but none in the last A_WHILE_DAYS, the longest ago first,
 * those with a few tracks played before those with one. With too few of them, those played
 * longest ago that aren't in Played a Lot Recently, down to LATELY_DAYS ago. */
static void pick_not_played_lately(const sources *s, const row *lately, row *r) {
  const mc_played_info **order = sorted_played(s, oldest_cmp);
  int n = order ? s->played->count : 0;
  int64_t a_while = s->now_ms - A_WHILE_DAYS * DAY_MS, lately_ms = s->now_ms - LATELY_DAYS * DAY_MS;
  for (int i = 0; i < n && order[i]->last_played_ms < a_while; i++)
    if (order[i]->tracks_played >= A_WHILE_TRACKS) add_played(r, s, order[i]);
  for (int i = 0; i < n && order[i]->last_played_ms < a_while; i++) add_played(r, s, order[i]);
  for (int i = 0; r->count < ROW_ENOUGH && i < n && order[i]->last_played_ms < lately_ms; i++)
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
  chosen = n < HOME_ROW_MAX ? n : HOME_ROW_MAX;
  qsort(all, (size_t)chosen, sizeof *all, candidate_cmp);
  for (int i = 0; i < chosen; i++) {
    if (all[i].album) add_album(r, all[i].album);
    else if (all[i].playlist) add_playlist(r, all[i].playlist);
    else add_smart(r, MC_RECENT_PLAYLIST, all[i].smart->uid, all[i].smart->name);
  }
  free(all);
}

void home_picks_load(mc_library *lib, unsigned seed, home_picks *out) {
  memset(out, 0, sizeof *out);
  if (!lib) return;
  sources s;
  load_sources(lib, seed, &s);

  row rows[HOME_ROW_COUNT] = {0};
  pick_radio(&s, &rows[HOME_ROW_RADIO]);
  pick_recents(&s, &rows[HOME_ROW_RECENTS]);
  pick_played_lately(&s, &rows[HOME_ROW_PLAYED_LATELY]);
  pick_not_played_lately(&s, &rows[HOME_ROW_PLAYED_LATELY], &rows[HOME_ROW_NOT_PLAYED_LATELY]);
  pick_random(&s, &rows[HOME_ROW_PLAYED_LATELY], &rows[HOME_ROW_NOT_PLAYED_LATELY], &rows[HOME_ROW_RANDOM]);
  for (int i = 0; i < HOME_ROW_COUNT; i++) {
    out->rows[i] = rows[i].items;
    out->counts[i] = rows[i].count;
    out->has_music |= i != HOME_ROW_RADIO && rows[i].count > 0;
  }
  out->has_plays = s.played && s.played->count > 0;
  free_sources(&s);
}

void home_picks_free(home_picks *picks) {
  for (int i = 0; i < HOME_ROW_COUNT; i++) {
    for (int k = 0; k < picks->counts[i]; k++) {
      free(picks->rows[i][k].title);
      free(picks->rows[i][k].sub);
      free(picks->rows[i][k].cover_dir);
    }
    free(picks->rows[i]);
  }
  memset(picks, 0, sizeof *picks);
}

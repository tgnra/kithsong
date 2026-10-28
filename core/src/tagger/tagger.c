/* Import pipeline (see docs/tagging-rules.md), all on one core-owned thread:
 *   1. walk the folders and read existing tags (tag_io.cpp)
 *   2. group files into items: albums by release id, then album tag and folder; files with
 *      no album tag are singles
 *   3. look up candidates on MusicBrainz (musicbrainz.c) and score them (match.c)
 *   4. auto-accept confident matches; leave the rest in REVIEW for the user. While
 *      MusicBrainz can't be reached, items skip 3 and go straight to REVIEW; with tagging
 *      skipped, they're imported as they are.
 *   5. on accept: compare with what the library has (dupes.c: skip duplicates, or swap in a
 *      better release), copy (or, inside the library, move) the files into the library
 *      folder (organize.c), write tags, add a cover (coverart.c), then library rows
 *      (import_db.c)
 * After matching, the thread stays alive to work through the user's review decisions.
 */
#include "musiccore/tagger.h"
#include "musiccore/i18n.h"

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "library/library_internal.h"
#include "tagger/coverart.h"
#include "tagger/dupes.h"
#include "tagger/import_db.h"
#include "tagger/match.h"
#include "tagger/musicbrainz.h"
#include "tagger/organize.h"
#include "util/platform.h"

#define DEFAULT_THRESHOLD 0.9
#define RELEASE_LOOKUPS 3   /* full release lookups per album; each costs a second */
#define RECORDING_RESULTS 5
#define PROBE_TIMEOUT_MS 3000   /* offline check: connecting to MusicBrainz */
#define PROBE_INTERVAL_MS 30000 /* while offline, how often to check again */

typedef struct file_entry {
  char *path;
  mc_tags tags;
  int64_t size;
  int64_t mtime_ms;
} file_entry;

typedef struct candidate {
  mb_release rel;
  int from_recording; /* a recording search result (rel has one track) */
  double distance;
  int missing;
  int extra;
  int *map; /* item file -> release track index, or -1 */
} candidate;

typedef struct item {
  mc_import_kind kind;
  mc_import_state state;
  char *folder;
  char *artist;
  char *album; /* album tag, or the track title for a single */
  int *files;  /* indices into mc_import.files, in disc/track order */
  int file_count;
  candidate *cands; /* best first */
  int cand_count;
  int chosen;
  char *error;
  int unreachable; /* in REVIEW because MusicBrainz couldn't be reached */
  mc_import_outcome outcome; /* guarded by lock, like state */
  int replaced, duplicates;
} item;

/* REQ_RETRY matches an item waiting on MusicBrainz again (see auto_match), once it's back or
 * tagging is skipped; it does nothing if the item has moved on meanwhile. */
typedef enum req_kind { REQ_ACCEPT, REQ_AS_IS, REQ_SKIP, REQ_SEARCH, REQ_RETRY, REQ_ASSIGN } req_kind;

typedef struct request {
  req_kind kind;
  int item;
  int match;
  int track, from_item, file; /* REQ_ASSIGN */
  char *artist;
  char *title;
  struct request *next;
} request;

struct mc_import {
  char *lib_dir;
  char **paths;
  int path_count;
  double threshold;
  int fetch_art;
  mc_import_fn callback;
  void *user;
  mc_thread thread;
  int thread_started;

  mc_mutex lock;
  mc_cond wake;
  /* Guarded by lock. */
  int cancel;
  int skip_tagging; /* import as-is from now on */
  int offline;      /* MusicBrainz was unreachable at the last check */
  uint64_t next_probe_ms; /* while offline: when to check again */
  request *head, *tail;
  /* Item state, error, chosen and candidates are guarded by lock once items are published,
   * and so are an item's files (mc_import_assign moves files between items). They change only
   * on the import thread, which can read them without the lock. Everything else about items
   * and files is written only before publishing. */
  item *items;
  int item_count;

  file_entry *files;
  int file_count, file_cap;
};

static void emit(mc_import *imp, mc_import_event ev, int arg) {
  if (imp->callback) imp->callback(imp->user, ev, arg);
}

static int cancelled(mc_import *imp) {
  mc_mutex_lock(&imp->lock);
  int c = imp->cancel;
  mc_mutex_unlock(&imp->lock);
  return c;
}

static void set_state(mc_import *imp, int idx, mc_import_state state, const char *error) {
  mc_mutex_lock(&imp->lock);
  item *it = &imp->items[idx];
  it->state = state;
  it->unreachable = 0;
  free(it->error);
  it->error = mc_strdup(error);
  mc_mutex_unlock(&imp->lock);
  emit(imp, MC_IMPORT_EV_ITEM_CHANGED, idx);
}

/* Puts an item in review because MusicBrainz couldn't be reached. */
static void set_unreachable(mc_import *imp, int idx) {
  mc_mutex_lock(&imp->lock);
  item *it = &imp->items[idx];
  it->state = MC_IMPORT_REVIEW;
  it->unreachable = 1;
  free(it->error);
  it->error = mc_strdup(mc_str(MC_STR_IMPORT_ERROR_UNREACHABLE));
  mc_mutex_unlock(&imp->lock);
  emit(imp, MC_IMPORT_EV_ITEM_CHANGED, idx);
}

static int skipping_tags(mc_import *imp) {
  mc_mutex_lock(&imp->lock);
  int skip = imp->skip_tagging;
  mc_mutex_unlock(&imp->lock);
  return skip;
}

/* --- Scan --- */

static const char *extension(const char *path) {
  const char *dot = strrchr(path, '.');
  const char *sep = strrchr(path, MC_PATH_SEP);
  const char *slash = strrchr(path, '/');
  if (slash > sep) sep = slash;
  return dot && dot > (sep ? sep : path) ? dot + 1 : "";
}

/* The lowercase extension of an audio file ("flac", "mp3", ...), or NULL for other files. */
static const char *audio_format(const char *path) {
  static const char *const exts[] = {"flac", "mp3", "m4a", "mp4", "aac", "ogg", "oga",
                                     "opus", "wav", "aif", "aiff", "wv",  "ape", "mpc",
                                     "wma",  "dsf", NULL};
  const char *ext = extension(path);
  for (const char *const *e = exts; *e; e++) {
    size_t i = 0;
    while ((*e)[i] && tolower((unsigned char)ext[i]) == (*e)[i]) i++;
    if (!(*e)[i] && !ext[i]) return *e;
  }
  return NULL;
}

static int scan_file(void *user, const char *path) {
  mc_import *imp = user;
  if (cancelled(imp)) return 1;
  if (!audio_format(path)) return 0;

  if (imp->file_count == imp->file_cap) {
    int cap = imp->file_cap ? imp->file_cap * 2 : 256;
    file_entry *grown = realloc(imp->files, (size_t)cap * sizeof *grown);
    if (!grown) return 1;
    imp->files = grown;
    imp->file_cap = cap;
  }
  file_entry *f = &imp->files[imp->file_count];
  memset(f, 0, sizeof *f);
  if (mc_tags_read(path, &f->tags) != MC_OK) return 0; /* not readable as audio: ignore */
  if (!(f->path = mc_strdup(path))) {
    mc_tags_clear(&f->tags);
    return 1;
  }
  mc_file_info(path, &f->size, &f->mtime_ms);
  imp->file_count++;
  if (imp->file_count % 25 == 0) emit(imp, MC_IMPORT_EV_SCANNING, imp->file_count);
  return 0;
}

static int compare_paths(const void *a, const void *b) {
  return strcmp(((const file_entry *)a)->path, ((const file_entry *)b)->path);
}

/* --- Grouping --- */

static const char *last_sep(const char *path) {
  const char *a = strrchr(path, '/'), *b = strrchr(path, '\\');
  return a > b ? a : b;
}

static char *dirname_of(const char *path) {
  const char *sep = last_sep(path);
  if (!sep) return mc_strdup("");
  size_t n = (size_t)(sep - path);
  char *d = malloc(n + 1);
  if (d) {
    memcpy(d, path, n);
    d[n] = '\0';
  }
  return d;
}

/* "CD1", "Disc 2", "disk_03": a disc folder inside an album folder. */
static int is_disc_folder(const char *name) {
  static const char *const prefixes[] = {"cd", "disc", "disk", NULL};
  for (const char *const *p = prefixes; *p; p++) {
    size_t n = strlen(*p), i = 0;
    while (i < n && tolower((unsigned char)name[i]) == (*p)[i]) i++;
    if (i < n) continue;
    const char *s = name + n;
    while (*s == ' ' || *s == '_' || *s == '-' || *s == '.') s++;
    if (!isdigit((unsigned char)*s)) continue;
    while (isdigit((unsigned char)*s)) s++;
    if (!*s) return 1;
  }
  return 0;
}

/* The folder that holds an album: the file's folder, or its parent for disc folders. */
static char *album_folder(const char *path) {
  char *dir = dirname_of(path);
  if (!dir) return NULL;
  const char *sep = last_sep(dir);
  if (sep && is_disc_folder(sep + 1)) dir[sep - dir] = '\0';
  return dir;
}

static int same_text(const char *a, const char *b) {
  return a && b && mc_string_distance(a, b) == 0.0;
}

static char *singles_title(const char *artist);

/* A file we tagged as one of an artist's singles: album "<album artist> Singles". */
static int is_singles_album(const mc_tags *t) {
  const char *artist = t->album_artist ? t->album_artist : t->artist;
  if (!t->album || !artist) return 0;
  char *expected = singles_title(artist);
  int same = expected && strcmp(t->album, expected) == 0;
  free(expected);
  return same;
}

/* Most frequent non-empty value of a tag field across an item's files, and whether every
 * file that has the field agrees. */
static const char *common_tag(mc_import *imp, const int *files, int n, size_t offset, int *all_same) {
  const char *best = NULL;
  int best_count = 0;
  *all_same = 1;
  for (int i = 0; i < n; i++) {
    const char *v = *(char **)((char *)&imp->files[files[i]].tags + offset);
    if (!v) continue;
    int count = 0;
    for (int j = 0; j < n; j++) {
      const char *w = *(char **)((char *)&imp->files[files[j]].tags + offset);
      if (w && strcmp(v, w) == 0) count++;
      else if (w) *all_same = 0;
    }
    if (count > best_count) best = v, best_count = count;
  }
  return best;
}

static const mc_tags *item_tags(mc_import *imp, const item *it, int i) {
  return &imp->files[it->files[i]].tags;
}

/* Title (and maybe artist) from a file name like "03 - Artist - Title.mp3", for files
 * without tags. */
static void guess_from_filename(const char *path, char **artist, char **title) {
  const char *sep = last_sep(path);
  const char *name = sep ? sep + 1 : path;
  const char *ext = strrchr(name, '.');
  size_t len = ext ? (size_t)(ext - name) : strlen(name);
  char *stem = malloc(len + 1);
  if (!stem) return;
  memcpy(stem, name, len);
  stem[len] = '\0';

  char *s = stem;
  while (isdigit((unsigned char)*s)) s++; /* leading track number */
  if (s != stem && (*s == ' ' || *s == '.' || *s == '-' || *s == '_')) {
    while (*s == ' ' || *s == '.' || *s == '-' || *s == '_') s++;
  } else {
    s = stem;
  }
  char *dash = strstr(s, " - ");
  if (dash && !*artist) {
    *dash = '\0';
    *artist = mc_strdup(s);
    s = dash + 3;
  }
  if (*s) *title = mc_strdup(s);
  free(stem);
}

static int file_order(const file_entry *a, const file_entry *b) {
  if (a->tags.disc != b->tags.disc) return a->tags.disc < b->tags.disc ? -1 : 1;
  if (a->tags.track != b->tags.track) return a->tags.track < b->tags.track ? -1 : 1;
  return strcmp(a->path, b->path);
}

/* Insertion sort of an item's files by disc, track, path; items are small. */
static void sort_item_files(mc_import *imp, item *it) {
  for (int i = 1; i < it->file_count; i++) {
    int v = it->files[i], j = i;
    while (j > 0 && file_order(&imp->files[it->files[j - 1]], &imp->files[v]) > 0) {
      it->files[j] = it->files[j - 1];
      j--;
    }
    it->files[j] = v;
  }
}

typedef struct item_list {
  item *items;
  int count, cap;
} item_list;

static item *add_item(item_list *l, mc_import_kind kind, const char *folder) {
  if (l->count == l->cap) {
    int n = l->cap ? l->cap * 2 : 64;
    item *grown = realloc(l->items, (size_t)n * sizeof *grown);
    if (!grown) return NULL;
    l->items = grown;
    l->cap = n;
  }
  item *it = &l->items[l->count++];
  memset(it, 0, sizeof *it);
  it->kind = kind;
  it->chosen = -1;
  it->folder = mc_strdup(folder);
  return it;
}

/* Builds the items into *out, which the caller publishes once complete. */
static mc_status group_files(mc_import *imp, item_list *out) {
  int n = imp->file_count;
  char **keys = calloc((size_t)n + 1, sizeof *keys);
  int *item_of = malloc(((size_t)n + 1) * sizeof *item_of);
  mc_status st = MC_ERR_NOMEM;
  if (!keys || !item_of) goto done;

  /* Key each file by album folder + album tag (or release id if there's no album tag).
   * Files with neither are singles, and so are files we filed under "<artist> Singles"
   * before. The folder keeps two copies of one album apart. */
  for (int i = 0; i < n; i++) {
    const mc_tags *t = &imp->files[i].tags;
    char *folder = album_folder(imp->files[i].path);
    if (!folder) goto done;
    if ((t->album || t->mb_albumid) && !is_singles_album(t)) {
      const char *v = t->album ? t->album : t->mb_albumid;
      size_t len = strlen(folder) + strlen(v) + 4;
      if ((keys[i] = malloc(len))) {
        snprintf(keys[i], len, "%s\x1f%s:%s", folder, t->album ? "al" : "id", v);
        for (char *k = keys[i]; *k; k++) *k = (char)tolower((unsigned char)*k);
      }
    }
    free(folder);
    item_of[i] = -1;
  }

  for (int i = 0; i < n; i++) {
    if (item_of[i] >= 0) continue;
    char *folder = keys[i] ? album_folder(imp->files[i].path) : dirname_of(imp->files[i].path);
    item *it = add_item(out, keys[i] ? MC_IMPORT_ALBUM : MC_IMPORT_SINGLE, folder);
    free(folder);
    st = MC_ERR_NOMEM;
    if (!it) goto done;
    int idx = out->count - 1;

    int members = 1;
    if (keys[i])
      for (int j = i + 1; j < n; j++) members += keys[j] && strcmp(keys[i], keys[j]) == 0;
    if (!(it->files = malloc((size_t)members * sizeof *it->files))) {
      free(it->folder);
      out->count--;
      goto done;
    }
    for (int j = i; j < n && it->file_count < members; j++) {
      if (j == i || (keys[i] && keys[j] && strcmp(keys[i], keys[j]) == 0)) {
        it->files[it->file_count++] = j;
        item_of[j] = idx;
      }
    }
    sort_item_files(imp, it);

    const mc_tags *first = item_tags(imp, it, 0);
    /* A lone file whose album is named after the track is a single. */
    if (it->kind == MC_IMPORT_ALBUM && it->file_count == 1 && same_text(first->album, first->title))
      it->kind = MC_IMPORT_SINGLE;

    int same;
    if (it->kind == MC_IMPORT_SINGLE) {
      it->artist = mc_strdup(first->artist);
      it->album = mc_strdup(first->title);
      if (!it->album) guess_from_filename(imp->files[i].path, &it->artist, &it->album);
    } else {
      const char *aa = common_tag(imp, it->files, it->file_count, offsetof(mc_tags, album_artist), &same);
      const char *a = common_tag(imp, it->files, it->file_count, offsetof(mc_tags, artist), &same);
      it->artist = mc_strdup(aa ? aa : same ? a : "Various Artists");
      it->album = mc_strdup(common_tag(imp, it->files, it->file_count, offsetof(mc_tags, album), &same));
    }
  }
  st = MC_OK;

done:
  if (keys)
    for (int i = 0; i < n; i++) free(keys[i]);
  free(keys);
  free(item_of);
  return st;
}

/* --- Matching --- */

static void candidate_clear(candidate *c) {
  mb_release_clear(&c->rel);
  free(c->map);
}

static void candidates_free(candidate *c, int n) {
  for (int i = 0; i < n; i++) candidate_clear(&c[i]);
  free(c);
}

/* Best first. Ties go to the earliest original release, so a remaster or a compilation
 * appearance of the same song doesn't win over the original. */
static int compare_candidates(const void *a, const void *b) {
  const candidate *ca = a, *cb = b;
  if (ca->distance < cb->distance - 1e-6) return -1;
  if (ca->distance > cb->distance + 1e-6) return 1;
  int ya = ca->rel.original_year ? ca->rel.original_year : 9999;
  int yb = cb->rel.original_year ? cb->rel.original_year : 9999;
  return (ya > yb) - (ya < yb);
}

typedef struct cand_list {
  candidate *c;
  int n, cap;
  mc_status net; /* first network error, for the "why are there no candidates" message */
} cand_list;

static candidate *cand_push(cand_list *l) {
  if (l->n == l->cap) {
    int cap = l->cap ? l->cap * 2 : 8;
    candidate *grown = realloc(l->c, (size_t)cap * sizeof *grown);
    if (!grown) return NULL;
    l->c = grown;
    l->cap = cap;
  }
  candidate *c = &l->c[l->n++];
  memset(c, 0, sizeof *c);
  return c;
}

static void note_error(cand_list *l, mc_status st) {
  if (st != MC_OK && st != MC_ERR_NOT_FOUND && l->net == MC_OK) l->net = st;
}

static int have_release(const cand_list *l, const char *id) {
  for (int i = 0; i < l->n; i++)
    if (!l->c[i].from_recording && strcmp(l->c[i].rel.id, id) == 0) return 1;
  return 0;
}

static int have_recording(const cand_list *l, const char *id) {
  for (int i = 0; i < l->n; i++)
    if (l->c[i].from_recording && strcmp(l->c[i].rel.tracks[0].recording_id, id) == 0) return 1;
  return 0;
}

static void add_release(mc_import *imp, const item *it, const mc_tags *tags, const char *artist,
                        const char *album, const char *id, cand_list *l) {
  if (have_release(l, id) || cancelled(imp)) return;
  mb_release rel;
  mc_status st = mb_lookup_release(id, &rel);
  note_error(l, st);
  if (st != MC_OK) return;
  candidate *c = cand_push(l);
  int *map = malloc((size_t)it->file_count * sizeof *map);
  mc_release_score score;
  if (!c || !map ||
      mc_score_release(&rel, tags, it->file_count, album, artist, map, &score) != MC_OK) {
    if (c) l->n--;
    free(map);
    mb_release_clear(&rel);
    return;
  }
  c->rel = rel;
  c->map = map;
  c->distance = score.distance;
  c->missing = score.missing;
  c->extra = score.extra;
}

static void add_recording(mb_release *rel, const mc_tags *file, cand_list *l) {
  candidate *c = NULL;
  int *map = NULL;
  if (rel->track_count != 1 || have_recording(l, rel->tracks[0].recording_id) ||
      !(map = malloc(sizeof *map)) || !(c = cand_push(l))) {
    free(map);
    mb_release_clear(rel);
    return;
  }
  map[0] = 0;
  c->rel = *rel;
  c->from_recording = 1;
  c->map = map;
  c->distance = mc_score_recording(rel, file);
}

static void search_releases(mc_import *imp, const item *it, const mc_tags *tags,
                            const char *artist, const char *album, cand_list *l) {
  /* Files already tagged with one release: look that up first. */
  const char *tagged = tags[0].mb_albumid;
  for (int i = 1; i < it->file_count && tagged; i++)
    if (!tags[i].mb_albumid || strcmp(tags[i].mb_albumid, tagged) != 0) tagged = NULL;
  if (tagged) add_release(imp, it, tags, artist, album, tagged, l);

  if (album && !cancelled(imp)) {
    char ids[RELEASE_LOOKUPS][MC_UID_STRLEN];
    int n = 0;
    const char *search_artist = artist && mc_string_distance(artist, "Various Artists") != 0.0 ? artist : NULL;
    mc_status st = mb_search_releases(search_artist, album, it->file_count, ids, RELEASE_LOOKUPS, &n);
    note_error(l, st);
    for (int i = 0; i < n; i++) add_release(imp, it, tags, artist, album, ids[i], l);
  }
}

static void search_recordings(mc_import *imp, const mc_tags *file, const char *artist,
                              const char *title, cand_list *l) {
  mb_release rel;
  if (file->mb_trackid && !cancelled(imp)) {
    mc_status st = mb_lookup_recording(file->mb_trackid, &rel);
    note_error(l, st);
    if (st == MC_OK) add_recording(&rel, file, l);
  }
  if (title && !cancelled(imp)) {
    mb_release found[RECORDING_RESULTS];
    int n = 0;
    mc_status st = mb_search_recordings(artist, title, found, RECORDING_RESULTS, &n);
    note_error(l, st);
    for (int i = 0; i < n; i++) add_recording(&found[i], file, l);
  }
}

static int should_accept(const item *it, const candidate *c, double threshold) {
  if (1.0 - c->distance < threshold) return 0;
  /* Never auto-accept a partial match: missing tracks or files that would be left out. */
  return it->kind == MC_IMPORT_SINGLE || (c->missing == 0 && c->extra == 0);
}

static void write_item(mc_import *imp, mc_library *lib, int idx, int match);

/* Finds and scores candidates for an item. artist/title override the item's own tags (a
 * search the user typed); such results are never auto-accepted. Returns nonzero if nothing
 * was found because a request failed with a network error. */
static int match_item(mc_import *imp, mc_library *lib, int idx, const char *artist,
                       const char *title) {
  item *it = &imp->items[idx];
  int manual = artist || title;
  if (!manual) {
    artist = it->artist;
    title = it->album;
  }
  set_state(imp, idx, MC_IMPORT_MATCHING, NULL);

  mc_tags *tags = malloc((size_t)it->file_count * sizeof *tags);
  if (!tags) {
    set_state(imp, idx, MC_IMPORT_FAILED, mc_str(MC_STR_IMPORT_ERROR_NOMEM));
    return 0;
  }
  for (int i = 0; i < it->file_count; i++) tags[i] = *item_tags(imp, it, i);

  /* For a single, score against the search terms where the tags have nothing (a title
   * guessed from the file name) or the user typed something else. */
  mc_tags scored = tags[0];
  if (it->kind == MC_IMPORT_SINGLE) {
    if (artist && *artist && (manual || !scored.artist)) scored.artist = (char *)artist;
    if (title && *title && (manual || !scored.title)) scored.title = (char *)title;
  }

  cand_list l = {0};
  if (it->kind == MC_IMPORT_ALBUM) {
    search_releases(imp, it, tags, artist, title, &l);
    /* A lone file could also be one track off a release: offer it as a single too. */
    if (it->file_count == 1) search_recordings(imp, &tags[0], tags[0].artist, tags[0].title, &l);
  } else {
    search_recordings(imp, &scored, artist, title, &l);
  }
  free(tags);
  if (l.n > 1) qsort(l.c, (size_t)l.n, sizeof *l.c, compare_candidates);

  int unreachable = l.n == 0 && l.net == MC_ERR_NET;
  const char *why = l.n == 0 && !unreachable ? mc_str(MC_STR_IMPORT_ERROR_NO_MATCHES) : NULL;
  int accept = !manual && l.n > 0 && should_accept(it, &l.c[0], imp->threshold);

  mc_mutex_lock(&imp->lock);
  candidates_free(it->cands, it->cand_count);
  it->cands = l.c;
  it->cand_count = l.n;
  mc_mutex_unlock(&imp->lock);

  if (accept) write_item(imp, lib, idx, 0);
  else if (unreachable) set_unreachable(imp, idx);
  else set_state(imp, idx, MC_IMPORT_REVIEW, why);
  return unreachable;
}

/* --- Writing --- */

static char *singles_title(const char *artist) {
  const char *name = artist && *artist ? artist : "Unknown Artist";
  size_t len = strlen(name) + 9;
  char *s = malloc(len);
  if (s) snprintf(s, len, "%s Singles", name);
  return s;
}

static int disc_track_total(const mb_release *rel, int disc) {
  int n = 0;
  for (int i = 0; i < rel->track_count; i++) n += rel->tracks[i].disc == disc;
  return n;
}

/* Library rows for a file, from its tags as they are now. The caller sets the paths. */
static void track_from_tags(const file_entry *f, mc_db_track *t) {
  memset(t, 0, sizeof *t);
  t->format = audio_format(f->path);
  t->size_bytes = f->size;
  t->mtime_ms = f->mtime_ms;
  t->bitrate_kbps = f->tags.bitrate_kbps;
  t->duration_ms = f->tags.duration_ms;
  t->genre = f->tags.genre;
  t->title = f->tags.title;
  t->artist.name = f->tags.artist;
  t->artist.mb_artistid = f->tags.mb_artistid;
  t->track_no = f->tags.track;
  t->disc_no = f->tags.disc;
  t->year = f->tags.year;
  t->mb_trackid = f->tags.mb_trackid;
  t->mb_releasetrackid = f->tags.mb_releasetrackid;
  t->mb_albumid = f->tags.mb_albumid;
}

/* --- Placing files in the library --- */

/* One file on its way into the library folder. */
typedef struct placement {
  file_entry *f;
  mc_tags tags; /* what to write; only used when write_tags is set */
  int write_tags;
  const char *releasetrackid;
  char *src_rel;  /* the file's current path in the library, or NULL if it's outside */
  char *dest_rel; /* where it goes, relative to the library folder */
  char *dest_abs;
  enum { OP_NONE, OP_COPIED, OP_MOVED, OP_REPLACED } op;
  /* Swapping out a library file (dupes.h): it's at old_rel, and the new one is meant for
   * final_rel, which may be old_rel itself; until the old file is gone, the new one waits
   * at dest_rel. */
  int swap;
  const char *old_rel;
  char *final_rel;
} placement;

static void placements_free(placement *p, int n) {
  for (int i = 0; i < n; i++) {
    free(p[i].src_rel);
    free(p[i].dest_rel);
    free(p[i].dest_abs);
    free(p[i].final_rel);
  }
  free(p);
}

static char *join_rel(const char *dir, const char *name) {
  if (!dir || !name) return NULL;
  size_t len = strlen(dir) + strlen(name) + 2;
  char *out = malloc(len);
  if (out) snprintf(out, len, "%s/%s", dir, name);
  return out;
}

/* File name without folder or extension, for files with no title tag. */
static char *file_stem(const char *path) {
  const char *sep = last_sep(path);
  const char *name = sep ? sep + 1 : path;
  const char *dot = strrchr(name, '.');
  size_t len = dot && dot > name ? (size_t)(dot - name) : strlen(name);
  char *s = malloc(len + 1);
  if (s) {
    memcpy(s, name, len);
    s[len] = '\0';
  }
  return s;
}

/* "name (2).ext", "name (3).ext", ... : the first that doesn't exist yet. */
static char *unique_path(const char *abs) {
  const char *sep = last_sep(abs);
  const char *dot = strrchr(abs, '.');
  if (!dot || (sep && dot < sep)) dot = abs + strlen(abs);
  size_t base = (size_t)(dot - abs), len = strlen(abs) + 16;
  char *out = malloc(len);
  for (int n = 2; out && n < 1000; n++) {
    snprintf(out, len, "%.*s (%d)%s", (int)base, abs, n, dot);
    if (!mc_file_exists(out)) return out;
  }
  free(out);
  return NULL;
}

typedef struct dir_scan {
  int others; /* entries that aren't cover images */
} dir_scan;

static void count_non_covers(void *user, const char *name, int is_dir) {
  dir_scan *s = user;
  const char *dot = strrchr(name, '.');
  int cover = !is_dir && dot && (strncmp(name, "cover.", 6) == 0) &&
              (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".png") == 0);
  if (!cover) s->others++;
}

/* Removes dir and then its parents while they're empty (or hold nothing but a cover we
 * wrote), never going above the library folder. For folders left behind by moves. */
static void prune_dirs(mc_library *lib, char *dir) {
  while (dir) {
    char *rel = mc_library_relpath(lib, dir);
    if (!rel) break; /* the library folder itself, or outside it */
    free(rel);
    dir_scan s = {0};
    if (mc_list_dir(dir, count_non_covers, &s) != MC_OK || s.others) break;
    char *cover;
    while ((cover = mc_coverart_find(dir))) {
      int removed = mc_remove(cover) == MC_OK;
      free(cover);
      if (!removed) break;
    }
    if (mc_rmdir(dir) != MC_OK) break;
    char *parent = dirname_of(dir);
    free(dir);
    dir = parent;
  }
  free(dir);
}

/* Puts each file at its destination: a copy for files from outside the library, a move for
 * files already in it. */
static const char *place_files(mc_import *imp, mc_library *lib, placement *p, int n) {
  for (int i = 0; i < n; i++) {
    placement *pl = &p[i];
    const char *src = pl->f->path;
    pl->src_rel = mc_library_relpath(lib, src);
    pl->dest_abs = mc_library_abspath(lib, pl->dest_rel);
    if (!pl->dest_abs) return mc_str(MC_STR_IMPORT_ERROR_NOMEM);
    if (strcmp(src, pl->dest_abs) == 0) continue; /* already in place */

    char *dir = dirname_of(pl->dest_abs);
    mc_status st = dir ? mc_mkdirs(dir) : MC_ERR_NOMEM;
    free(dir);
    if (st != MC_OK) return mc_str(MC_STR_IMPORT_ERROR_MKDIR);

    if (mc_same_path(src, pl->dest_abs)) {
      /* Only the letter case differs: rename in place. */
      if (mc_rename(src, pl->dest_abs, 0) != MC_OK) return mc_str(MC_STR_IMPORT_ERROR_RENAME);
      pl->op = OP_MOVED;
      continue;
    }
    if (mc_file_exists(pl->dest_abs)) {
      if (!pl->swap && !pl->src_rel &&
          mc_import_db_is_track_at(lib, pl->dest_rel, pl->releasetrackid)) {
        /* The same track imported again from elsewhere: replace the old copy. */
        if (mc_copy_file(src, pl->dest_abs, 1) != MC_OK) return mc_str(MC_STR_IMPORT_ERROR_COPY);
        pl->op = OP_REPLACED;
        continue;
      }
      char *unique = unique_path(pl->dest_abs);
      char *unique_rel = unique ? mc_library_relpath(lib, unique) : NULL;
      if (!unique_rel) {
        free(unique);
        return mc_str(MC_STR_IMPORT_ERROR_NO_NAME);
      }
      free(pl->dest_abs);
      free(pl->dest_rel);
      pl->dest_abs = unique;
      pl->dest_rel = unique_rel;
    }
    if (pl->src_rel) {
      if (mc_rename(src, pl->dest_abs, 0) != MC_OK) return mc_str(MC_STR_IMPORT_ERROR_MOVE);
      pl->op = OP_MOVED;
    } else {
      if (cancelled(imp)) return mc_str(MC_STR_IMPORT_ERROR_CANCELLED);
      if (mc_copy_file(src, pl->dest_abs, 0) != MC_OK)
        return mc_str(MC_STR_IMPORT_ERROR_COPY_FULL);
      pl->op = OP_COPIED;
    }
  }
  return NULL;
}

/* Reverses place_files after a later step failed. Replaced copies can't be restored, but
 * they held the same track. */
static void unplace_files(mc_library *lib, placement *p, int n) {
  for (int i = 0; i < n; i++) {
    placement *pl = &p[i];
    if (pl->op == OP_COPIED) mc_remove(pl->dest_abs);
    else if (pl->op == OP_MOVED) mc_rename(pl->dest_abs, pl->f->path, 0);
    if (pl->op == OP_COPIED || pl->op == OP_MOVED) prune_dirs(lib, dirname_of(pl->dest_abs));
    pl->op = OP_NONE;
  }
}

/* Makes sure the album folder has a cover: keep one that's there, else bring the one from
 * the folder the files came from, else write out a picture embedded in one of the files
 * (p, n of them), else download one. Returns the cover's hash in hash, or NULL if the album
 * has none. Failures here never fail the import. */
static const char *ensure_cover(mc_import *imp, mc_library *lib, const char *album_rel,
                                const char *source_dir, const placement *p, int n,
                                const mb_release *rel, char hash[MC_ART_HASH_LEN]) {
  char *dir = mc_library_abspath(lib, album_rel);
  if (!dir) return NULL;
  char *cover = mc_coverart_find(dir);

  char *source = !cover && source_dir ? mc_coverart_find(source_dir) : NULL;
  if (source) {
    const char *ext = strrchr(source, '.');
    size_t len = strlen(dir) + 16;
    char *dest = malloc(len);
    if (dest) {
      snprintf(dest, len, "%s%ccover%s", dir, MC_PATH_SEP,
               ext && (strcmp(ext, ".png") == 0 || strcmp(ext, ".PNG") == 0) ? ".png" : ".jpg");
      if (mc_copy_file(source, dest, 0) == MC_OK) cover = dest;
      else free(dest);
    }
    free(source);
  }

  for (int i = 0; i < n && !cover; i++)
    if (mc_coverart_save_embedded(dir, p[i].dest_abs) == MC_OK) cover = mc_coverart_find(dir);

  if (!cover && rel && imp->fetch_art && !cancelled(imp)) {
    mc_buffer img;
    const char *ext;
    if (mc_coverart_fetch(rel->id, rel->releasegroup_id, rel->artist, rel->title, &img, &ext) == MC_OK) {
      if (mc_coverart_save(dir, &img, ext) == MC_OK) cover = mc_coverart_find(dir);
      mc_buffer_free(&img);
    }
  }

  const char *result = cover && mc_coverart_hash(cover, hash) == MC_OK ? hash : NULL;
  free(cover);
  free(dir);
  return result;
}

/* Whether none of the files is in the library folder already. */
static int all_outside(mc_library *lib, const placement *p, int n) {
  for (int i = 0; i < n; i++) {
    char *rel = mc_library_relpath(lib, p[i].f->path);
    free(rel);
    if (rel) return 0;
  }
  return 1;
}

static const char *base_name(const char *rel) {
  const char *slash = strrchr(rel, '/');
  return slash ? slash + 1 : rel;
}

/* old_rel with the extension of format: where a swapped-in file goes. */
static char *swap_path(const char *old_rel, const char *format) {
  const char *name = base_name(old_rel), *dot = strrchr(name, '.');
  size_t stem = dot && dot > name ? (size_t)(dot - old_rel) : strlen(old_rel);
  size_t len = stem + strlen(format) + 2;
  char *out = malloc(len);
  if (out) snprintf(out, len, "%.*s.%s", (int)stem, old_rel, format);
  return out;
}

/* Compares the item with the library (dupes.c) and drops, retargets or redirects its files
 * to suit. Returns the new count; *planned is set when the comparison ran. */
static int apply_dupes(mc_library *lib, mc_db_album *album, mc_db_track *rows,
                       placement *places, int count, mc_dupe_plan *dp, int *planned) {
  *planned = 0;
  if (count <= 0 || !all_outside(lib, places, count)) return count;
  mc_dupe_in *in = calloc((size_t)count, sizeof *in);
  if (!in) return count;
  for (int i = 0; i < count; i++) {
    const file_entry *f = places[i].f;
    in[i].row = &rows[i];
    in[i].src_path = f->path;
    in[i].size = f->size;
    in[i].quality = (mc_audio_quality){audio_format(f->path), f->tags.bitrate_kbps,
                                       f->tags.sample_rate, f->tags.bits_per_sample};
  }
  mc_status st = mc_dupes_plan(lib, album, in, count, dp);
  free(in);
  if (st != MC_OK) return count; /* import as before */
  *planned = 1;

  if (dp->album_uid[0]) album->existing_uid = dp->album_uid;
  int kept = 0;
  for (int i = 0; i < count; i++) {
    mc_dupe_decision *d = &dp->d[i];
    placement pl = places[i];
    mc_db_track row = rows[i];
    if (d->action == MC_DUPE_SKIP) {
      free(pl.dest_rel);
      continue;
    }
    if (d->action == MC_DUPE_SWAP || d->action == MC_DUPE_ATTACH) row.track_uid = d->track_uid;
    char *dest = NULL;
    if (d->action == MC_DUPE_SWAP) {
      row.replace_file_uid = d->file_uid;
      pl.swap = 1;
      pl.old_rel = d->old_path;
      pl.final_rel = swap_path(d->old_path, audio_format(pl.f->path));
      dest = mc_strdup(pl.final_rel);
    } else if (dp->album_dir && pl.dest_rel) {
      dest = join_rel(dp->album_dir, base_name(pl.dest_rel));
    }
    if (dest || d->action == MC_DUPE_SWAP) {
      free(pl.dest_rel);
      pl.dest_rel = dest; /* NULL when out of memory, which the caller reports */
    }
    places[kept] = pl;
    rows[kept] = row;
    kept++;
  }
  return kept;
}

/* After the rows are saved: the swapped-out files go to the trash, and new files waiting
 * next to them take their names. A file that can't be trashed stays where it is, no longer
 * in the library (sync's reconcile treats it as an unknown file). */
static void finish_swaps(mc_library *lib, placement *p, int n) {
  for (int i = 0; i < n; i++)
    if (p[i].swap) mc_dupes_finish_swap(lib, p[i].old_rel, p[i].final_rel, &p[i].dest_rel, &p[i].dest_abs);
}

/* Applies a candidate (or the existing tags, if match < 0): files are copied or moved into
 * place, tagged, given a cover, and only then recorded in the library. A failure at any step
 * puts the files back. Runs on the import thread. */
static void write_item(mc_import *imp, mc_library *lib, int idx, int match) {
  item *it = &imp->items[idx];
  mc_mutex_lock(&imp->lock);
  it->chosen = match;
  it->outcome = MC_IMPORT_OUTCOME_NONE;
  it->replaced = it->duplicates = 0;
  mc_mutex_unlock(&imp->lock);
  set_state(imp, idx, MC_IMPORT_WRITING, NULL);

  /* Candidates only change on this thread, so they're stable for the whole write. */
  const candidate *c = match >= 0 ? &it->cands[match] : NULL;
  const mb_release *rel = c ? &c->rel : NULL;
  const char *error = NULL;
  char hash[MC_ART_HASH_LEN];
  char *singles = NULL, *album_rel = NULL, *source_dir = NULL;
  mc_db_track *rows = calloc((size_t)it->file_count, sizeof *rows);
  placement *places = calloc((size_t)it->file_count, sizeof *places);
  int count = 0;
  if (!rows || !places) {
    free(rows);
    free(places);
    set_state(imp, idx, MC_IMPORT_FAILED, mc_str(MC_STR_IMPORT_ERROR_NOMEM));
    return;
  }
  /* A cover next to the original files is worth keeping. */
  source_dir = dirname_of(imp->files[it->files[0]].path);

  mc_db_album album = {0};
  if (rel) {
    album.singles = rel->is_single;
    if (rel->is_single) {
      album.artist = (mc_db_artist){rel->primary_artist, rel->primary_artist_sort, rel->primary_artist_id};
      singles = singles_title(rel->primary_artist);
      album_rel = mc_album_dir(rel->primary_artist, singles, 0);
    } else {
      album.title = rel->title;
      album.artist = (mc_db_artist){rel->artist, rel->artist_sort, rel->artist_id};
      album.year = rel->year;
      album.original_year = rel->original_year;
      album.disc_total = rel->disc_total;
      album.mb_albumid = rel->id;
      album.mb_releasegroupid = rel->releasegroup_id;
      album_rel = mc_album_dir(rel->artist, rel->title, rel->year);
    }

    for (int i = 0; i < it->file_count; i++) {
      if (c->map[i] < 0) continue; /* left untouched; the UI shows it as unmatched */
      file_entry *f = &imp->files[it->files[i]];
      const mb_track *tr = &rel->tracks[c->map[i]];
      placement *pl = &places[count];
      mc_db_track *t = &rows[count];
      count++;

      mc_tags tags = {0};
      tags.title = tr->title;
      tags.artist = tr->artist;
      tags.mb_trackid = tr->recording_id;
      tags.mb_releasetrackid = tr->id;
      tags.mb_artistid = tr->artist_id ? tr->artist_id : "";
      tags.original_year = rel->original_year;
      if (rel->is_single) {
        /* The album tag says "<artist> Singles", so drop ids that point at another release. */
        tags.album = singles;
        tags.album_artist = rel->primary_artist;
        tags.mb_albumartistid = rel->primary_artist_id ? rel->primary_artist_id : "";
        tags.mb_albumid = "";
        tags.mb_releasegroupid = "";
        tags.year = rel->original_year ? rel->original_year : rel->year;
      } else {
        tags.album = rel->title;
        tags.album_artist = rel->artist;
        tags.mb_albumartistid = rel->artist_id ? rel->artist_id : "";
        tags.mb_albumid = rel->id;
        tags.mb_releasegroupid = rel->releasegroup_id;
        tags.year = rel->year;
        tags.track = tr->number;
        tags.track_total = disc_track_total(rel, tr->disc);
        tags.disc = tr->disc;
        tags.disc_total = rel->disc_total;
      }
      pl->f = f;
      pl->tags = tags;
      pl->write_tags = 1;
      pl->releasetrackid = tr->id;
      char *name = rel->is_single ? mc_track_file(tr->title, 0, 0, 0, audio_format(f->path))
                                  : mc_track_file(tr->title, tr->disc, rel->disc_total, tr->number,
                                                  audio_format(f->path));
      pl->dest_rel = join_rel(album_rel, name);
      free(name);

      track_from_tags(f, t);
      t->title = tr->title;
      t->artist = (mc_db_artist){tr->artist, NULL, tr->artist_id};
      t->track_no = rel->is_single ? 0 : tr->number;
      t->disc_no = rel->is_single ? 0 : tr->disc;
      t->year = tags.year;
      t->mb_trackid = tr->recording_id;
      t->mb_releasetrackid = tr->id;
      t->mb_albumid = rel->id;
    }
  } else {
    /* As-is: file the tracks under their existing tags. */
    const mc_tags *first = item_tags(imp, it, 0);
    album.singles = it->kind == MC_IMPORT_SINGLE || !first->album;
    album.title = first->album;
    album.artist.name = first->album_artist ? first->album_artist : first->artist ? first->artist : it->artist;
    album.artist.mb_artistid = first->mb_albumartistid;
    album.year = first->year;
    album.original_year = first->original_year;
    album.disc_total = first->disc_total;
    album.mb_albumid = first->mb_albumid;
    album.mb_releasegroupid = first->mb_releasegroupid;
    if (album.singles) singles = singles_title(album.artist.name);
    album_rel = album.singles ? mc_album_dir(album.artist.name, singles, 0)
                              : mc_album_dir(album.artist.name, album.title, album.year);

    for (int i = 0; i < it->file_count; i++, count++) {
      file_entry *f = &imp->files[it->files[i]];
      mc_db_track *t = &rows[count];
      track_from_tags(f, t);
      if (!t->title && it->kind == MC_IMPORT_SINGLE) t->title = it->album; /* from the file name */
      char *stem = t->title ? NULL : file_stem(f->path);
      char *name = album.singles
                     ? mc_track_file(t->title ? t->title : stem, 0, 0, 0, audio_format(f->path))
                     : mc_track_file(t->title ? t->title : stem, f->tags.disc, f->tags.disc_total,
                                     f->tags.track, audio_format(f->path));
      free(stem);
      places[count].f = f;
      places[count].dest_rel = join_rel(album_rel, name);
      free(name);
    }
  }
  if (count == 0) error = mc_str(MC_STR_IMPORT_ERROR_NO_FILES);

  /* Already in the library? Skip, or swap in the better release (see tagger.h). */
  mc_dupe_plan dp = {0};
  int planned = 0;
  if (!error) count = apply_dupes(lib, &album, rows, places, count, &dp, &planned);
  int all_dupes = planned && count == 0;
  const char *cover_dir = planned && dp.album_dir ? dp.album_dir : album_rel;

  /* join_rel and the path builders return NULL only when out of memory. */
  for (int i = 0; i < count && !error; i++)
    if (!places[i].dest_rel || (places[i].swap && !places[i].final_rel))
      error = mc_str(MC_STR_IMPORT_ERROR_NOMEM);
  if (!error && !all_dupes) error = place_files(imp, lib, places, count);

  for (int i = 0; i < count && !error; i++) {
    if (places[i].write_tags && mc_tags_write(places[i].dest_abs, &places[i].tags) != MC_OK)
      error = mc_str(MC_STR_IMPORT_ERROR_WRITE_TAGS);
  }

  if (!error && !all_dupes) {
    if (!album.singles) album.art_hash = ensure_cover(imp, lib, cover_dir, source_dir, places, count, rel, hash);
    for (int i = 0; i < count; i++) {
      mc_db_track *t = &rows[i];
      t->path = places[i].dest_rel;
      t->old_path = places[i].op == OP_MOVED ? places[i].src_rel : NULL;
      mc_file_info(places[i].dest_abs, &t->size_bytes, &t->mtime_ms);
    }
    if (mc_import_db_write(lib, &album, rows, count) != MC_OK) error = mc_str(MC_STR_IMPORT_ERROR_SAVE);
  }

  if (error) {
    unplace_files(lib, places, count);
  } else {
    finish_swaps(lib, places, count);
    /* Done: tidy up folders that moves emptied, and remember where each file is now, so
     * choosing another match later moves it again instead of copying it a second time. */
    for (int i = 0; i < count; i++) {
      placement *pl = &places[i];
      if (pl->op == OP_MOVED) prune_dirs(lib, dirname_of(pl->f->path));
      char *now = mc_strdup(pl->dest_abs);
      if (now) {
        free(pl->f->path);
        pl->f->path = now;
      }
    }
  }
  mc_import_outcome outcome = MC_IMPORT_OUTCOME_NONE;
  if (!error && planned)
    outcome = dp.swaps ? MC_IMPORT_OUTCOME_REPLACED
              : all_dupes ? MC_IMPORT_OUTCOME_DUPLICATE
              : dp.skips  ? MC_IMPORT_OUTCOME_ADDED
                          : MC_IMPORT_OUTCOME_NONE;
  mc_mutex_lock(&imp->lock);
  it->outcome = outcome;
  it->replaced = !error && planned ? dp.swaps : 0;
  it->duplicates = !error && planned ? dp.skips : 0;
  mc_mutex_unlock(&imp->lock);

  placements_free(places, count);
  mc_dupe_plan_free(&dp);
  free(rows);
  free(singles);
  free(album_rel);
  free(source_dir);
  set_state(imp, idx, error ? MC_IMPORT_FAILED : all_dupes ? MC_IMPORT_SKIPPED : MC_IMPORT_DONE,
            error);
}

/* --- Requests from the UI --- */

static void request_free(request *r) {
  free(r->artist);
  free(r->title);
  free(r);
}

/* Appends a request for an item; the caller holds imp->lock. */
static mc_status push_request_locked(mc_import *imp, req_kind kind, int idx) {
  request *r = calloc(1, sizeof *r);
  if (!r) return MC_ERR_NOMEM;
  r->kind = kind;
  r->item = idx;
  r->match = -1;
  if (imp->tail) imp->tail->next = r;
  else imp->head = r;
  imp->tail = r;
  mc_cond_broadcast(&imp->wake);
  return MC_OK;
}

/* --- Offline --- */

static void set_offline(mc_import *imp, int offline) {
  mc_mutex_lock(&imp->lock);
  int changed = imp->offline != offline;
  imp->offline = offline;
  if (offline) imp->next_probe_ms = mc_mono_ms() + PROBE_INTERVAL_MS;
  /* Back online: match the items that were waiting on MusicBrainz after all, unless tagging
   * is being skipped (then they were queued as-is already). */
  for (int i = 0; changed && !offline && !imp->skip_tagging && i < imp->item_count; i++)
    if (imp->items[i].state == MC_IMPORT_REVIEW && imp->items[i].unreachable)
      push_request_locked(imp, REQ_RETRY, i);
  mc_mutex_unlock(&imp->lock);
  if (changed) emit(imp, MC_IMPORT_EV_OFFLINE, offline);
}

/* Checks whether MusicBrainz can be reached, and reports a change. */
static void probe(mc_import *imp) {
  set_offline(imp, mc_musicbrainz_reachable(PROBE_TIMEOUT_MS) != MC_OK);
}

/* While offline, checks again once the interval has passed. Returns nonzero if online. */
static int online(mc_import *imp) {
  mc_mutex_lock(&imp->lock);
  int due = imp->offline && mc_mono_ms() >= imp->next_probe_ms;
  mc_mutex_unlock(&imp->lock);
  if (due) probe(imp);
  return !mc_import_offline(imp);
}

/* Matches an item without the user: as-is with tagging skipped, straight to review while
 * offline, else against MusicBrainz. */
static void auto_match(mc_import *imp, mc_library *lib, int idx) {
  if (skipping_tags(imp)) write_item(imp, lib, idx, -1);
  else if (!online(imp)) set_unreachable(imp, idx);
  else if (match_item(imp, lib, idx, NULL, NULL)) {
    probe(imp); /* a network error: offline? */
    /* Tagging was skipped while this item was being looked up. */
    if (skipping_tags(imp)) write_item(imp, lib, idx, -1);
  }
}

static int busy(mc_import_state s) {
  return s == MC_IMPORT_PENDING || s == MC_IMPORT_MATCHING || s == MC_IMPORT_WRITING;
}

/* Counts a candidate's missing tracks and extra files from its map. */
static void recount(candidate *c, int file_count) {
  int matched = 0;
  for (int i = 0; i < file_count; i++) matched += c->map[i] >= 0;
  c->missing = c->rel.track_count - matched;
  c->extra = file_count - matched;
}

/* Moves file `file` of item `from` to the end of item `to`'s files, unmatched in every
 * candidate of `to`. The caller holds the lock. Returns the file's index in `to`, or -1 when
 * out of memory (nothing has moved then). */
static int move_file_locked(mc_import *imp, int to, int from, int file) {
  item *a = &imp->items[to], *b = &imp->items[from];
  int n = a->file_count + 1;
  int *files = realloc(a->files, (size_t)n * sizeof *files);
  if (!files) return -1;
  a->files = files;
  for (int k = 0; k < a->cand_count; k++) {
    int *map = realloc(a->cands[k].map, (size_t)n * sizeof *map);
    if (!map) return -1; /* the maps that grew are only bigger */
    a->cands[k].map = map;
  }
  a->files[a->file_count] = b->files[file];
  for (int k = 0; k < a->cand_count; k++) a->cands[k].map[a->file_count] = -1;
  a->file_count = n;
  for (int k = 0; k < a->cand_count; k++) recount(&a->cands[k], n);

  int rest = b->file_count - file - 1;
  memmove(&b->files[file], &b->files[file + 1], (size_t)rest * sizeof *b->files);
  for (int k = 0; k < b->cand_count; k++) {
    int *map = b->cands[k].map;
    memmove(&map[file], &map[file + 1], (size_t)rest * sizeof *map);
  }
  b->file_count--;
  for (int k = 0; k < b->cand_count; k++) recount(&b->cands[k], b->file_count);
  return n - 1;
}

/* REQ_ASSIGN (mc_import_assign): the file becomes the track, then the item is written. */
static void assign_file(mc_import *imp, mc_library *lib, const request *r) {
  mc_mutex_lock(&imp->lock);
  item *it = &imp->items[r->item];
  item *from = &imp->items[r->from_item];
  /* Checked when queued, but requests before this one may have changed the items. */
  int ok = !busy(it->state) && !busy(from->state) && r->match >= 0 && r->match < it->cand_count &&
           r->track >= 0 && r->track < it->cands[r->match].rel.track_count && r->file >= 0 &&
           r->file < from->file_count;
  int file = r->file;
  if (ok && r->from_item != r->item) {
    file = move_file_locked(imp, r->item, r->from_item, r->file);
    ok = file >= 0;
    if (ok && from->file_count == 0) {
      from->state = MC_IMPORT_SKIPPED;
      from->outcome = MC_IMPORT_OUTCOME_MOVED;
      from->unreachable = 0;
      free(from->error);
      from->error = NULL;
    }
  }
  if (ok) {
    candidate *c = &it->cands[r->match];
    for (int i = 0; i < it->file_count; i++)
      if (c->map[i] == r->track) c->map[i] = -1; /* the file that was on it stays, unmatched */
    c->map[file] = r->track;
    recount(c, it->file_count);
  }
  mc_mutex_unlock(&imp->lock);
  if (!ok) return;
  if (r->from_item != r->item) emit(imp, MC_IMPORT_EV_ITEM_CHANGED, r->from_item);
  write_item(imp, lib, r->item, r->match);
}

static void handle_request(mc_import *imp, mc_library *lib, request *r) {
  if (!lib) {
    set_state(imp, r->item, MC_IMPORT_FAILED, mc_str(MC_STR_LIBRARY_OPEN_FAILED));
    return;
  }
  switch (r->kind) {
    case REQ_ACCEPT: {
      mc_mutex_lock(&imp->lock);
      int ok = r->match >= 0 && r->match < imp->items[r->item].cand_count;
      mc_mutex_unlock(&imp->lock);
      if (ok) write_item(imp, lib, r->item, r->match);
      break;
    }
    case REQ_AS_IS: write_item(imp, lib, r->item, -1); break;
    case REQ_SKIP: set_state(imp, r->item, MC_IMPORT_SKIPPED, NULL); break;
    case REQ_SEARCH:
      if (match_item(imp, lib, r->item, r->artist, r->title)) probe(imp);
      break;
    case REQ_ASSIGN: assign_file(imp, lib, r); break;
    case REQ_RETRY: {
      /* Only if nothing else happened to the item since it was queued. */
      mc_mutex_lock(&imp->lock);
      const item *it = &imp->items[r->item];
      int waiting = it->state == MC_IMPORT_REVIEW && it->unreachable;
      mc_mutex_unlock(&imp->lock);
      if (waiting) auto_match(imp, lib, r->item);
      break;
    }
  }
}

/* Takes the next request, or NULL. With wait, blocks until there is one or the session is
 * cancelled; while offline it also checks every so often whether MusicBrainz is back. */
static request *next_request(mc_import *imp, int wait) {
  mc_mutex_lock(&imp->lock);
  while (wait && !imp->cancel && !imp->head) {
    if (!imp->offline || imp->skip_tagging) {
      mc_cond_wait(&imp->wake, &imp->lock);
      continue;
    }
    uint64_t now = mc_mono_ms();
    if (now < imp->next_probe_ms) {
      mc_cond_wait_ms(&imp->wake, &imp->lock, (uint32_t)(imp->next_probe_ms - now));
      continue;
    }
    mc_mutex_unlock(&imp->lock);
    probe(imp); /* queues retries if MusicBrainz is back */
    mc_mutex_lock(&imp->lock);
  }
  request *r = imp->cancel ? NULL : imp->head;
  if (r) {
    imp->head = r->next;
    if (!imp->head) imp->tail = NULL;
  }
  mc_mutex_unlock(&imp->lock);
  return r;
}

static void drain_requests(mc_import *imp, mc_library *lib) {
  request *r;
  while ((r = next_request(imp, 0))) {
    handle_request(imp, lib, r);
    request_free(r);
  }
}

static void import_thread(void *arg) {
  mc_import *imp = arg;
  mc_library *lib = NULL;
  if (mc_library_open(imp->lib_dir, &lib) != MC_OK) lib = NULL;

  for (int i = 0; i < imp->path_count && !cancelled(imp); i++) mc_walk(imp->paths[i], scan_file, imp);
  if (imp->file_count > 1) qsort(imp->files, (size_t)imp->file_count, sizeof *imp->files, compare_paths);
  emit(imp, MC_IMPORT_EV_SCANNING, imp->file_count);

  /* Items are built privately, then published under the lock in one go. On allocation
   * failure, whatever was grouped so far is still imported. */
  item_list staged = {0};
  group_files(imp, &staged);
  mc_mutex_lock(&imp->lock);
  imp->items = staged.items;
  imp->item_count = staged.count;
  mc_mutex_unlock(&imp->lock);
  emit(imp, MC_IMPORT_EV_SCANNED, imp->item_count);

  /* One quick check up front, so an offline import says so before the first item. */
  if (lib && imp->item_count && !skipping_tags(imp) && !cancelled(imp)) probe(imp);

  for (int i = 0; i < imp->item_count && !cancelled(imp); i++) {
    drain_requests(imp, lib); /* the user's decisions come before more matching */
    if (lib) auto_match(imp, lib, i);
    else set_state(imp, i, MC_IMPORT_FAILED, mc_str(MC_STR_LIBRARY_OPEN_FAILED));
  }
  if (!cancelled(imp)) emit(imp, MC_IMPORT_EV_IDLE, 0);

  request *r;
  while ((r = next_request(imp, 1))) {
    handle_request(imp, lib, r);
    request_free(r);
    mc_mutex_lock(&imp->lock);
    int idle = !imp->head && !imp->cancel;
    mc_mutex_unlock(&imp->lock);
    if (idle) emit(imp, MC_IMPORT_EV_IDLE, 0);
  }
  mc_library_close(lib);
}

/* --- Public API --- */

mc_status mc_import_start(mc_library *lib, const char *const *paths, int path_count,
                          const mc_import_options *opts, mc_import_fn callback, void *user,
                          mc_import **out) {
  if (!lib || !paths || path_count <= 0 || !out) return MC_ERR_INVALID_ARG;
  *out = NULL;
  mc_import *imp = calloc(1, sizeof *imp);
  if (!imp) return MC_ERR_NOMEM;
  mc_mutex_init(&imp->lock);
  mc_cond_init(&imp->wake);
  imp->threshold = opts && opts->threshold > 0 ? opts->threshold : DEFAULT_THRESHOLD;
  imp->fetch_art = opts ? opts->fetch_art : 1;
  imp->skip_tagging = opts ? opts->skip_tagging : 0;
  imp->callback = callback;
  imp->user = user;
  imp->lib_dir = mc_strdup(mc_library_dir(lib));
  imp->paths = calloc((size_t)path_count, sizeof *imp->paths);
  mc_status st = imp->lib_dir && imp->paths ? MC_OK : MC_ERR_NOMEM;
  for (int i = 0; i < path_count && st == MC_OK; i++) {
    if (!(imp->paths[i] = mc_strdup(paths[i]))) st = MC_ERR_NOMEM;
    imp->path_count = i + 1;
  }
  if (st == MC_OK && (st = mc_thread_start(&imp->thread, import_thread, imp)) == MC_OK)
    imp->thread_started = 1;
  if (st != MC_OK) {
    mc_import_free(imp);
    return st;
  }
  *out = imp;
  return MC_OK;
}

void mc_import_free(mc_import *imp) {
  if (!imp) return;
  mc_mutex_lock(&imp->lock);
  imp->cancel = 1;
  mc_cond_broadcast(&imp->wake);
  mc_mutex_unlock(&imp->lock);
  if (imp->thread_started) mc_thread_join(&imp->thread);

  for (request *r = imp->head, *next; r; r = next) {
    next = r->next;
    request_free(r);
  }
  for (int i = 0; i < imp->item_count; i++) {
    item *it = &imp->items[i];
    free(it->folder);
    free(it->artist);
    free(it->album);
    free(it->files);
    free(it->error);
    candidates_free(it->cands, it->cand_count);
  }
  free(imp->items);
  for (int i = 0; i < imp->file_count; i++) {
    free(imp->files[i].path);
    mc_tags_clear(&imp->files[i].tags);
  }
  free(imp->files);
  for (int i = 0; i < imp->path_count; i++) free(imp->paths[i]);
  free(imp->paths);
  free(imp->lib_dir);
  mc_cond_destroy(&imp->wake);
  mc_mutex_destroy(&imp->lock);
  free(imp);
}

int mc_import_item_count(mc_import *imp) {
  if (!imp) return 0;
  mc_mutex_lock(&imp->lock);
  int n = imp->item_count;
  mc_mutex_unlock(&imp->lock);
  return n;
}

static void copy_match(const item *it, const candidate *c, mc_import_match *m) {
  const mb_release *rel = &c->rel;
  m->album = rel->is_single ? singles_title(rel->primary_artist) : mc_strdup(rel->title);
  m->album_artist = mc_strdup(rel->is_single ? rel->primary_artist : rel->artist);
  m->release = mc_strdup(rel->title);
  m->country = mc_strdup(rel->country);
  m->label = mc_strdup(rel->label);
  m->catalog = mc_strdup(rel->catalog);
  m->format = mc_strdup(rel->format);
  m->release_type = mc_strdup(rel->release_type);
  m->mb_albumid = mc_strdup(rel->id);
  m->year = rel->is_single && rel->original_year ? rel->original_year : rel->year;
  m->is_single = rel->is_single;
  m->confidence = c->distance < 0 ? 1.0 : c->distance > 1 ? 0.0 : 1.0 - c->distance;
  m->extra_files = c->extra;

  if (!(m->tracks = calloc((size_t)rel->track_count, sizeof *m->tracks))) return;
  m->track_count = rel->track_count;
  for (int j = 0; j < rel->track_count; j++) {
    const mb_track *t = &rel->tracks[j];
    mc_import_match_track *mt = &m->tracks[j];
    mt->title = mc_strdup(t->title);
    mt->artist = mc_strdup(t->artist);
    mt->disc = t->disc;
    mt->number = t->number;
    mt->length_ms = t->length_ms;
    mt->file = -1;
    for (int i = 0; i < it->file_count; i++)
      if (c->map[i] == j) mt->file = i;
  }
}

mc_status mc_import_item_get(mc_import *imp, int idx, mc_import_item **out) {
  if (!imp || !out) return MC_ERR_INVALID_ARG;
  *out = NULL;
  mc_import_item *snap = calloc(1, sizeof *snap);
  if (!snap) return MC_ERR_NOMEM;

  mc_mutex_lock(&imp->lock);
  if (idx < 0 || idx >= imp->item_count) {
    mc_mutex_unlock(&imp->lock);
    free(snap);
    return MC_ERR_INVALID_ARG;
  }
  const item *it = &imp->items[idx];
  snap->kind = it->kind;
  snap->state = it->state;
  snap->folder = mc_strdup(it->folder);
  snap->artist = mc_strdup(it->artist);
  snap->album = mc_strdup(it->album);
  snap->chosen = it->chosen;
  snap->error = mc_strdup(it->error);
  snap->outcome = it->outcome;
  snap->replaced_files = it->replaced;
  snap->duplicate_files = it->duplicates;
  if ((snap->files = calloc((size_t)it->file_count, sizeof *snap->files))) {
    snap->file_count = it->file_count;
    for (int i = 0; i < it->file_count; i++) {
      const file_entry *f = &imp->files[it->files[i]];
      mc_import_file *sf = &snap->files[i];
      sf->path = mc_strdup(f->path);
      sf->title = mc_strdup(f->tags.title);
      sf->artist = mc_strdup(f->tags.artist);
      sf->track = f->tags.track;
      sf->disc = f->tags.disc;
      sf->duration_ms = f->tags.duration_ms;
    }
  }
  if (it->cand_count && (snap->matches = calloc((size_t)it->cand_count, sizeof *snap->matches))) {
    snap->match_count = it->cand_count;
    for (int i = 0; i < it->cand_count; i++) copy_match(it, &it->cands[i], &snap->matches[i]);
  }
  mc_mutex_unlock(&imp->lock);

  *out = snap;
  return MC_OK;
}

void mc_import_item_free(mc_import_item *s) {
  if (!s) return;
  for (int i = 0; i < s->file_count; i++) {
    free(s->files[i].path);
    free(s->files[i].title);
    free(s->files[i].artist);
  }
  free(s->files);
  for (int i = 0; i < s->match_count; i++) {
    mc_import_match *m = &s->matches[i];
    char *strs[] = {m->album, m->album_artist, m->release, m->country, m->label,
                    m->catalog, m->format, m->release_type, m->mb_albumid};
    for (size_t k = 0; k < sizeof strs / sizeof strs[0]; k++) free(strs[k]);
    for (int j = 0; j < m->track_count; j++) {
      free(m->tracks[j].title);
      free(m->tracks[j].artist);
    }
    free(m->tracks);
  }
  free(s->matches);
  free(s->folder);
  free(s->artist);
  free(s->album);
  free(s->error);
  free(s);
}

static mc_status enqueue(mc_import *imp, req_kind kind, int idx, int match, const char *artist,
                         const char *title) {
  if (!imp) return MC_ERR_INVALID_ARG;
  request *r = calloc(1, sizeof *r);
  if (!r) return MC_ERR_NOMEM;
  r->kind = kind;
  r->item = idx;
  r->match = match;
  r->artist = artist && *artist ? mc_strdup(artist) : NULL;
  r->title = title && *title ? mc_strdup(title) : NULL;

  mc_mutex_lock(&imp->lock);
  mc_status st = MC_OK;
  if (idx < 0 || idx >= imp->item_count) {
    st = MC_ERR_INVALID_ARG;
  } else {
    /* Busy items can't be changed; a finished one can be re-tagged but not un-imported. */
    /* Nor one whose files all went to another item (mc_import_assign). */
    mc_import_state s = imp->items[idx].state;
    if (busy(s) || (s == MC_IMPORT_DONE && kind == REQ_SKIP) || imp->items[idx].file_count == 0)
      st = MC_ERR_INVALID_ARG;
  }
  if (st == MC_OK) {
    if (imp->tail) imp->tail->next = r;
    else imp->head = r;
    imp->tail = r;
    mc_cond_broadcast(&imp->wake);
  }
  mc_mutex_unlock(&imp->lock);
  if (st != MC_OK) request_free(r);
  return st;
}

mc_status mc_import_accept(mc_import *imp, int idx, int match) {
  return enqueue(imp, REQ_ACCEPT, idx, match, NULL, NULL);
}

mc_status mc_import_accept_as_is(mc_import *imp, int idx) {
  return enqueue(imp, REQ_AS_IS, idx, -1, NULL, NULL);
}

mc_status mc_import_skip(mc_import *imp, int idx) {
  return enqueue(imp, REQ_SKIP, idx, -1, NULL, NULL);
}

mc_status mc_import_search(mc_import *imp, int idx, const char *artist, const char *title) {
  if (!title || !*title) return MC_ERR_INVALID_ARG;
  return enqueue(imp, REQ_SEARCH, idx, -1, artist, title);
}

mc_status mc_import_assign(mc_import *imp, int idx, int match, int track, int from_item, int file) {
  if (!imp) return MC_ERR_INVALID_ARG;
  request *r = calloc(1, sizeof *r);
  if (!r) return MC_ERR_NOMEM;
  r->kind = REQ_ASSIGN;
  r->item = idx;
  r->match = match;
  r->track = track;
  r->from_item = from_item;
  r->file = file;

  mc_mutex_lock(&imp->lock);
  mc_status st = MC_ERR_INVALID_ARG;
  if (idx >= 0 && idx < imp->item_count && from_item >= 0 && from_item < imp->item_count) {
    const item *it = &imp->items[idx], *from = &imp->items[from_item];
    if (!busy(it->state) && !busy(from->state) && match >= 0 && match < it->cand_count &&
        track >= 0 && track < it->cands[match].rel.track_count && file >= 0 && file < from->file_count)
      st = MC_OK;
  }
  if (st == MC_OK) {
    if (imp->tail) imp->tail->next = r;
    else imp->head = r;
    imp->tail = r;
    mc_cond_broadcast(&imp->wake);
  }
  mc_mutex_unlock(&imp->lock);
  if (st != MC_OK) request_free(r);
  return st;
}

/* Whether dir is root or a folder inside it. */
static int folder_within(const char *dir, const char *root) {
  size_t n = root ? strlen(root) : 0;
  if (!dir || !n || strncmp(dir, root, n) != 0) return 0;
  return dir[n] == '\0' || dir[n] == '/' || dir[n] == '\\';
}

int mc_import_related(mc_import *imp, int idx, int *out, int max) {
  if (!imp) return 0;
  int n = 0;
  mc_mutex_lock(&imp->lock);
  if (idx >= 0 && idx < imp->item_count) {
    const char *root = imp->items[idx].folder;
    for (int i = 0; i < imp->item_count; i++) {
      const item *it = &imp->items[i];
      if (i == idx || it->file_count == 0 || !folder_within(it->folder, root)) continue;
      if (out && n < max) out[n] = i;
      n++;
    }
  }
  mc_mutex_unlock(&imp->lock);
  return n;
}

mc_status mc_import_skip_tagging(mc_import *imp) {
  if (!imp) return MC_ERR_INVALID_ARG;
  mc_status st = MC_OK;
  mc_mutex_lock(&imp->lock);
  if (!imp->skip_tagging) {
    imp->skip_tagging = 1;
    for (int i = 0; i < imp->item_count && st == MC_OK; i++)
      if (imp->items[i].state == MC_IMPORT_REVIEW && imp->items[i].unreachable)
        st = push_request_locked(imp, REQ_RETRY, i); /* as-is now */
    mc_cond_broadcast(&imp->wake); /* the thread may be waiting to check the network again */
  }
  mc_mutex_unlock(&imp->lock);
  return st;
}

int mc_import_offline(mc_import *imp) {
  if (!imp) return 0;
  mc_mutex_lock(&imp->lock);
  int offline = imp->offline;
  mc_mutex_unlock(&imp->lock);
  return offline;
}

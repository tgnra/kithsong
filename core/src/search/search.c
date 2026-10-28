/* Library search (musiccore/search.h).
 *
 * The index is a sorted vocabulary of folded words (util/text.h), each with a posting list of
 * the items and fields that hold it. A search folds the query into words and, for each, walks
 * the vocabulary as a trie: words that share a prefix share its columns of the edit distance
 * table, and every word under a prefix that's already too far from the query word is skipped
 * at once. Items reached by every query word are scored by how well and where each matched.
 * Indexes are built in the background; see "Background builds". */
#include "search/search_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "library/library_internal.h"
#include "util/platform.h"
#include "util/text.h"

#define MAX_WORD 48 /* longer words are cut short; nobody types more */
#define MAX_QUERY_WORDS 16
#define MAX_FIELDS 3
#define KIND_COUNT 5
#define NO_TEXT UINT32_MAX

/* How much a match in each field counts: the name first, then the artist and album that
 * tell an album or track apart. */
static const double k_weight[KIND_COUNT][MAX_FIELDS] = {
  [MC_SEARCH_ARTIST] = {1.0, 0.9},     /* name, sort name */
  [MC_SEARCH_ALBUM] = {1.0, 0.5},      /* title, artist */
  [MC_SEARCH_TRACK] = {1.0, 0.5, 0.4}, /* title, artist, album */
  [MC_SEARCH_PLAYLIST] = {1.0},
  [MC_SEARCH_ALBUM_GROUP] = {1.0},
};

/* What a query word's match is worth before its field's weight. */
#define Q_EXACT 1.0
#define Q_PREFIX 0.9
#define Q_ONE_EDIT 0.7
#define Q_TWO_EDITS 0.5
/* Added when the whole query is the name, or the start of it. */
#define BONUS_WHOLE 0.5
#define BONUS_START 0.25
/* Shared out by the name's words, so of two equal matches the shorter name wins. */
#define BONUS_SHORT 0.1

/* Tables whose changes can change what the index holds. */
#define INDEXED_ENTITIES "'artists', 'albums', 'tracks', 'playlists', 'album_groups', 'smart_lists'"

typedef struct item {
  mc_search_kind kind;
  int is_smart;
  char uid[MC_UID_STRLEN];
  char album_uid[MC_UID_STRLEN]; /* a track's album, or "" */
  uint32_t name, artist, album;  /* in text, or NO_TEXT */
  uint32_t key, key_len;        /* the name folded, its words joined by ' ', in the arena */
  int words;                    /* how many words the name has */
} item;

typedef struct word {
  uint32_t at, len; /* in the arena */
} word;

typedef struct posting {
  uint32_t item, field;
} posting;

typedef struct raw_posting {
  uint32_t word, item, field;
} raw_posting;

typedef struct mc_search_index mc_search_index;

struct mc_search_index {
  int64_t seq; /* the changelog seq it was built from, or moved on to by stale() */
  item *items;
  size_t nitems, items_cap;
  char *text; /* the items' names, artists and albums, each ending in '\0' */
  size_t text_len, text_cap;
  uint32_t *arena; /* vocabulary words and item keys */
  size_t arena_len, arena_cap;
  word *words; /* in order once the build is done */
  size_t nwords, words_cap;
  uint32_t *slots; /* hash table of word id + 1, 0 if empty; a power of two in size */
  size_t slots_cap;
  /* Word w's postings are post[post_start[w]] up to post[post_start[w + 1]]. Until a build
   * finishes they're collected unsorted in raw. */
  uint32_t *post_start;
  posting *post;
  raw_posting *raw;
  size_t nraw, raw_cap;
  /* A search's scratch, one per item. total and mask are all 0 between searches, best is all
   * 0 between query words. */
  double *total, *best;
  uint32_t *mask, *seen, *touched;
};

static void mc_search_index_free(mc_search_index *ix) {
  if (!ix) return;
  free(ix->items);
  free(ix->text);
  free(ix->arena);
  free(ix->words);
  free(ix->slots);
  free(ix->post_start);
  free(ix->post);
  free(ix->raw);
  free(ix->total);
  free(ix->best);
  free(ix->mask);
  free(ix->seen);
  free(ix->touched);
  free(ix);
}

static int grow(void **p, size_t *cap, size_t need, size_t size) {
  if (need <= *cap) return 1;
  size_t c = *cap ? *cap : 256;
  while (c < need) c *= 2;
  void *q = realloc(*p, c * size);
  if (!q) return 0;
  *p = q;
  *cap = c;
  return 1;
}

/* --- Words --- */

typedef int (*word_fn)(void *ctx, const uint32_t *w, int n);

#define RUNS 1  /* also words joined by punctuation alone: "ac/dc" -> "acdc" */
#define WHOLE 2 /* also the whole of a 2 to 4 word name: "the beatles" -> "thebeatles" */

/* Calls fn with each folded word of s, and per flags with the words run together, so a query
 * typed without the punctuation or spaces still matches. Stops at fn's first nonzero return
 * and returns it. */
static int each_word(const char *s, int flags, word_fn fn, void *ctx) {
  uint32_t w[MAX_WORD], run[MAX_WORD], all[MAX_WORD];
  int n = 0, rn = 0, an = 0, run_words = 0, all_words = 0, rc;
  const unsigned char *p = (const unsigned char *)(s ? s : "");
  for (;;) {
    uint32_t f[3];
    int end = !*p;
    int k = end ? MC_FOLD_SPACE : mc_text_fold(mc_utf8_next(&p), f);
    if (k == MC_FOLD_DROP) continue;
    if (k > 0) {
      for (int i = 0; i < k; i++) {
        if (n < MAX_WORD) w[n++] = f[i];
        if (rn < MAX_WORD) run[rn++] = f[i];
        if (an < MAX_WORD) all[an++] = f[i];
      }
      continue;
    }
    if (n) {
      if ((rc = fn(ctx, w, n))) return rc;
      run_words++, all_words++;
      n = 0;
    }
    if (k == MC_FOLD_SPACE) {
      if ((flags & RUNS) && run_words > 1 && (rc = fn(ctx, run, rn))) return rc;
      rn = run_words = 0;
    }
    if (end) break;
  }
  if ((flags & WHOLE) && all_words > 1 && all_words <= 4) return fn(ctx, all, an);
  return 0;
}

static int compare(const uint32_t *a, uint32_t n, const uint32_t *b, uint32_t m) {
  for (uint32_t i = 0; i < n && i < m; i++)
    if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
  return (n > m) - (n < m);
}

/* --- Building --- */

static int64_t arena_put(mc_search_index *ix, const uint32_t *w, size_t n) {
  if (!grow((void **)&ix->arena, &ix->arena_cap, ix->arena_len + n, sizeof *ix->arena)) return -1;
  memcpy(ix->arena + ix->arena_len, w, n * sizeof *w);
  ix->arena_len += n;
  return (int64_t)(ix->arena_len - n);
}

/* Copies s into text: its offset, NO_TEXT for NULL, or -1 if out of memory. */
static int64_t text_put(mc_search_index *ix, const char *s) {
  if (!s) return NO_TEXT;
  size_t n = strlen(s) + 1;
  if (!grow((void **)&ix->text, &ix->text_cap, ix->text_len + n, 1)) return -1;
  memcpy(ix->text + ix->text_len, s, n);
  ix->text_len += n;
  return (int64_t)(ix->text_len - n);
}

static const char *text_at(const mc_search_index *ix, uint32_t at) {
  return at == NO_TEXT ? NULL : ix->text + at;
}

static uint32_t hash_word(const uint32_t *w, int n) {
  uint32_t h = 2166136261u;
  for (int i = 0; i < n; i++) h = (h ^ w[i]) * 16777619u;
  return h;
}

static void slot_insert(mc_search_index *ix, uint32_t id) {
  const word *x = &ix->words[id];
  size_t mask = ix->slots_cap - 1;
  size_t i = hash_word(ix->arena + x->at, (int)x->len) & mask;
  while (ix->slots[i]) i = (i + 1) & mask;
  ix->slots[i] = id + 1;
}

/* The id of word w, added to the vocabulary if it's new; -1 if out of memory. */
static int64_t intern(mc_search_index *ix, const uint32_t *w, int n) {
  if ((ix->nwords + 1) * 2 > ix->slots_cap) {
    size_t cap = ix->slots_cap ? ix->slots_cap * 2 : 1024;
    uint32_t *slots = calloc(cap, sizeof *slots);
    if (!slots) return -1;
    free(ix->slots);
    ix->slots = slots;
    ix->slots_cap = cap;
    for (size_t id = 0; id < ix->nwords; id++) slot_insert(ix, (uint32_t)id);
  }
  size_t mask = ix->slots_cap - 1;
  for (size_t i = hash_word(w, n) & mask; ix->slots[i]; i = (i + 1) & mask) {
    const word *x = &ix->words[ix->slots[i] - 1];
    if (x->len == (uint32_t)n && memcmp(ix->arena + x->at, w, (size_t)n * sizeof *w) == 0)
      return ix->slots[i] - 1;
  }
  if (!grow((void **)&ix->words, &ix->words_cap, ix->nwords + 1, sizeof *ix->words)) return -1;
  int64_t at = arena_put(ix, w, (size_t)n);
  if (at < 0) return -1;
  ix->words[ix->nwords] = (word){(uint32_t)at, (uint32_t)n};
  slot_insert(ix, (uint32_t)ix->nwords);
  return (int64_t)ix->nwords++;
}

typedef struct field_ctx {
  mc_search_index *ix;
  uint32_t item, field;
} field_ctx;

static int add_posting(void *ctx, const uint32_t *w, int n) {
  field_ctx *f = ctx;
  mc_search_index *ix = f->ix;
  int64_t id = intern(ix, w, n);
  if (id < 0 || !grow((void **)&ix->raw, &ix->raw_cap, ix->nraw + 1, sizeof *ix->raw)) return 1;
  ix->raw[ix->nraw++] = (raw_posting){(uint32_t)id, f->item, f->field};
  return 0;
}

typedef struct key_ctx {
  uint32_t *buf; /* NULL to append to ix's arena instead */
  mc_search_index *ix;
  size_t len, cap;
  int words;
} key_ctx;

/* Appends a word to a key, after a ' ' if it isn't the first. */
static int add_key_word(void *ctx, const uint32_t *w, int n) {
  key_ctx *k = ctx;
  uint32_t space = ' ';
  size_t sep = k->words ? 1 : 0;
  if (k->buf) {
    if (k->len + sep + (size_t)n > k->cap) return 1;
    if (sep) k->buf[k->len] = space;
    memcpy(k->buf + k->len + sep, w, (size_t)n * sizeof *w);
  } else if ((sep && arena_put(k->ix, &space, 1) < 0) || arena_put(k->ix, w, (size_t)n) < 0) {
    return 1;
  }
  k->len += sep + (size_t)n;
  k->words++;
  return 0;
}

/* Adds an item and indexes its fields (NULL ones are skipped); 0 if out of memory. */
static int add_item(mc_search_index *ix, mc_search_kind kind, int is_smart, const char *uid,
                    const char *name, const char *artist, const char *album, const char *album_uid,
                    const char *sort_name) {
  if (!grow((void **)&ix->items, &ix->items_cap, ix->nitems + 1, sizeof *ix->items)) return 0;
  item *it = &ix->items[ix->nitems];
  memset(it, 0, sizeof *it);
  it->kind = kind;
  it->is_smart = is_smart;
  snprintf(it->uid, sizeof it->uid, "%s", uid ? uid : "");
  snprintf(it->album_uid, sizeof it->album_uid, "%s", album_uid ? album_uid : "");
  int64_t at[3] = {text_put(ix, name ? name : ""), text_put(ix, artist), text_put(ix, album)};
  if (at[0] < 0 || at[1] < 0 || at[2] < 0) return 0;
  it->name = (uint32_t)at[0], it->artist = (uint32_t)at[1], it->album = (uint32_t)at[2];

  key_ctx k = {.ix = ix};
  it->key = (uint32_t)ix->arena_len;
  if (each_word(name, 0, add_key_word, &k)) return 0;
  it->key_len = (uint32_t)k.len;
  it->words = k.words;
  ix->nitems++;

  const char *fields[MAX_FIELDS] = {name};
  int flags[MAX_FIELDS] = {RUNS};
  if (kind == MC_SEARCH_ARTIST) {
    flags[0] |= WHOLE;
    fields[1] = sort_name, flags[1] = RUNS;
  } else if (kind == MC_SEARCH_ALBUM || kind == MC_SEARCH_TRACK) {
    fields[1] = artist, flags[1] = RUNS | WHOLE;
    if (kind == MC_SEARCH_TRACK) fields[2] = album, flags[2] = RUNS;
  }
  for (uint32_t f = 0; f < MAX_FIELDS; f++) {
    if (!fields[f]) continue;
    field_ctx ctx = {ix, (uint32_t)(ix->nitems - 1), f};
    if (each_word(fields[f], flags[f], add_posting, &ctx)) return 0;
  }
  return 1;
}

static const char *col(sqlite3_stmt *stmt, int i) {
  return (const char *)sqlite3_column_text(stmt, i);
}

/* Adds the rows of sql as items of kind; see build() for the columns. */
static mc_status load(sqlite3 *db, mc_search_index *ix, mc_search_kind kind, const char *sql) {
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return MC_ERR_DB;
  mc_status st = MC_OK;
  int rc;
  while (st == MC_OK && (rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const char *uid = col(stmt, 0), *name = col(stmt, 1);
    int ok = 1;
    switch (kind) {
    case MC_SEARCH_ARTIST:
      ok = add_item(ix, kind, 0, uid, name, NULL, NULL, NULL, col(stmt, 2));
      break;
    case MC_SEARCH_ALBUM:
      ok = add_item(ix, kind, 0, uid, name, col(stmt, 2), NULL, NULL, NULL);
      break;
    case MC_SEARCH_TRACK:
      ok = add_item(ix, kind, 0, uid, name, col(stmt, 2), col(stmt, 3), col(stmt, 4), NULL);
      break;
    case MC_SEARCH_PLAYLIST:
    case MC_SEARCH_ALBUM_GROUP: {
      /* A third column is a smart list's subject; one this build doesn't know is skipped. */
      const char *subject = sqlite3_column_count(stmt) > 2 ? col(stmt, 2) : NULL;
      mc_search_kind k = kind;
      if (subject) {
        if (strcmp(subject, "tracks") == 0) k = MC_SEARCH_PLAYLIST;
        else if (strcmp(subject, "albums") == 0) k = MC_SEARCH_ALBUM_GROUP;
        else break;
      }
      ok = add_item(ix, k, subject != NULL, uid, name, NULL, NULL, NULL, NULL);
      break;
    }
    }
    if (!ok) st = MC_ERR_NOMEM;
  }
  if (st == MC_OK && rc != SQLITE_DONE) st = MC_ERR_DB;
  sqlite3_finalize(stmt);
  return st;
}

static int word_before(const mc_search_index *ix, uint32_t a, uint32_t b) {
  const word *x = &ix->words[a], *y = &ix->words[b];
  return compare(ix->arena + x->at, x->len, ix->arena + y->at, y->len) < 0;
}

/* Bottom-up merge sort of n word ids into order, using tmp; the result ends up in *order. */
static void sort_words(const mc_search_index *ix, uint32_t **order, uint32_t **tmp, size_t n) {
  for (size_t width = 1; width < n; width *= 2) {
    uint32_t *a = *order, *t = *tmp;
    for (size_t lo = 0; lo < n; lo += 2 * width) {
      size_t mid = lo + width < n ? lo + width : n, hi = lo + 2 * width < n ? lo + 2 * width : n;
      size_t i = lo, j = mid, k = lo;
      while (i < mid && j < hi) t[k++] = word_before(ix, a[j], a[i]) ? a[j++] : a[i++];
      while (i < mid) t[k++] = a[i++];
      while (j < hi) t[k++] = a[j++];
    }
    *order = t, *tmp = a;
  }
}

/* Puts the vocabulary in order, so words sharing a prefix sit together, and the raw postings
 * into post by word; then makes the search scratch. */
static mc_status finish(mc_search_index *ix) {
  size_t nw = ix->nwords, ni = ix->nitems ? ix->nitems : 1;
  uint32_t *order = malloc((nw ? nw : 1) * sizeof *order), *tmp = malloc((nw ? nw : 1) * sizeof *tmp);
  word *sorted = malloc((nw ? nw : 1) * sizeof *sorted);
  ix->post_start = calloc(nw + 1, sizeof *ix->post_start);
  ix->post = malloc((ix->nraw ? ix->nraw : 1) * sizeof *ix->post);
  ix->total = calloc(ni, sizeof *ix->total);
  ix->best = calloc(ni, sizeof *ix->best);
  ix->mask = calloc(ni, sizeof *ix->mask);
  ix->seen = malloc(ni * sizeof *ix->seen);
  ix->touched = malloc(ni * sizeof *ix->touched);
  mc_status st = MC_ERR_NOMEM;
  if (!order || !tmp || !sorted || !ix->post_start || !ix->post || !ix->total || !ix->best ||
      !ix->mask || !ix->seen || !ix->touched)
    goto done;

  for (size_t i = 0; i < nw; i++) order[i] = (uint32_t)i;
  sort_words(ix, &order, &tmp, nw);
  uint32_t *rank = tmp; /* rank[old id] = new id */
  for (size_t i = 0; i < nw; i++) {
    sorted[i] = ix->words[order[i]];
    rank[order[i]] = (uint32_t)i;
  }
  free(ix->words);
  ix->words = sorted;
  sorted = NULL;

  for (size_t i = 0; i < ix->nraw; i++) ix->post_start[rank[ix->raw[i].word] + 1]++;
  for (size_t w = 0; w < nw; w++) ix->post_start[w + 1] += ix->post_start[w];
  uint32_t *next = order; /* where each word's next posting goes */
  memcpy(next, ix->post_start, nw * sizeof *next);
  for (size_t i = 0; i < ix->nraw; i++) {
    const raw_posting *r = &ix->raw[i];
    ix->post[next[rank[r->word]]++] = (posting){r->item, r->field};
  }
  free(ix->raw);
  ix->raw = NULL;
  ix->nraw = ix->raw_cap = 0;
  free(ix->slots); /* only needed while interning */
  ix->slots = NULL;
  ix->slots_cap = 0;
  st = MC_OK;

done:
  free(order);
  free(tmp);
  free(sorted);
  return st;
}

static mc_status build(sqlite3 *db, mc_search_index **out) {
  mc_search_index *ix = calloc(1, sizeof *ix);
  if (!ix) return MC_ERR_NOMEM;
  mc_status st;
  if ((st = load(db, ix, MC_SEARCH_ARTIST, "SELECT uid, name, sort_name FROM artists")) != MC_OK ||
      (st = load(db, ix, MC_SEARCH_ALBUM,
                 "SELECT a.uid, a.title, ar.name FROM albums a" ALBUM_JOINS)) != MC_OK ||
      (st = load(db, ix, MC_SEARCH_TRACK,
                 "SELECT t.uid, t.title, coalesce(ar.name, aar.name), al.title, al.uid FROM tracks t"
                 TRACK_JOINS)) != MC_OK ||
      (st = load(db, ix, MC_SEARCH_PLAYLIST, "SELECT uid, name FROM playlists")) != MC_OK ||
      (st = load(db, ix, MC_SEARCH_ALBUM_GROUP, "SELECT uid, name FROM album_groups")) != MC_OK ||
      (st = load(db, ix, MC_SEARCH_PLAYLIST, "SELECT uid, name, subject FROM smart_lists")) != MC_OK ||
      (st = finish(ix)) != MC_OK) {
    mc_search_index_free(ix);
    return st;
  }
  *out = ix;
  return MC_OK;
}

static mc_status query_int64(sqlite3 *db, const char *sql, int64_t arg, int64_t *out) {
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return MC_ERR_DB;
  if (sqlite3_bind_parameter_count(stmt)) sqlite3_bind_int64(stmt, 1, arg);
  int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) *out = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  return rc == SQLITE_ROW ? MC_OK : MC_ERR_DB;
}

/* --- Background builds --- */

/* Indexes are built on a worker thread with a read-only connection of its own, so the
 * handle's thread never waits for one except the very first. The worker only hands a
 * finished index over through ready; the index in use belongs to the handle's thread, which
 * picks up ready at the start of a search or prepare. */
struct mc_search {
  char *db_path;
  mc_search_index *index; /* in use; NULL until the first build is done */
  int builds;             /* indexes put to use */
  mc_mutex mutex;         /* guards the rest */
  mc_cond done;           /* broadcast when ready is set or the worker finishes */
  mc_thread thread;
  int running;            /* the worker hasn't finished */
  int joinable;           /* a worker was started and not yet joined */
  int again;              /* build once more: the library changed after this build's snapshot */
  int64_t snap_seq;       /* the changelog seq the running build reads, -1 until it has */
  int closing;
  sqlite3 *worker_db;     /* the worker's connection while it's open, to interrupt */
  mc_search_index *ready; /* finished and not yet put to use */
};

/* Builds one index from one snapshot of db, recording the snapshot's changelog seq in
 * s->snap_seq and the index. */
static mc_status build_snapshot(mc_search *s, sqlite3 *db, mc_search_index **out) {
  if (sqlite3_exec(db, "BEGIN", NULL, NULL, NULL) != SQLITE_OK) return MC_ERR_DB;
  int64_t seq = 0;
  mc_status st = query_int64(db, "SELECT coalesce(max(seq), 0) FROM changelog", 0, &seq);
  if (st == MC_OK) {
    mc_mutex_lock(&s->mutex);
    s->snap_seq = seq;
    mc_mutex_unlock(&s->mutex);
    if ((st = build(db, out)) == MC_OK) (*out)->seq = seq;
  }
  sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
  return st;
}

static void worker(void *arg) {
  mc_search *s = arg;
  sqlite3 *db = NULL;
  int ok = sqlite3_open_v2(s->db_path, &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, NULL) ==
           SQLITE_OK;
  if (ok) sqlite3_busy_timeout(db, 5000);
  mc_mutex_lock(&s->mutex);
  s->worker_db = ok ? db : NULL;
  while (ok && !s->closing) {
    s->again = 0;
    s->snap_seq = -1;
    mc_mutex_unlock(&s->mutex);
    mc_search_index *ix = NULL;
    mc_status st = build_snapshot(s, db, &ix);
    mc_mutex_lock(&s->mutex);
    if (st == MC_OK && !s->closing) {
      mc_search_index_free(s->ready);
      s->ready = ix;
      mc_cond_broadcast(&s->done);
    } else {
      mc_search_index_free(ix);
      if (!s->closing) fprintf(stderr, "musiccore: search index build failed (%d)\n", st);
    }
    if (!s->again) break;
  }
  s->worker_db = NULL;
  s->running = 0;
  mc_cond_broadcast(&s->done);
  mc_mutex_unlock(&s->mutex);
  sqlite3_close(db);
}

/* Starts a build that will see every change up to seq: a new worker, or, if one is running
 * whose snapshot is older, another round of it. Under s->mutex. */
static mc_status start_build(mc_search *s, int64_t seq) {
  if (s->running) {
    /* A worker that hasn't read its snapshot yet will see the change anyway. */
    if (s->snap_seq >= 0 && seq > s->snap_seq) s->again = 1;
    return MC_OK;
  }
  if (s->joinable) mc_thread_join(&s->thread); /* finished; only closing its connection */
  s->joinable = 0;
  s->running = 1;
  s->snap_seq = -1;
  mc_status st = mc_thread_start(&s->thread, worker, s);
  if (st != MC_OK) s->running = 0;
  else s->joinable = 1;
  return st;
}

/* Puts a finished build to use, if there is one. Under s->mutex. */
static void adopt(mc_search *s) {
  if (!s->ready) return;
  mc_search_index_free(s->index);
  s->index = s->ready;
  s->ready = NULL;
  s->builds++;
}

static mc_search *search_state(mc_library *lib) {
  if (lib->search) return lib->search;
  mc_search *s = calloc(1, sizeof *s);
  size_t n = strlen(lib->dir) + sizeof MC_LIBRARY_DB_NAME + 1;
  if (!s || !(s->db_path = malloc(n))) {
    free(s);
    return NULL;
  }
  snprintf(s->db_path, n, "%s%c" MC_LIBRARY_DB_NAME, lib->dir, MC_PATH_SEP);
  mc_mutex_init(&s->mutex);
  mc_cond_init(&s->done);
  return lib->search = s;
}

void mc_search_free(mc_search *s) {
  if (!s) return;
  mc_mutex_lock(&s->mutex);
  s->closing = 1;
  /* Ends the statement it's in; one between statements still runs to the end of its build. */
  if (s->worker_db) sqlite3_interrupt(s->worker_db);
  mc_mutex_unlock(&s->mutex);
  if (s->joinable) mc_thread_join(&s->thread);
  mc_search_index_free(s->index);
  mc_search_index_free(s->ready);
  mc_cond_destroy(&s->done);
  mc_mutex_destroy(&s->mutex);
  free(s->db_path);
  free(s);
}

void mc_search_wait(mc_library *lib) {
  mc_search *s = lib->search;
  if (!s) return;
  mc_mutex_lock(&s->mutex);
  while (s->running) mc_cond_wait(&s->done, &s->mutex);
  adopt(s);
  mc_mutex_unlock(&s->mutex);
}

int mc_search_builds(const mc_library *lib) {
  return lib->search ? lib->search->builds : 0;
}

/* Whether a name may have changed since ix was built: a changelog row of an indexed table,
 * from any connection, after its seq. If nothing it holds changed, moves its seq on so those
 * rows aren't looked at again. *seq is the newest changelog seq. */
static mc_status stale(sqlite3 *db, mc_search_index *ix, int *out, int64_t *seq) {
  int64_t changed = 0;
  *out = 0;
  mc_status st = query_int64(db, "SELECT coalesce(max(seq), 0) FROM changelog", 0, seq);
  if (st != MC_OK || *seq == ix->seq) return st;
  st = query_int64(db,
                   "SELECT EXISTS (SELECT 1 FROM changelog WHERE seq > ?1"
                   " AND entity IN (" INDEXED_ENTITIES "))",
                   ix->seq, &changed);
  if (st == MC_OK && !changed) ix->seq = *seq;
  *out = (int)changed;
  return st;
}

/* Puts a finished build to use, then starts another if the library changed since the index in
 * use was built, or there's none. With wait and no index yet, waits for it. */
static mc_status refresh(mc_library *lib, mc_search *s, int wait) {
  mc_mutex_lock(&s->mutex);
  adopt(s);
  mc_mutex_unlock(&s->mutex);
  int changed = 1;
  int64_t seq = 0;
  mc_status st = s->index ? stale(lib->db, s->index, &changed, &seq) : MC_OK;
  if (st != MC_OK || !changed) return st;
  mc_mutex_lock(&s->mutex);
  st = start_build(s, seq);
  if (st == MC_OK && wait && !s->index) {
    while (s->running && !s->ready) mc_cond_wait(&s->done, &s->mutex);
    adopt(s);
    if (!s->index) st = MC_ERR_DB;
  }
  mc_mutex_unlock(&s->mutex);
  return st;
}

mc_status mc_library_search_prepare(mc_library *lib) {
  if (!lib) return MC_ERR_INVALID_ARG;
  mc_search *s = search_state(lib);
  return s ? refresh(lib, s, 0) : MC_ERR_NOMEM;
}

/* --- Matching --- */

/* Optimal string alignment distance (an adjacent swap is one edit) from a query word to each
 * prefix of a vocabulary word, a column per character of the vocabulary word. */
typedef struct matcher {
  const uint32_t *q;
  int n, max;
  int col[MAX_WORD + 1][MAX_WORD + 1]; /* col[j][i]: from q[0..i) to w[0..j) */
  int lo[MAX_WORD + 1];                /* the smallest of col[j] */
  int reach[MAX_WORD + 1];             /* from all of q to the closest of w[0..k), k <= j */
} matcher;

static void matcher_init(matcher *mt, const uint32_t *q, int n, int max) {
  mt->q = q, mt->n = n, mt->max = max;
  for (int i = 0; i <= n; i++) mt->col[0][i] = i;
  mt->lo[0] = 0;
  mt->reach[0] = n;
}

/* Fills column j, for w[j - 1], from the columns before it. */
static void column(matcher *mt, const uint32_t *w, int j) {
  const uint32_t *q = mt->q;
  int *c = mt->col[j], *p = mt->col[j - 1], *pp = j > 1 ? mt->col[j - 2] : NULL;
  int lo = c[0] = j;
  for (int i = 1; i <= mt->n; i++) {
    int d = p[i - 1] + (q[i - 1] != w[j - 1]);
    if (p[i] + 1 < d) d = p[i] + 1;
    if (c[i - 1] + 1 < d) d = c[i - 1] + 1;
    if (pp && i > 1 && q[i - 1] == w[j - 2] && q[i - 2] == w[j - 1] && pp[i - 2] + 1 < d)
      d = pp[i - 2] + 1;
    c[i] = d;
    if (d < lo) lo = d;
  }
  mt->lo[j] = lo;
  mt->reach[j] = c[mt->n] < mt->reach[j - 1] ? c[mt->n] : mt->reach[j - 1];
}

/* Whether no word starting with the j characters of column j can come within max: later
 * columns build on this one, or through a swap on the one before. With prefix, a word that
 * already did within those j characters still matches. */
static int too_far(const matcher *mt, int j, int prefix) {
  return mt->lo[j] > mt->max && mt->lo[j - 1] + 1 > mt->max && !(prefix && mt->reach[j] <= mt->max);
}

/* The first word after w that doesn't start with w's first j characters. */
static size_t skip_prefix(const mc_search_index *ix, size_t w, int j) {
  const uint32_t *p = ix->arena + ix->words[w].at;
  size_t lo = w + 1, hi = ix->nwords;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    const word *x = &ix->words[mid];
    if (x->len >= (uint32_t)j && memcmp(ix->arena + x->at, p, (size_t)j * sizeof *p) == 0) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}

typedef struct query {
  uint32_t words[MAX_QUERY_WORDS][MAX_WORD];
  int len[MAX_QUERY_WORDS];
  int count;
} query;

/* Adds query word qi's matches to every item that matched the words before it. */
static void match_word(mc_search_index *ix, matcher *mt, int qi, int prefix, size_t *nseen) {
  int n = mt->n, max = mt->max;
  uint32_t need = (1u << qi) - 1;
  uint32_t *touched = qi ? ix->touched : ix->seen;
  size_t nt = 0;
  const uint32_t *prev = NULL;
  int valid = 0; /* columns 1..valid are for prev's first valid characters */
  for (size_t w = 0; w < ix->nwords;) {
    const word *x = &ix->words[w];
    const uint32_t *s = ix->arena + x->at;
    int m = (int)x->len;
    /* A prefix further than max from q is no closer for being longer. */
    int lim = prefix && m > n + max ? n + max : m;
    int j = 0, cut = 0;
    while (j < valid && j < lim && s[j] == prev[j]) j++;
    while (j < lim && !cut) {
      column(mt, s, ++j);
      cut = too_far(mt, j, prefix);
    }
    prev = s, valid = j;
    if (cut) {
      w = skip_prefix(ix, w, j);
      continue;
    }
    int d = prefix ? mt->reach[lim] : mt->col[lim][n];
    if (d <= max) {
      double quality = d == 0 ? (m == n ? Q_EXACT : Q_PREFIX) : d == 1 ? Q_ONE_EDIT : Q_TWO_EDITS;
      for (uint32_t p = ix->post_start[w]; p < ix->post_start[w + 1]; p++) {
        uint32_t it = ix->post[p].item;
        if (ix->mask[it] != need) continue;
        double sc = quality * k_weight[ix->items[it].kind][ix->post[p].field];
        if (ix->best[it] == 0) touched[nt++] = it;
        if (sc > ix->best[it]) ix->best[it] = sc;
      }
    }
    w++;
  }
  for (size_t t = 0; t < nt; t++) {
    uint32_t it = touched[t];
    ix->total[it] += ix->best[it];
    ix->mask[it] |= 1u << qi;
    ix->best[it] = 0;
  }
  if (!qi) *nseen = nt;
}

/* --- Searching --- */

static int add_query_word(void *ctx, const uint32_t *w, int n) {
  query *q = ctx;
  if (q->count == MAX_QUERY_WORDS) return 1;
  memcpy(q->words[q->count], w, (size_t)n * sizeof *w);
  q->len[q->count++] = n;
  return 0;
}

typedef struct candidate {
  double score;
  const uint32_t *key;
  uint32_t key_len, item;
} candidate;

static int by_score(const void *a, const void *b) {
  const candidate *x = a, *y = b;
  if (x->score != y->score) return x->score < y->score ? 1 : -1;
  int c = compare(x->key, x->key_len, y->key, y->key_len);
  if (c) return c;
  return x->item < y->item ? -1 : x->item > y->item;
}

/* Puts c into top, which holds the best *n (at most limit) of a kind so far, best first. */
static void keep_best(candidate *top, size_t *n, size_t limit, const candidate *c) {
  if (*n == limit && by_score(c, &top[limit - 1]) >= 0) return;
  size_t i = *n < limit ? (*n)++ : limit - 1;
  for (; i > 0 && by_score(c, &top[i - 1]) < 0; i--) top[i] = top[i - 1];
  top[i] = *c;
}

/* The items in seen that every query word matched, scored, best first; clears the scratch.
 * With a small limit, only the best limit of each kind: a short list on every keystroke
 * shouldn't sort every match. NULL if out of memory. */
static candidate *rank_items(mc_search_index *ix, size_t nseen, int words, const uint32_t *qkey,
                             size_t qkey_len, size_t limit, size_t *count) {
  uint32_t full = (1u << words) - 1;
  int all = !limit || limit > 64 || limit >= nseen;
  /* All of them in one run, or a slice of limit per kind. */
  candidate *cands = malloc((all ? (nseen ? nseen : 1) : KIND_COUNT * limit) * sizeof *cands);
  size_t per_kind[KIND_COUNT] = {0}, nc = 0;
  for (size_t t = 0; t < nseen; t++) {
    uint32_t i = ix->seen[t];
    if (cands && ix->mask[i] == full) {
      const item *it = &ix->items[i];
      const uint32_t *key = ix->arena + it->key;
      double s = ix->total[i] + BONUS_SHORT / (1 + it->words);
      if (qkey_len && it->key_len >= qkey_len && memcmp(key, qkey, qkey_len * sizeof *qkey) == 0)
        s += it->key_len == qkey_len ? BONUS_WHOLE : BONUS_START;
      candidate c = {s, key, it->key_len, i};
      if (all) cands[nc++] = c;
      else keep_best(cands + it->kind * limit, &per_kind[it->kind], limit, &c);
    }
    ix->total[i] = 0;
    ix->mask[i] = 0;
  }
  if (!cands) return NULL;
  for (int kind = 0; !all && kind < KIND_COUNT; kind++) {
    memmove(cands + nc, cands + kind * limit, per_kind[kind] * sizeof *cands);
    nc += per_kind[kind];
  }
  qsort(cands, nc, sizeof *cands, by_score);
  *count = nc;
  return cands;
}

mc_status mc_library_search(mc_library *lib, const char *query_text, int limit_per_kind,
                            mc_search_results **out) {
  if (!lib || !query_text || !out) return MC_ERR_INVALID_ARG;
  *out = NULL;
  mc_search_results *res = calloc(1, sizeof *res);
  query *q = malloc(sizeof *q);
  matcher *mt = malloc(sizeof *mt);
  uint32_t qkey[MAX_QUERY_WORDS * (MAX_WORD + 1)];
  key_ctx k = {.buf = qkey, .cap = sizeof qkey / sizeof *qkey};
  candidate *cands = NULL;
  mc_status st = MC_ERR_NOMEM;
  if (!res || !q || !mt) goto done;

  q->count = 0;
  each_word(query_text, 0, add_query_word, q);
  each_word(query_text, 0, add_key_word, &k);
  size_t len = strlen(query_text);
  int typing = len && query_text[len - 1] != ' ' && query_text[len - 1] != '\t';
  st = MC_OK;
  if (!q->count) goto done;
  mc_search *s = search_state(lib);
  st = MC_ERR_NOMEM;
  if (!s) goto done;
  /* A failed check or build is only an error if there's no index at all to search. */
  st = refresh(lib, s, 1);
  if (!s->index) {
    if (st == MC_OK) st = MC_ERR_DB;
    goto done;
  }

  mc_search_index *ix = s->index;
  size_t nseen = 0, nc;
  for (int qi = 0; qi < q->count; qi++) {
    int n = q->len[qi];
    matcher_init(mt, q->words[qi], n, n <= 3 ? 0 : n <= 7 ? 1 : 2);
    match_word(ix, mt, qi, typing && qi == q->count - 1, &nseen);
  }
  st = MC_ERR_NOMEM;
  size_t limit = limit_per_kind > 0 ? (size_t)limit_per_kind : 0;
  if (!(cands = rank_items(ix, nseen, q->count, qkey, k.len, limit, &nc))) goto done;
  if (!(res->items = calloc(nc ? nc : 1, sizeof *res->items))) goto done;
  int per_kind[KIND_COUNT] = {0};
  st = MC_OK;
  for (size_t c = 0; c < nc && st == MC_OK; c++) {
    const item *it = &ix->items[cands[c].item];
    if (limit_per_kind > 0 && per_kind[it->kind] >= limit_per_kind) continue;
    per_kind[it->kind]++;
    mc_search_hit *h = &res->items[res->count++];
    h->kind = it->kind;
    memcpy(h->uid, it->uid, sizeof h->uid);
    memcpy(h->album_uid, it->album_uid, sizeof h->album_uid);
    h->name = mc_strdup(text_at(ix, it->name));
    h->artist = mc_strdup(text_at(ix, it->artist));
    h->album = mc_strdup(text_at(ix, it->album));
    h->is_smart = it->is_smart;
    h->score = cands[c].score;
    if (!h->name || (it->artist != NO_TEXT && !h->artist) || (it->album != NO_TEXT && !h->album))
      st = MC_ERR_NOMEM;
  }

done:
  free(q);
  free(mt);
  free(cands);
  if (st != MC_OK) {
    mc_search_results_free(res);
    return st;
  }
  *out = res;
  return MC_OK;
}

void mc_search_results_free(mc_search_results *results) {
  if (!results) return;
  for (int i = 0; i < results->count; i++) {
    free(results->items[i].name);
    free(results->items[i].artist);
    free(results->items[i].album);
  }
  free(results->items);
  free(results);
}

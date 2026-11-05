/* Smart lists: a rule (JSON) compiled to one SQL query each time the list is listed. Field
 * names map to fixed SQL expressions in k_fields and every value from the rule is a bound
 * parameter, so nothing from a rule is ever spliced into SQL. Every condition is coalesced to
 * 0 or 1, so "not" behaves as people expect where a value is missing: "not played within 180
 * days" includes tracks never played at all. The format is in docs/schema.md. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L /* localtime_r */
#endif

#include "musiccore/smartlist.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <yyjson.h>

#include "library/library_internal.h"
#include "musiccore/i18n.h"
#include "util/platform.h"

#define RULE_VERSION 1
#define MAX_DEPTH 32
#define DAY_MS 86400000LL
#define MAX_LIMIT_SECONDS 31536000000LL /* a thousand years */

typedef enum ftype { F_TEXT, F_INT, F_DURATION, F_DATE, F_BOOL, F_PLAYLIST, F_GROUP, F_FORMAT } ftype;

static const char *const k_type_names[] = {"text", "int",      "duration", "date",
                                           "bool", "playlist", "group",    "format"};

static const char *const k_text_ops[] = {"is", "is_not", "contains", "not_contains", "starts_with",
                                         "empty", "not_empty", NULL};
static const char *const k_int_ops[] = {"=", "!=", "<", "<=", ">", ">=", "between", "empty", "not_empty",
                                        NULL};
static const char *const k_date_ops[] = {"before", "after", "within", "not_within", "empty", "not_empty",
                                         NULL};
static const char *const k_bool_ops[] = {"is", NULL};
static const char *const k_member_ops[] = {"is", "is_not", NULL};

static const char *const *const k_type_ops[] = {k_text_ops, k_int_ops,    k_int_ops,    k_date_ops,
                                                k_bool_ops, k_member_ops, k_member_ops, k_member_ops};

/* A field's SQL for one subject. For text, int, duration and date fields expr is a value;
 * for the others it's a condition, with one ? for the value if it takes one. sort is what to
 * order by, if not expr. A field with wrap is true if it's true of any of the rows wrap
 * ranges over (an album's tracks tt, a credit's names cn): the condition goes inside wrap
 * and a closing parenthesis. */
typedef struct field_sql {
  const char *expr, *sort, *wrap;
} field_sql;

typedef struct field_def {
  const char *name;
  ftype type;
  field_sql sql[2]; /* by mc_smart_subject; expr NULL if the subject doesn't have it */
} field_def;

#define T_PLAYS "(SELECT coalesce(sum(p.play_count), 0) FROM track_plays p WHERE p.track_uid = t.uid)"
#define T_LAST "(SELECT max(p.last_played_ms) FROM track_plays p WHERE p.track_uid = t.uid)"
#define A_TRACKS " FROM tracks tt WHERE tt.album_uid = a.uid)"
#define A_PLAYS " FROM tracks tt JOIN track_plays p ON p.track_uid = tt.uid WHERE tt.album_uid = a.uid)"
#define A_FILES " FROM tracks tt JOIN files f ON f.track_uid = tt.uid WHERE tt.album_uid = a.uid"

/* An artist condition holds if it holds of the credit as shown ("A feat. B") or of any one of
 * its artists, so "artist is B" finds what B is featured on. The names as rows cn.name, for
 * a track (its own credit, else its album's), a track's album and an album. */
#define CREDIT_NAMES(shown, first, rows) \
  "EXISTS (SELECT 1 FROM (SELECT " shown " AS name UNION ALL SELECT " first \
  " UNION ALL SELECT x.name FROM " rows " JOIN artists x ON x.uid = c.artist_uid" \
  " WHERE c.position > 0) cn WHERE "
#define T_ARTIST_NAMES                                                                       \
  CREDIT_NAMES(TRACK_ARTIST_NAME, "coalesce(ar.name, aar.name)",                             \
               "(SELECT artist_uid, position FROM track_artists WHERE track_uid = t.uid"     \
               " UNION ALL SELECT artist_uid, position FROM album_artists"                   \
               " WHERE t.artist_uid IS NULL AND album_uid = t.album_uid) c")
#define T_ALBUM_ARTIST_NAMES                                         \
  CREDIT_NAMES("coalesce(al.artist_credit, aar.name)", "aar.name", \
               "(SELECT artist_uid, position FROM album_artists WHERE album_uid = t.album_uid) c")
#define A_ARTIST_NAMES                                             \
  CREDIT_NAMES("coalesce(a.artist_credit, ar.name)", "ar.name", \
               "(SELECT artist_uid, position FROM album_artists WHERE album_uid = a.uid) c")

static const field_def k_fields[] = {
  {"title", F_TEXT, {{"t.title", NULL, NULL}, {"a.title", NULL, NULL}}},
  {"artist",
   F_TEXT,
   {{"cn.name",
     "CASE WHEN ar.uid IS NOT NULL THEN coalesce(ar.sort_name, ar.name)"
     " ELSE coalesce(aar.sort_name, aar.name) END",
     T_ARTIST_NAMES},
    {"cn.name", "coalesce(ar.sort_name, ar.name)", A_ARTIST_NAMES}}},
  {"album_artist",
   F_TEXT,
   {{"cn.name", "coalesce(aar.sort_name, aar.name)", T_ALBUM_ARTIST_NAMES}, {NULL, NULL, NULL}}},
  {"album", F_TEXT, {{"al.title", NULL, NULL}, {NULL, NULL, NULL}}},
  {"genre",
   F_TEXT,
   {{"t.genre", NULL, NULL}, {"tt.genre", NULL, "EXISTS (SELECT 1 FROM tracks tt WHERE tt.album_uid = a.uid AND "}}},
  {"year", F_INT, {{"coalesce(t.year, al.year)", NULL, NULL}, {"a.year", NULL, NULL}}},
  {"duration", F_DURATION, {{"t.duration_ms", NULL, NULL}, {"(SELECT sum(tt.duration_ms)" A_TRACKS, NULL, NULL}}},
  {"track_no", F_INT, {{"t.track_no", NULL, NULL}, {NULL, NULL, NULL}}},
  {"disc_no", F_INT, {{"t.disc_no", NULL, NULL}, {NULL, NULL, NULL}}},
  {"track_count", F_INT, {{NULL, NULL, NULL}, {"(SELECT count(*)" A_TRACKS, NULL, NULL}}},
  {"added", F_DATE, {{"t.added_ms", NULL, NULL}, {"(SELECT max(tt.added_ms)" A_TRACKS, NULL, NULL}}},
  {"plays", F_INT, {{T_PLAYS, NULL, NULL}, {"(SELECT coalesce(sum(p.play_count), 0)" A_PLAYS, NULL, NULL}}},
  {"last_played", F_DATE, {{T_LAST, NULL, NULL}, {"(SELECT max(p.last_played_ms)" A_PLAYS, NULL, NULL}}},
  /* A single: a track alone on its album in the library, or on none; an album of one track. */
  {"singles",
   F_BOOL,
   {{"(t.album_uid IS NULL OR (SELECT count(*) FROM tracks st WHERE st.album_uid = t.album_uid) = 1)", NULL,
     NULL},
    {ALBUM_IS_SINGLE, NULL, NULL}}},
  {"on_device",
   F_BOOL,
   {{"EXISTS (SELECT 1 FROM files f WHERE f.track_uid = t.uid)", NULL, NULL},
    {"EXISTS (SELECT 1" A_FILES ")", NULL, NULL}}},
  {"format",
   F_FORMAT,
   {{"EXISTS (SELECT 1 FROM files f WHERE f.track_uid = t.uid AND f.format = ? COLLATE NOCASE)", NULL, NULL},
    {"EXISTS (SELECT 1" A_FILES " AND f.format = ? COLLATE NOCASE)", NULL, NULL}}},
  {"playlist",
   F_PLAYLIST,
   {{"EXISTS (SELECT 1 FROM playlist_entries e WHERE e.track_uid = t.uid AND e.playlist_uid = ?)", NULL, NULL},
    {NULL, NULL, NULL}}},
  {"group",
   F_GROUP,
   {{"EXISTS (SELECT 1 FROM album_group_entries g WHERE g.album_uid = t.album_uid AND g.group_uid = ?)", NULL,
     NULL},
    {"EXISTS (SELECT 1 FROM album_group_entries g WHERE g.album_uid = a.uid AND g.group_uid = ?)", NULL, NULL}}},
};
#define FIELD_COUNT ((int)(sizeof k_fields / sizeof k_fields[0]))

/* Without an order, or after it: tracks by album as the album grid shows them, then in album
 * order; albums as mc_library_list_albums() has them. uid last, so ties always come out the
 * same way. */
static const char *const k_default_order[] = {
  "coalesce(aar.sort_name, aar.name) COLLATE NOCASE, al.year,"
  " al.title COLLATE NOCASE, al.uid, coalesce(t.disc_no, 0), t.track_no IS NULL, t.track_no,"
  " t.title COLLATE NOCASE, t.uid",
  ALBUM_ORDER ", a.uid",
};
static const char *const k_uid_col[] = {"t.uid", "a.uid"};
static const char *const k_subject_names[] = {"tracks", "albums"};

static int sortable(ftype type) {
  return type == F_TEXT || type == F_INT || type == F_DURATION || type == F_DATE;
}

int mc_smart_field_count(mc_smart_subject subject) {
  int n = 0;
  mc_smart_field f;
  while (mc_smart_field_get(subject, n, &f)) n++;
  return n;
}

int mc_smart_field_get(mc_smart_subject subject, int i, mc_smart_field *out) {
  if ((unsigned)subject > MC_SMART_ALBUMS || i < 0 || !out) return 0;
  for (int k = 0; k < FIELD_COUNT; k++) {
    const field_sql *s = &k_fields[k].sql[subject];
    if (!s->expr || i--) continue;
    out->name = k_fields[k].name;
    out->type = k_type_names[k_fields[k].type];
    out->ops = k_type_ops[k_fields[k].type];
    out->sortable = sortable(k_fields[k].type) && !s->wrap;
    return 1;
  }
  return 0;
}

/* --- Compiling a rule --- */

typedef struct bind {
  const char *text; /* NULL for an integer */
  int64_t num;
} bind;

/* The query being built. The rule's document stays open until the statement is bound, since
 * text binds point into it. */
typedef struct query {
  mc_smart_subject subject;
  int64_t now_ms;
  yyjson_doc *doc;
  char *sql;
  size_t len, cap;
  bind *binds;
  int nbinds, bcap;
  mc_status st; /* the first error; once set, everything else is a no-op */
} query;

static void put(query *q, const char *s) {
  if (q->st != MC_OK) return;
  size_t n = strlen(s);
  if (q->len + n + 1 > q->cap) {
    size_t cap = q->cap ? q->cap : 1024;
    while (q->len + n + 1 > cap) cap *= 2;
    char *p = realloc(q->sql, cap);
    if (!p) {
      q->st = MC_ERR_NOMEM;
      return;
    }
    q->sql = p;
    q->cap = cap;
  }
  memcpy(q->sql + q->len, s, n + 1);
  q->len += n;
}

/* Adds a bound value; the ? for it must be written by the caller, in order. */
static void add_bind(query *q, const char *text, int64_t num) {
  if (q->st != MC_OK) return;
  if (q->nbinds == q->bcap) {
    int cap = q->bcap ? q->bcap * 2 : 8;
    bind *p = realloc(q->binds, (size_t)cap * sizeof *p);
    if (!p) {
      q->st = MC_ERR_NOMEM;
      return;
    }
    q->binds = p;
    q->bcap = cap;
  }
  q->binds[q->nbinds].text = text;
  q->binds[q->nbinds].num = num;
  q->nbinds++;
}

static void put_text(query *q, const char *text) {
  put(q, "?");
  add_bind(q, text, 0);
}

static void put_int(query *q, int64_t num) {
  put(q, "?");
  add_bind(q, NULL, num);
}

static void bad(query *q) {
  if (q->st == MC_OK) q->st = MC_ERR_FORMAT;
}

static void query_free(query *q) {
  yyjson_doc_free(q->doc);
  free(q->sql);
  free(q->binds);
}

/* An integer that fits in int64. */
static int get_int(yyjson_val *v, int64_t *out) {
  if (yyjson_is_sint(v)) {
    *out = yyjson_get_sint(v);
    return 1;
  }
  if (yyjson_is_uint(v) && yyjson_get_uint(v) <= (uint64_t)INT64_MAX) {
    *out = (int64_t)yyjson_get_uint(v);
    return 1;
  }
  return 0;
}

/* Whether obj has no keys other than the NULL-terminated allowed ones. */
static int only_keys(yyjson_val *obj, const char *const *allowed) {
  size_t idx, max;
  yyjson_val *key, *val;
  yyjson_obj_foreach(obj, idx, max, key, val) {
    (void)val;
    int ok = 0;
    for (const char *const *a = allowed; *a && !ok; a++) ok = yyjson_equals_str(key, *a);
    if (!ok) return 0;
  }
  return 1;
}

static const field_def *find_field(mc_smart_subject subject, yyjson_val *name) {
  if (!yyjson_is_str(name)) return NULL;
  for (int k = 0; k < FIELD_COUNT; k++)
    if (k_fields[k].sql[subject].expr && yyjson_equals_str(name, k_fields[k].name)) return &k_fields[k];
  return NULL;
}

static int op_is(yyjson_val *op, const char *name) {
  return yyjson_equals_str(op, name);
}

/* Writes a scalar field's condition for op, without negation: e is the field's value. Returns
 * 0 if op isn't one of the type's, or the value doesn't suit it. negate is set for the ops
 * written as the negation of another. */
static int put_scalar(query *q, ftype type, const char *e, yyjson_val *op, yyjson_val *value, int *negate) {
  *negate = 0;
  int64_t n, m;
  if (op_is(op, "empty") || op_is(op, "not_empty")) {
    if (value) return 0;
    *negate = op_is(op, "not_empty");
    put(q, type == F_TEXT ? "coalesce(" : "");
    put(q, e);
    put(q, type == F_TEXT ? ", '') = ''" : " IS NULL");
    return 1;
  }
  if (type == F_TEXT) {
    if (!yyjson_is_str(value)) return 0;
    const char *s = yyjson_get_str(value);
    if (op_is(op, "is") || op_is(op, "is_not")) {
      *negate = op_is(op, "is_not");
      put(q, "coalesce(");
      put(q, e);
      put(q, ", '') = ");
      put_text(q, s);
      put(q, " COLLATE NOCASE");
    } else if (op_is(op, "contains") || op_is(op, "not_contains") || op_is(op, "starts_with")) {
      *negate = op_is(op, "not_contains");
      put(q, "instr(lower(coalesce(");
      put(q, e);
      put(q, ", '')), lower(");
      put_text(q, s);
      put(q, op_is(op, "starts_with") ? ")) = 1" : ")) > 0");
    } else {
      return 0;
    }
    return 1;
  }
  if (type == F_INT || type == F_DURATION) {
    int64_t scale = type == F_DURATION ? 1000 : 1; /* seconds in the rule, ms in the table */
    static const char *const cmp[][2] = {{"=", " = "}, {"!=", " = "}, {"<", " < "},
                                         {"<=", " <= "}, {">", " > "}, {">=", " >= "}};
    for (size_t i = 0; i < sizeof cmp / sizeof cmp[0]; i++) {
      if (!op_is(op, cmp[i][0])) continue;
      if (!get_int(value, &n) || n > INT64_MAX / 1000 || n < INT64_MIN / 1000) return 0;
      *negate = i == 1; /* != is "not =", so it includes missing values */
      put(q, e);
      put(q, cmp[i][1]);
      put_int(q, n * scale);
      return 1;
    }
    if (!op_is(op, "between")) return 0;
    if (!yyjson_is_arr(value) || yyjson_arr_size(value) != 2 || !get_int(yyjson_arr_get(value, 0), &n) ||
        !get_int(yyjson_arr_get(value, 1), &m) || n > INT64_MAX / 1000 || m > INT64_MAX / 1000 ||
        n < INT64_MIN / 1000 || m < INT64_MIN / 1000)
      return 0;
    put(q, e);
    put(q, " BETWEEN ");
    put_int(q, n * scale);
    put(q, " AND ");
    put_int(q, m * scale);
    return 1;
  }
  /* F_DATE */
  if (op_is(op, "before") || op_is(op, "after")) {
    if (!get_int(value, &n)) return 0;
    put(q, e);
    put(q, op_is(op, "before") ? " < " : " > ");
    put_int(q, n);
    return 1;
  }
  if (op_is(op, "within") || op_is(op, "not_within")) {
    static const char *const days_only[] = {"days", NULL};
    if (!yyjson_is_obj(value) || !only_keys(value, days_only) || !get_int(yyjson_obj_get(value, "days"), &n) ||
        n < 0 || n > 1000000)
      return 0;
    *negate = op_is(op, "not_within");
    put(q, e);
    put(q, " >= ");
    put_int(q, q->now_ms - n * DAY_MS);
    return 1;
  }
  return 0;
}

static void put_leaf(query *q, yyjson_val *leaf) {
  static const char *const leaf_keys[] = {"field", "op", "value", NULL};
  const field_def *f = find_field(q->subject, yyjson_obj_get(leaf, "field"));
  yyjson_val *op = yyjson_obj_get(leaf, "op"), *value = yyjson_obj_get(leaf, "value");
  if (!f || !yyjson_is_str(op) || !only_keys(leaf, leaf_keys)) {
    bad(q);
    return;
  }
  const field_sql *s = &f->sql[q->subject];

  /* Conditions written as the negation of another go to a scratch query first, so the NOT can
   * go in front. */
  query cond = {.subject = q->subject, .now_ms = q->now_ms};
  int negate = 0, ok;
  if (f->type == F_BOOL) {
    ok = op_is(op, "is") && yyjson_is_bool(value);
    negate = ok && !yyjson_get_bool(value);
    put(&cond, s->expr);
  } else if (f->type == F_PLAYLIST || f->type == F_GROUP || f->type == F_FORMAT) {
    ok = (op_is(op, "is") || op_is(op, "is_not")) && yyjson_is_str(value);
    negate = op_is(op, "is_not");
    /* The template's one ? is the value. */
    const char *mark = strchr(s->expr, '?');
    size_t head = (size_t)(mark - s->expr);
    char *before = malloc(head + 1);
    if (!before) {
      cond.st = MC_ERR_NOMEM;
    } else {
      memcpy(before, s->expr, head);
      before[head] = '\0';
      put(&cond, before);
      free(before);
    }
    if (ok) put_text(&cond, yyjson_get_str(value));
    put(&cond, mark + 1);
  } else {
    ok = put_scalar(&cond, f->type, s->expr, op, value, &negate);
  }

  if (!ok) bad(q);
  if (cond.st != MC_OK && q->st == MC_OK) q->st = cond.st;
  put(q, negate ? "NOT " : "");
  if (s->wrap) {
    put(q, s->wrap);
    put(q, cond.sql ? cond.sql : "");
    put(q, ")");
  } else {
    put(q, "coalesce((");
    put(q, cond.sql ? cond.sql : "");
    put(q, "), 0)");
  }
  for (int i = 0; i < cond.nbinds; i++) add_bind(q, cond.binds[i].text, cond.binds[i].num);
  free(cond.sql);
  free(cond.binds);
}

/* Takes back what was written since the query was len long. */
static void unput(query *q, size_t len) {
  if (q->st != MC_OK || !q->sql) return;
  q->len = len;
  q->sql[len] = '\0';
}

/* Writes node's condition; returns 0, having written nothing, if it's ignored: a group with
 * nothing in it but ignored nodes, or a "not" around one. A group may have a "name", a string
 * that's only a label (a "not" may too if it's around a group, which then has none of its
 * own); in_not says node is a "not"'s. */
static int put_match(query *q, yyjson_val *node, int depth, int in_not) {
  if (q->st != MC_OK) return 0;
  if (depth > MAX_DEPTH || !yyjson_is_obj(node)) {
    bad(q);
    return 0;
  }
  yyjson_val *all = yyjson_obj_get(node, "all"), *any = yyjson_obj_get(node, "any");
  yyjson_val *not = yyjson_obj_get(node, "not");
  if (!all && !any && !not) {
    put_leaf(q, node);
    return 1;
  }
  yyjson_val *name = yyjson_obj_get(node, "name");
  int around_group = not && (yyjson_obj_get(not, "all") || yyjson_obj_get(not, "any"));
  if (yyjson_obj_size(node) != 1u + !!name ||
      (name && (!yyjson_is_str(name) || in_not || (not && !around_group)))) {
    bad(q);
    return 0;
  }
  size_t start = q->len;
  if (not) {
    put(q, "NOT (");
    if (!put_match(q, not, depth + 1, 1)) {
      unput(q, start);
      return 0;
    }
    put(q, ")");
    return 1;
  }
  yyjson_val *list = all ? all : any;
  if (!yyjson_is_arr(list)) {
    bad(q);
    return 0;
  }
  put(q, "(");
  int written = 0;
  size_t idx, max;
  yyjson_val *child;
  yyjson_arr_foreach(list, idx, max, child) {
    size_t before = q->len;
    if (written) put(q, all ? " AND " : " OR ");
    if (put_match(q, child, depth + 1, 0)) written = 1;
    else unput(q, before);
  }
  if (!written) {
    unput(q, start);
    return 0;
  }
  put(q, ")");
  return 1;
}

static void put_order(query *q, yyjson_val *order) {
  if (order && !yyjson_is_arr(order)) bad(q);
  size_t idx, max;
  yyjson_val *key;
  yyjson_arr_foreach(order, idx, max, key) {
    static const char *const sort_keys[] = {"field", "dir", NULL};
    static const char *const random_keys[] = {"field", "seed", NULL};
    yyjson_val *name = yyjson_obj_get(key, "field"), *dir = yyjson_obj_get(key, "dir");
    if (yyjson_equals_str(name, "random")) {
      int64_t seed;
      if (idx != max - 1 || !only_keys(key, random_keys) || !get_int(yyjson_obj_get(key, "seed"), &seed)) {
        bad(q);
        return;
      }
      put(q, "mc_shuffle_key(");
      put_int(q, seed);
      put(q, ", ");
      put(q, k_uid_col[q->subject]);
      put(q, "), ");
      continue;
    }
    const field_def *f = find_field(q->subject, name);
    const field_sql *s = f ? &f->sql[q->subject] : NULL;
    /* A wrapped field's expr only means something inside its wrap; its sort, if it has one,
     * stands alone. */
    if (!yyjson_is_obj(key) || !f || !sortable(f->type) || (s->wrap && !s->sort) ||
        !only_keys(key, sort_keys) ||
        (dir && !yyjson_equals_str(dir, "asc") && !yyjson_equals_str(dir, "desc"))) {
      bad(q);
      return;
    }
    put(q, s->sort ? s->sort : s->expr);
    put(q, f->type == F_TEXT ? " COLLATE NOCASE" : "");
    put(q, dir && yyjson_equals_str(dir, "desc") ? " DESC" : "");
    put(q, " NULLS LAST, ");
  }
  put(q, k_default_order[q->subject]);
}

/* A continuous mix's tracks are never picked, whatever the rule (edit.h); a smart album group
 * can pick the album. al as in TRACK_JOINS. */
#define TRACK_NOT_IN_MIX "NOT " TRACK_IN_MIX

/* What a compiled query selects: the list itself, one row of how many it picks, the albums
 * for its cover art (see mc_smart_prepare_art()), or one row if anything it picks has a file
 * here (mc_smart_has_playable()). */
typedef enum mode { M_LIST, M_COUNT, M_ART, M_PLAYABLE } mode;

/* Whether a track, or any track of an album, has a file here. */
#define TRACK_HERE(uid) "EXISTS (SELECT 1 FROM files f WHERE f.track_uid = " uid ")"
#define ALBUM_HERE(uid) \
  "EXISTS (SELECT 1 FROM tracks ft JOIN files f ON f.track_uid = ft.uid WHERE ft.album_uid = " uid ")"

/* The condition for what a rule picks, before any limit. */
static void put_picks(query *q, yyjson_val *match) {
  if (q->subject == MC_SMART_TRACKS) put(q, TRACK_NOT_IN_MIX " AND ");
  /* Without a match, or with one that's ignored, everything matches. */
  if (!match || !put_match(q, match, 0, 0)) put(q, "1");
}

/* The condition for what's left of a rule's picks under a limit_duration: in its order, those
 * whose running total is within seconds. The first is kept even if it's too long by itself,
 * so a limit doesn't empty a list. An album's length is its tracks'. */
static void put_within_duration(query *q, yyjson_val *match, yyjson_val *order, int64_t seconds) {
  int tracks = q->subject == MC_SMART_TRACKS;
  const char *length = tracks ? "coalesce(t.duration_ms, 0)" : "coalesce((SELECT sum(tt.duration_ms)" A_TRACKS ", 0)";
  put(q, k_uid_col[q->subject]);
  put(q, " IN (SELECT uid FROM (SELECT ");
  put(q, k_uid_col[q->subject]);
  put(q, " AS uid, ");
  put(q, length);
  put(q, " AS own, sum(");
  put(q, length);
  put(q, ") OVER (ORDER BY ");
  put_order(q, order);
  put(q, " ROWS UNBOUNDED PRECEDING) AS upto FROM ");
  put(q, tracks ? "tracks t" TRACK_JOINS : "albums a" ALBUM_JOINS);
  put(q, " WHERE ");
  put_picks(q, match);
  put(q, ") WHERE upto = own OR upto <= ");
  put_int(q, seconds * 1000);
  put(q, ")");
}

/* Compiles rule_json into q: the whole query, ready to prepare and bind. For M_COUNT the
 * order is checked but left out, since it only matters to which rows a limit keeps, not to
 * how many; so for M_PLAYABLE without a limit, which stops at the first row with a file
 * whatever the order. With a limit, M_PLAYABLE looks through the ordered rows it keeps. A
 * limit_duration is a condition of its own, with the order inside it. */
static mc_status compile(query *q, mc_smart_subject subject, const char *rule_json, int64_t now_ms, mode m) {
  memset(q, 0, sizeof *q);
  q->subject = subject;
  q->now_ms = now_ms;
  if ((unsigned)subject > MC_SMART_ALBUMS || !rule_json) return q->st = MC_ERR_INVALID_ARG;
  q->doc = yyjson_read(rule_json, strlen(rule_json), 0);
  yyjson_val *root = yyjson_doc_get_root(q->doc);
  static const char *const top_keys[] = {"v", "match", "order", "limit", "limit_duration", NULL};
  int64_t v = 0, limit = 0, seconds = 0;
  yyjson_val *limit_val = yyjson_obj_get(root, "limit"), *seconds_val = yyjson_obj_get(root, "limit_duration");
  if (!yyjson_is_obj(root) || !only_keys(root, top_keys) || !get_int(yyjson_obj_get(root, "v"), &v) ||
      v != RULE_VERSION || (limit_val && (!get_int(limit_val, &limit) || limit < 1)) ||
      (seconds_val && (limit_val || !get_int(seconds_val, &seconds) || seconds < 1 || seconds > MAX_LIMIT_SECONDS)))
    return q->st = MC_ERR_FORMAT;

  int count = m == M_COUNT, tracks = subject == MC_SMART_TRACKS;
  /* Only the rows a limit keeps are the rule's: they're picked, in order, first. */
  int picked = m == M_PLAYABLE && limit_val;
  if (count)
    put(q, tracks ? "SELECT count(*) FROM (SELECT 1 FROM tracks t" TRACK_JOINS
                  : "SELECT count(*) FROM (SELECT 1 FROM albums a" ALBUM_JOINS);
  else if (picked)
    put(q, tracks ? "SELECT 1 FROM (SELECT t.uid AS uid FROM tracks t" TRACK_JOINS
                  : "SELECT 1 FROM (SELECT a.uid AS uid FROM albums a" ALBUM_JOINS);
  else if (m == M_PLAYABLE)
    put(q, tracks ? "SELECT 1 FROM tracks t" TRACK_JOINS : "SELECT 1 FROM albums a" ALBUM_JOINS);
  else if (m == M_ART)
    put(q, subject == MC_SMART_TRACKS
             ? "SELECT pa.art_hash, " MC_ALBUM_FILE("pa") " FROM albums pa WHERE pa.art_hash IS NOT NULL"
               " AND pa.uid IN (SELECT t.album_uid FROM tracks t" TRACK_JOINS
             : "SELECT a.art_hash, " MC_ALBUM_FILE("a") " FROM albums a" ALBUM_JOINS);
  else
    put(q, subject == MC_SMART_TRACKS ? TRACK_COLS " FROM tracks t" TRACK_JOINS
                                      : ALBUM_COLS " FROM albums a" ALBUM_JOINS);
  put(q, " WHERE ");
  yyjson_val *match = yyjson_obj_get(root, "match");
  if (seconds_val) put_within_duration(q, match, yyjson_obj_get(root, "order"), seconds);
  else put_picks(q, match);
  if (m == M_PLAYABLE && !picked) put(q, tracks ? " AND " TRACK_HERE("t.uid") : " AND " ALBUM_HERE("a.uid"));
  if (count || (m == M_PLAYABLE && !picked)) {
    query order = {.subject = subject, .now_ms = now_ms};
    put_order(&order, yyjson_obj_get(root, "order"));
    if (order.st != MC_OK && q->st == MC_OK) q->st = order.st;
    free(order.sql);
    free(order.binds);
  } else {
    put(q, " ORDER BY ");
    put_order(q, yyjson_obj_get(root, "order"));
  }
  if (limit_val) {
    put(q, " LIMIT ");
    put_int(q, limit);
  }
  if (count || (m == M_ART && tracks)) put(q, ")");
  else if (picked) put(q, tracks ? ") picked WHERE " TRACK_HERE("picked.uid") : ") picked WHERE " ALBUM_HERE("picked.uid"));
  if (m == M_PLAYABLE) put(q, " LIMIT 1");
  return q->st;
}

/* Compiles and prepares rule_json; *stmt is NULL on failure. */
static mc_status prepare(mc_library *lib, mc_smart_subject subject, const char *rule_json, mode m,
                         sqlite3_stmt **stmt) {
  *stmt = NULL;
  query q;
  mc_status st = compile(&q, subject, rule_json, (int64_t)lib->hlc.wall_ms(), m);
  if (st == MC_OK && sqlite3_prepare_v2(lib->db, q.sql, -1, stmt, NULL) != SQLITE_OK) st = MC_ERR_DB;
  for (int i = 0; st == MC_OK && i < q.nbinds; i++) {
    int rc = q.binds[i].text ? sqlite3_bind_text(*stmt, i + 1, q.binds[i].text, -1, SQLITE_TRANSIENT)
                             : sqlite3_bind_int64(*stmt, i + 1, q.binds[i].num);
    if (rc != SQLITE_OK) st = MC_ERR_DB;
  }
  query_free(&q);
  if (st != MC_OK) {
    sqlite3_finalize(*stmt);
    *stmt = NULL;
  }
  return st;
}

mc_status mc_smart_validate(mc_smart_subject subject, const char *rule_json) {
  query q;
  mc_status st = compile(&q, subject, rule_json, 0, M_LIST);
  query_free(&q);
  return st;
}

mc_status mc_smart_preview_tracks(mc_library *lib, const char *rule_json, mc_track_list **out) {
  if (!lib || !rule_json || !out) return MC_ERR_INVALID_ARG;
  *out = NULL;
  sqlite3_stmt *stmt;
  mc_status st = prepare(lib, MC_SMART_TRACKS, rule_json, M_LIST, &stmt);
  return st == MC_OK ? mc_library_read_tracks(lib, stmt, out) : st;
}

mc_status mc_smart_preview_albums(mc_library *lib, const char *rule_json, mc_album_list **out) {
  if (!lib || !rule_json || !out) return MC_ERR_INVALID_ARG;
  *out = NULL;
  sqlite3_stmt *stmt;
  mc_status st = prepare(lib, MC_SMART_ALBUMS, rule_json, M_LIST, &stmt);
  return st == MC_OK ? mc_library_read_albums(lib, stmt, out) : st;
}

mc_status mc_smart_count(mc_library *lib, mc_smart_subject subject, const char *rule_json, int *out) {
  if (!lib || !rule_json || !out) return MC_ERR_INVALID_ARG;
  *out = 0;
  sqlite3_stmt *stmt;
  mc_status st = prepare(lib, subject, rule_json, M_COUNT, &stmt);
  if (st != MC_OK) return st;
  if (sqlite3_step(stmt) == SQLITE_ROW)
    *out = sqlite3_column_int(stmt, 0);
  else
    st = MC_ERR_DB;
  sqlite3_finalize(stmt);
  return st;
}

/* --- Describing a rule ---
 *
 * The rule is validated first, so the walk below can take its shape as given. Every piece is
 * a malloc'd string from a template in strings/<lang>.json; NULL means out of memory, and
 * passes up through every level. */

#define NO_STR MC_STR_COUNT

/* A field's words: its picker label, its name inside a condition, and its sort orders. */
typedef struct field_words {
  const char *name;
  enum mc_str_id label, in_text, asc, desc;
} field_words;

static const field_words k_field_words[] = {
  {"title", MC_STR_SMART_FIELD_TITLE, MC_STR_SMART_FIELD_TITLE_IN_TEXT, MC_STR_SMART_ORDER_TITLE_ASC,
   MC_STR_SMART_ORDER_TITLE_DESC},
  {"artist", MC_STR_SMART_FIELD_ARTIST, MC_STR_SMART_FIELD_ARTIST_IN_TEXT, MC_STR_SMART_ORDER_ARTIST_ASC,
   MC_STR_SMART_ORDER_ARTIST_DESC},
  {"album_artist", MC_STR_SMART_FIELD_ALBUM_ARTIST, MC_STR_SMART_FIELD_ALBUM_ARTIST_IN_TEXT,
   MC_STR_SMART_ORDER_ALBUM_ARTIST_ASC, MC_STR_SMART_ORDER_ALBUM_ARTIST_DESC},
  {"album", MC_STR_SMART_FIELD_ALBUM, MC_STR_SMART_FIELD_ALBUM_IN_TEXT, MC_STR_SMART_ORDER_ALBUM_ASC,
   MC_STR_SMART_ORDER_ALBUM_DESC},
  {"genre", MC_STR_SMART_FIELD_GENRE, MC_STR_SMART_FIELD_GENRE_IN_TEXT, MC_STR_SMART_ORDER_GENRE_ASC,
   MC_STR_SMART_ORDER_GENRE_DESC},
  {"year", MC_STR_SMART_FIELD_YEAR, MC_STR_SMART_FIELD_YEAR_IN_TEXT, MC_STR_SMART_ORDER_YEAR_ASC,
   MC_STR_SMART_ORDER_YEAR_DESC},
  {"duration", MC_STR_SMART_FIELD_DURATION, MC_STR_SMART_FIELD_DURATION_IN_TEXT, MC_STR_SMART_ORDER_DURATION_ASC,
   MC_STR_SMART_ORDER_DURATION_DESC},
  {"track_no", MC_STR_SMART_FIELD_TRACK_NO, MC_STR_SMART_FIELD_TRACK_NO_IN_TEXT, MC_STR_SMART_ORDER_TRACK_NO_ASC,
   MC_STR_SMART_ORDER_TRACK_NO_DESC},
  {"disc_no", MC_STR_SMART_FIELD_DISC_NO, MC_STR_SMART_FIELD_DISC_NO_IN_TEXT, MC_STR_SMART_ORDER_DISC_NO_ASC,
   MC_STR_SMART_ORDER_DISC_NO_DESC},
  {"track_count", MC_STR_SMART_FIELD_TRACK_COUNT, MC_STR_SMART_FIELD_TRACK_COUNT_IN_TEXT,
   MC_STR_SMART_ORDER_TRACK_COUNT_ASC, MC_STR_SMART_ORDER_TRACK_COUNT_DESC},
  {"added", MC_STR_SMART_FIELD_ADDED, MC_STR_SMART_FIELD_ADDED_IN_TEXT, MC_STR_SMART_ORDER_ADDED_ASC,
   MC_STR_SMART_ORDER_ADDED_DESC},
  {"plays", MC_STR_SMART_FIELD_PLAYS, MC_STR_SMART_FIELD_PLAYS_IN_TEXT, MC_STR_SMART_ORDER_PLAYS_ASC,
   MC_STR_SMART_ORDER_PLAYS_DESC},
  {"last_played", MC_STR_SMART_FIELD_LAST_PLAYED, MC_STR_SMART_FIELD_LAST_PLAYED_IN_TEXT,
   MC_STR_SMART_ORDER_LAST_PLAYED_ASC, MC_STR_SMART_ORDER_LAST_PLAYED_DESC},
  {"singles", MC_STR_SMART_FIELD_SINGLES, NO_STR, NO_STR, NO_STR},
  {"on_device", MC_STR_SMART_FIELD_ON_DEVICE, NO_STR, NO_STR, NO_STR},
  {"format", MC_STR_SMART_FIELD_FORMAT, NO_STR, NO_STR, NO_STR},
  {"playlist", MC_STR_SMART_FIELD_PLAYLIST, NO_STR, NO_STR, NO_STR},
  {"group", MC_STR_SMART_FIELD_GROUP, NO_STR, NO_STR, NO_STR},
  {"random", MC_STR_SMART_FIELD_RANDOM, NO_STR, MC_STR_SMART_ORDER_RANDOM, MC_STR_SMART_ORDER_RANDOM},
};

/* An operator's picker label, its condition, and the condition under a "not". */
typedef struct op_words {
  const char *op;
  enum mc_str_id label, cond, negated;
} op_words;

static const op_words k_op_words[] = {
  {"is", MC_STR_SMART_OP_IS, MC_STR_SMART_COND_IS, MC_STR_SMART_COND_IS_NOT},
  {"is_not", MC_STR_SMART_OP_IS_NOT, MC_STR_SMART_COND_IS_NOT, MC_STR_SMART_COND_IS},
  {"contains", MC_STR_SMART_OP_CONTAINS, MC_STR_SMART_COND_CONTAINS, MC_STR_SMART_COND_NOT_CONTAINS},
  {"not_contains", MC_STR_SMART_OP_NOT_CONTAINS, MC_STR_SMART_COND_NOT_CONTAINS, MC_STR_SMART_COND_CONTAINS},
  {"starts_with", MC_STR_SMART_OP_STARTS_WITH, MC_STR_SMART_COND_STARTS_WITH, MC_STR_SMART_COND_NOT_STARTS_WITH},
  {"empty", MC_STR_SMART_OP_EMPTY, MC_STR_SMART_COND_EMPTY, MC_STR_SMART_COND_NOT_EMPTY},
  {"not_empty", MC_STR_SMART_OP_NOT_EMPTY, MC_STR_SMART_COND_NOT_EMPTY, MC_STR_SMART_COND_EMPTY},
  {"=", MC_STR_SMART_OP_IS, MC_STR_SMART_COND_IS, MC_STR_SMART_COND_IS_NOT},
  {"!=", MC_STR_SMART_OP_IS_NOT, MC_STR_SMART_COND_IS_NOT, MC_STR_SMART_COND_IS},
  {"<", MC_STR_SMART_OP_LT, MC_STR_SMART_COND_LT, MC_STR_SMART_COND_NOT_LT},
  {"<=", MC_STR_SMART_OP_LE, MC_STR_SMART_COND_LE, MC_STR_SMART_COND_NOT_LE},
  {">", MC_STR_SMART_OP_GT, MC_STR_SMART_COND_GT, MC_STR_SMART_COND_NOT_GT},
  {">=", MC_STR_SMART_OP_GE, MC_STR_SMART_COND_GE, MC_STR_SMART_COND_NOT_GE},
  {"between", MC_STR_SMART_OP_BETWEEN, MC_STR_SMART_COND_BETWEEN, MC_STR_SMART_COND_NOT_BETWEEN},
  {"before", MC_STR_SMART_OP_BEFORE, MC_STR_SMART_COND_BEFORE, MC_STR_SMART_COND_NOT_BEFORE},
  {"after", MC_STR_SMART_OP_AFTER, MC_STR_SMART_COND_AFTER, MC_STR_SMART_COND_NOT_AFTER},
  {"within", MC_STR_SMART_OP_WITHIN, MC_STR_SMART_COND_WITHIN, MC_STR_SMART_COND_NOT_WITHIN},
  {"not_within", MC_STR_SMART_OP_NOT_WITHIN, MC_STR_SMART_COND_NOT_WITHIN, MC_STR_SMART_COND_WITHIN},
};

static const enum mc_str_id k_months[12] = {
  MC_STR_SMART_MONTH_JANUARY, MC_STR_SMART_MONTH_FEBRUARY, MC_STR_SMART_MONTH_MARCH,
  MC_STR_SMART_MONTH_APRIL,   MC_STR_SMART_MONTH_MAY,      MC_STR_SMART_MONTH_JUNE,
  MC_STR_SMART_MONTH_JULY,    MC_STR_SMART_MONTH_AUGUST,   MC_STR_SMART_MONTH_SEPTEMBER,
  MC_STR_SMART_MONTH_OCTOBER, MC_STR_SMART_MONTH_NOVEMBER, MC_STR_SMART_MONTH_DECEMBER,
};

static const field_words *words_of(const char *name) {
  for (size_t i = 0; name && i < sizeof k_field_words / sizeof k_field_words[0]; i++)
    if (strcmp(k_field_words[i].name, name) == 0) return &k_field_words[i];
  return NULL;
}

static const op_words *op_words_of(const char *op) {
  for (size_t i = 0; op && i < sizeof k_op_words / sizeof k_op_words[0]; i++)
    if (strcmp(k_op_words[i].op, op) == 0) return &k_op_words[i];
  return NULL;
}

const char *mc_smart_field_label(const char *field) {
  const field_words *w = words_of(field);
  return w ? mc_str(w->label) : NULL;
}

const char *mc_smart_op_label(const char *type, const char *op) {
  for (size_t t = 0; type && op && t < sizeof k_type_names / sizeof k_type_names[0]; t++) {
    if (strcmp(k_type_names[t], type) != 0) continue;
    for (const char *const *o = k_type_ops[t]; *o; o++)
      if (strcmp(*o, op) == 0) return mc_str(op_words_of(op)->label);
  }
  return NULL;
}

const char *mc_smart_order_label(const char *field, int desc) {
  const field_words *w = words_of(field);
  enum mc_str_id id = !w ? NO_STR : desc ? w->desc : w->asc;
  return id == NO_STR ? NULL : mc_str(id);
}

typedef struct describer {
  mc_library *lib;
  mc_smart_subject subject;
  mc_status st; /* a database error; running out of memory is a NULL string instead */
} describer;

static char *fmt1(enum mc_str_id id, mc_arg a) {
  return mc_format_alloc(mc_str(id), a);
}

static char *number(int64_t n) {
  return mc_format_alloc("{0}", MC_D(n));
}

/* Seconds as m:ss, or h:mm:ss from an hour. */
static char *duration(int64_t s) {
  char buf[48];
  const char *sign = s < 0 ? "-" : "";
  uint64_t u = s < 0 ? 0 - (uint64_t)s : (uint64_t)s;
  if (u >= 3600)
    snprintf(buf, sizeof buf, "%s%llu:%02u:%02u", sign, (unsigned long long)(u / 3600), (unsigned)(u / 60 % 60),
             (unsigned)(u % 60));
  else
    snprintf(buf, sizeof buf, "%s%u:%02u", sign, (unsigned)(u / 60), (unsigned)(u % 60));
  return mc_strdup(buf);
}

/* A limit_duration in hours and minutes, a part of a minute counting as one. */
static char *limit_length(int64_t seconds) {
  int64_t minutes = (seconds + 59) / 60;
  if (minutes < 60) return mc_format_alloc(mc_str(MC_STR_DURATION_MINUTES), MC_D(minutes));
  return mc_format_alloc(mc_str(MC_STR_DURATION_HOURS_MINUTES), MC_D(minutes / 60), MC_D(minutes % 60));
}

/* A Unix ms time as a date in local time. */
static char *date(int64_t ms) {
  int64_t secs = ms / 1000 - (ms % 1000 < 0); /* rounded down, before 1970 too */
  time_t t = (time_t)secs;
  struct tm tm;
#ifdef _WIN32
  int ok = (int64_t)t == secs && localtime_s(&tm, &t) == 0;
#else
  int ok = (int64_t)t == secs && localtime_r(&t, &tm) != NULL;
#endif
  if (!ok || tm.tm_mon < 0 || tm.tm_mon > 11) return mc_strdup(mc_str(MC_STR_UNKNOWN_VALUE));
  return mc_format_alloc(mc_str(MC_STR_SMART_DATE), MC_D(tm.tm_mday), MC_S(mc_str(k_months[tm.tm_mon])),
                         MC_D(tm.tm_year + 1900LL));
}

/* {"days": n} as weeks where it's a whole number of them, from 2, else days. */
static char *days(int64_t n) {
  if (n >= 14 && n % 7 == 0) return mc_format_alloc(mc_str_plural(MC_STR_SMART_WEEKS, (long)(n / 7)), MC_D(n / 7));
  return mc_format_alloc(mc_str_plural(MC_STR_SMART_DAYS, (long)n), MC_D(n));
}

/* A value of a text, int, duration or date field, as it reads in a condition. */
static char *scalar_value(ftype type, const char *op, yyjson_val *v) {
  int64_t n = 0;
  if (type == F_TEXT) return fmt1(MC_STR_SMART_QUOTED, MC_S(yyjson_get_str(v)));
  get_int(type == F_DATE && yyjson_is_obj(v) ? yyjson_obj_get(v, "days") : v, &n);
  if (type == F_DURATION) return duration(n);
  if (type == F_DATE) return strstr(op, "within") ? days(n) : date(n);
  return number(n);
}

/* The name of the playlist or album group uid; NULL with st still MC_OK if there's none. */
static char *member_name(describer *d, ftype type, const char *uid) {
  sqlite3_stmt *stmt;
  const char *sql = type == F_PLAYLIST ? "SELECT name FROM playlists WHERE uid = ?1"
                                       : "SELECT name FROM album_groups WHERE uid = ?1";
  if (sqlite3_prepare_v2(d->lib->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
    d->st = MC_ERR_DB;
    return NULL;
  }
  sqlite3_bind_text(stmt, 1, uid, -1, SQLITE_STATIC);
  int rc = sqlite3_step(stmt);
  char *name = NULL;
  if (rc == SQLITE_ROW) name = fmt1(MC_STR_SMART_QUOTED, MC_S((const char *)sqlite3_column_text(stmt, 0)));
  else if (rc != SQLITE_DONE) d->st = MC_ERR_DB;
  sqlite3_finalize(stmt);
  return name;
}

static char *describe_leaf(describer *d, yyjson_val *leaf, int negated) {
  const field_def *f = find_field(d->subject, yyjson_obj_get(leaf, "field"));
  const char *op = yyjson_get_str(yyjson_obj_get(leaf, "op"));
  yyjson_val *value = yyjson_obj_get(leaf, "value");
  /* Whether the condition reads as the positive one, for the types with a fixed sentence each. */
  int yes = (f->type == F_BOOL ? yyjson_get_bool(value) : strcmp(op, "is") == 0) != negated;

  if (f->type == F_BOOL) {
    if (strcmp(f->name, "on_device") == 0)
      return mc_strdup(mc_str(yes ? MC_STR_SMART_COND_ON_DEVICE : MC_STR_SMART_COND_NOT_ON_DEVICE));
    if (d->subject == MC_SMART_TRACKS)
      return mc_strdup(mc_str(yes ? MC_STR_SMART_COND_SINGLE : MC_STR_SMART_COND_NOT_SINGLE));
    return mc_strdup(mc_str(yes ? MC_STR_SMART_COND_SINGLES_ALBUM : MC_STR_SMART_COND_NOT_SINGLES_ALBUM));
  }
  if (f->type == F_FORMAT) {
    char *upper = mc_strdup(yyjson_get_str(value));
    if (!upper) return NULL;
    for (char *c = upper; *c; c++)
      if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 'a' + 'A');
    char *s = fmt1(yes ? MC_STR_SMART_COND_FORMAT : MC_STR_SMART_COND_NOT_FORMAT, MC_S(upper));
    free(upper);
    return s;
  }
  if (f->type == F_PLAYLIST || f->type == F_GROUP) {
    int playlist = f->type == F_PLAYLIST;
    char *name = member_name(d, f->type, yyjson_get_str(value));
    if (d->st != MC_OK) return NULL;
    if (!name) {
      enum mc_str_id id = playlist ? (yes ? MC_STR_SMART_COND_IN_DELETED_PLAYLIST
                                          : MC_STR_SMART_COND_NOT_IN_DELETED_PLAYLIST)
                                   : (yes ? MC_STR_SMART_COND_IN_DELETED_GROUP : MC_STR_SMART_COND_NOT_IN_DELETED_GROUP);
      return mc_strdup(mc_str(id));
    }
    enum mc_str_id id = playlist ? (yes ? MC_STR_SMART_COND_IN_PLAYLIST : MC_STR_SMART_COND_NOT_IN_PLAYLIST)
                                 : (yes ? MC_STR_SMART_COND_IN_GROUP : MC_STR_SMART_COND_NOT_IN_GROUP);
    char *s = fmt1(id, MC_S(name));
    free(name);
    return s;
  }

  int empty = strcmp(op, "empty") == 0, not_empty = strcmp(op, "not_empty") == 0;
  if (strcmp(f->name, "last_played") == 0 && (empty || not_empty))
    return mc_strdup(mc_str(empty != negated ? MC_STR_SMART_COND_NEVER_PLAYED : MC_STR_SMART_COND_PLAYED));
  const op_words *w = op_words_of(op);
  const char *tmpl = mc_str(negated ? w->negated : w->cond);
  const char *field = mc_str(words_of(f->name)->in_text);
  if (empty || not_empty) return mc_format_alloc(tmpl, MC_S(field));
  char *a, *b, *s;
  if (strcmp(op, "between") == 0) {
    a = scalar_value(f->type, op, yyjson_arr_get(value, 0));
    b = scalar_value(f->type, op, yyjson_arr_get(value, 1));
    s = a && b ? mc_format_alloc(tmpl, MC_S(field), MC_S(a), MC_S(b)) : NULL;
    free(b);
  } else {
    a = scalar_value(f->type, op, value);
    s = a ? mc_format_alloc(tmpl, MC_S(field), MC_S(a)) : NULL;
  }
  free(a);
  return s;
}

/* Joins n parts (n >= 2) into a list with "and" or "or", freeing them. */
static char *join(char **parts, int n, int all) {
  enum mc_str_id two = all ? MC_STR_SMART_AND_TWO : MC_STR_SMART_OR_TWO;
  enum mc_str_id more = all ? MC_STR_SMART_AND_MORE : MC_STR_SMART_OR_MORE;
  enum mc_str_id last = all ? MC_STR_SMART_AND_LAST : MC_STR_SMART_OR_LAST;
  char *acc = parts[0];
  parts[0] = NULL;
  for (int i = 1; i < n && acc; i++) {
    enum mc_str_id id = n == 2 ? two : i == n - 1 ? last : more;
    char *next = mc_format_alloc(mc_str(id), MC_S(acc), MC_S(parts[i]));
    free(acc);
    acc = next;
  }
  for (int i = 0; i < n; i++) free(parts[i]);
  return acc;
}

/* Whether node is left out of the rule, as put_match() leaves it out: a group with nothing in
 * it but ignored nodes, or a "not" around one. */
static int ignored(yyjson_val *node) {
  yyjson_val *not = yyjson_obj_get(node, "not");
  if (not) return ignored(not);
  yyjson_val *all = yyjson_obj_get(node, "all"), *any = yyjson_obj_get(node, "any");
  if (!all && !any) return 0;
  size_t idx, max;
  yyjson_val *child;
  yyjson_arr_foreach(all ? all : any, idx, max, child) {
    if (!ignored(child)) return 0;
  }
  return 1;
}

/* A group's name with the spaces around it trimmed (malloc'd), or NULL if it has none or it's
 * blank. */
static char *group_name(yyjson_val *node) {
  const char *s = yyjson_get_str(yyjson_obj_get(node, "name"));
  if (!s) return NULL;
  size_t n = strlen(s);
  while (*s == ' ' || (*s >= '\t' && *s <= '\r')) s++, n--;
  while (n && (s[n - 1] == ' ' || (s[n - 1] >= '\t' && s[n - 1] <= '\r'))) n--;
  if (!n) return NULL;
  char *name = malloc(n + 1);
  if (name) {
    memcpy(name, s, n);
    name[n] = '\0';
  }
  return name;
}

/* node as a phrase. nested says it's inside a group, so a group of its own needs
 * parentheses, and a named one reads as its name. A "not" goes into the condition it's around
 * ("isn't", "doesn't contain"), and around a group reads as "none of" or "not all of". Ignored
 * nodes are left out. */
static char *describe_match(describer *d, yyjson_val *node, int negated, int nested) {
  yyjson_val *not = yyjson_obj_get(node, "not");
  yyjson_val *all = yyjson_obj_get(node, "all"), *any = yyjson_obj_get(node, "any");
  if (!not && !all && !any) return describe_leaf(d, node, negated);
  if (nested && yyjson_obj_get(node, "name")) {
    char *name = group_name(node);
    if (name) {
      char *quoted = fmt1(MC_STR_SMART_QUOTED, MC_S(name));
      char *s = quoted ? fmt1(negated ? MC_STR_SMART_NOT_NAMED : MC_STR_SMART_NAMED, MC_S(quoted)) : NULL;
      free(quoted);
      free(name);
      return s;
    }
  }
  if (not) return describe_match(d, not, !negated, nested);
  yyjson_val *list = all ? all : any;
  int n = 0;
  size_t idx, max;
  yyjson_val *child, *only = NULL;
  yyjson_arr_foreach(list, idx, max, child) {
    if (!ignored(child)) n++, only = child;
  }
  if (n == 1) return describe_match(d, only, negated, nested);
  char **parts = calloc((size_t)n, sizeof *parts);
  if (!parts) return NULL;
  int ok = 1, k = 0;
  yyjson_arr_foreach(list, idx, max, child) {
    if (ok && !ignored(child) && !(parts[k++] = describe_match(d, child, 0, 1))) ok = 0;
  }
  char *joined = NULL;
  if (ok) {
    joined = join(parts, n, !!all);
  } else {
    for (int i = 0; i < n; i++) free(parts[i]);
  }
  free(parts);
  if (!joined || (!negated && !nested)) return joined;
  enum mc_str_id wrap = !negated ? MC_STR_SMART_PARENS : all ? MC_STR_SMART_NOT_ALL_OF : MC_STR_SMART_NONE_OF;
  char *s = fmt1(wrap, MC_S(joined));
  free(joined);
  return s;
}

/* The order keys, "then"-joined, or NULL with *none set if there are none. */
static char *describe_order(yyjson_val *order, int *none) {
  char *acc = NULL;
  *none = 1;
  size_t idx, max;
  yyjson_val *key;
  yyjson_arr_foreach(order, idx, max, key) {
    yyjson_val *dir = yyjson_obj_get(key, "dir");
    const char *label = mc_smart_order_label(yyjson_get_str(yyjson_obj_get(key, "field")),
                                             dir && yyjson_equals_str(dir, "desc"));
    char *next = idx ? mc_format_alloc(mc_str(MC_STR_SMART_ORDER_THEN), MC_S(acc), MC_S(label))
                     : mc_strdup(label ? label : "");
    free(acc);
    *none = 0;
    if (!(acc = next)) return NULL;
  }
  return acc;
}

mc_status mc_smart_describe(mc_library *lib, mc_smart_subject subject, const char *rule_json, char **out) {
  if (!lib || !out) return MC_ERR_INVALID_ARG;
  *out = NULL;
  mc_status st = mc_smart_validate(subject, rule_json);
  if (st != MC_OK) return st;
  yyjson_doc *doc = yyjson_read(rule_json, strlen(rule_json), 0);
  if (!doc) return MC_ERR_NOMEM; /* it parsed a moment ago */
  yyjson_val *root = yyjson_doc_get_root(doc);
  describer d = {lib, subject, MC_OK};
  int tracks = subject == MC_SMART_TRACKS;

  /* The parts, joined with " · ": what matches, the order, the limit. */
  char *parts[3] = {NULL, NULL, NULL};
  int nparts = 0, ok = 1;
  yyjson_val *match = yyjson_obj_get(root, "match");
  if (!match || ignored(match)) {
    ok = !!(parts[nparts++] = mc_strdup(mc_str(tracks ? MC_STR_SMART_ALL_TRACKS : MC_STR_SMART_ALL_ALBUMS)));
  } else {
    char *conds = describe_match(&d, match, 0, 0);
    ok = conds && (parts[nparts++] = fmt1(tracks ? MC_STR_SMART_TRACKS_WHERE : MC_STR_SMART_ALBUMS_WHERE,
                                          MC_S(conds)));
    free(conds);
  }
  int none;
  if (ok) {
    char *order = describe_order(yyjson_obj_get(root, "order"), &none);
    if (order) parts[nparts++] = order;
    else ok = none;
  }
  int64_t limit;
  if (ok && get_int(yyjson_obj_get(root, "limit"), &limit))
    ok = !!(parts[nparts++] = mc_format_alloc(mc_str_plural(tracks ? MC_STR_TRACKS : MC_STR_COUNT_ALBUMS, (long)limit),
                                              MC_D(limit)));
  if (ok && get_int(yyjson_obj_get(root, "limit_duration"), &limit)) {
    char *length = limit_length(limit);
    ok = length && (parts[nparts++] = fmt1(MC_STR_SMART_LIMIT_OF_MUSIC, MC_S(length)));
    free(length);
  }
  yyjson_doc_free(doc);

  const char *sep = mc_str(MC_STR_SEPARATOR_DOT);
  size_t len = 1;
  for (int i = 0; ok && i < nparts; i++) len += strlen(parts[i]) + strlen(sep);
  char *s = ok ? malloc(len) : NULL;
  if (s) {
    s[0] = '\0';
    for (int i = 0; i < nparts; i++) {
      if (i) strcat(s, sep);
      strcat(s, parts[i]);
    }
  }
  for (int i = 0; i < 3; i++) free(parts[i]);
  if (d.st != MC_OK) {
    free(s);
    return d.st;
  }
  if (!s) return MC_ERR_NOMEM;
  *out = s;
  return MC_OK;
}

/* --- Saved lists --- */

/* The example lists library.c's v10 seeds, and what each is called in the language in use. */
static const struct {
  const char *uid, *seeded;
  enum mc_str_id name;
} k_examples[] = {
  {"5e1ec7ed-0000-4000-8000-000000000001", "Recently added", MC_STR_SMART_PRESET_RECENT},
  {"5e1ec7ed-0000-4000-8000-000000000002", "Most played", MC_STR_SMART_EXAMPLE_MOST_PLAYED},
  {"5e1ec7ed-0000-4000-8000-000000000003", "Never played", MC_STR_SMART_EXAMPLE_NEVER_PLAYED},
  {"5e1ec7ed-0000-4000-8000-000000000004", "Recently added albums", MC_STR_SMART_PRESET_RECENT_ALBUMS},
  {"5e1ec7ed-0000-4000-8000-000000000005", "Random albums", MC_STR_SMART_EXAMPLE_RANDOM_ALBUMS},
};

const char *mc_smart_shown_name(const char *uid, const char *name) {
  for (size_t i = 0; uid && name && i < sizeof k_examples / sizeof k_examples[0]; i++)
    if (!strcmp(uid, k_examples[i].uid) && !strcmp(name, k_examples[i].seeded)) return mc_str(k_examples[i].name);
  return name;
}

void mc_smart_sql_shown_name(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  (void)argc;
  const char *name = mc_smart_shown_name((const char *)sqlite3_value_text(argv[0]),
                                         (const char *)sqlite3_value_text(argv[1]));
  if (name) sqlite3_result_text(ctx, name, -1, SQLITE_TRANSIENT);
  else sqlite3_result_null(ctx);
}

static int parse_subject(const unsigned char *s, mc_smart_subject *out) {
  for (int i = 0; s && i <= MC_SMART_ALBUMS; i++)
    if (strcmp((const char *)s, k_subject_names[i]) == 0) {
      *out = (mc_smart_subject)i;
      return 1;
    }
  return 0;
}

/* Reads a saved list's subject and rule (malloc'd). */
static mc_status load(mc_library *lib, const char *uid, mc_smart_subject *subject, char **rule) {
  *rule = NULL;
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(lib->db, "SELECT subject, rule FROM smart_lists WHERE uid = ?1", -1, &stmt, NULL) !=
      SQLITE_OK)
    return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, uid, -1, SQLITE_STATIC);
  int rc = sqlite3_step(stmt);
  mc_status st = rc == SQLITE_ROW ? MC_OK : rc == SQLITE_DONE ? MC_ERR_NOT_FOUND : MC_ERR_DB;
  /* A subject this build doesn't know came from a newer app. */
  if (st == MC_OK && !parse_subject(sqlite3_column_text(stmt, 0), subject)) st = MC_ERR_FORMAT;
  if (st == MC_OK && !(*rule = mc_strdup((const char *)sqlite3_column_text(stmt, 1)))) st = MC_ERR_NOMEM;
  sqlite3_finalize(stmt);
  return st;
}

mc_status mc_library_list_smart_tracks(mc_library *lib, const char *smart_uid, mc_track_list **out) {
  if (!lib || !smart_uid || !out) return MC_ERR_INVALID_ARG;
  *out = NULL;
  mc_smart_subject subject;
  char *rule;
  mc_status st = load(lib, smart_uid, &subject, &rule);
  if (st == MC_OK) st = subject == MC_SMART_TRACKS ? mc_smart_preview_tracks(lib, rule, out) : MC_ERR_INVALID_ARG;
  free(rule);
  return st;
}

mc_status mc_library_list_smart_albums(mc_library *lib, const char *smart_uid, mc_album_list **out) {
  if (!lib || !smart_uid || !out) return MC_ERR_INVALID_ARG;
  *out = NULL;
  mc_smart_subject subject;
  char *rule;
  mc_status st = load(lib, smart_uid, &subject, &rule);
  if (st == MC_OK) st = subject == MC_SMART_ALBUMS ? mc_smart_preview_albums(lib, rule, out) : MC_ERR_INVALID_ARG;
  free(rule);
  return st;
}

/* Whether a rule holds a track with a file here, or an album with one: M_PLAYABLE gives a
 * row if so, without listing (or sorting) everything the rule picks. */
static mc_status rule_has_playable(mc_library *lib, mc_smart_subject subject, const char *rule, int *out) {
  *out = 0;
  sqlite3_stmt *stmt;
  mc_status st = prepare(lib, subject, rule, M_PLAYABLE, &stmt);
  if (st != MC_OK) return st;
  int rc = sqlite3_step(stmt);
  *out = rc == SQLITE_ROW;
  if (rc != SQLITE_ROW && rc != SQLITE_DONE) st = MC_ERR_DB;
  sqlite3_finalize(stmt);
  return st;
}

/* Whether this device has any file at all, into *out. */
static mc_status any_file_here(mc_library *lib, int *out) {
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(lib->db, "SELECT 1 FROM files LIMIT 1", -1, &stmt, NULL) != SQLITE_OK) return MC_ERR_DB;
  int rc = sqlite3_step(stmt);
  *out = rc == SQLITE_ROW;
  sqlite3_finalize(stmt);
  return rc == SQLITE_ROW || rc == SQLITE_DONE ? MC_OK : MC_ERR_DB;
}

/* Whether MC_LIST_PLAYABLE keeps a list with this rule: it has something here, or it's empty
 * everywhere rather than only here, on a device that has some music to show it beside. */
static mc_status rule_listed_here(mc_library *lib, mc_smart_subject subject, const char *rule, int *out) {
  mc_status st = rule_has_playable(lib, subject, rule, out);
  if (st != MC_OK || *out) return st;
  int picks = 0;
  if ((st = mc_smart_count(lib, subject, rule, &picks)) != MC_OK) return st;
  return picks ? MC_OK : any_file_here(lib, out);
}

/* Runs test on saved smart list smart_uid's rule. One that can't be evaluated is a no. */
static mc_status test_saved(mc_library *lib, const char *smart_uid, int *out,
                            mc_status (*test)(mc_library *, mc_smart_subject, const char *, int *)) {
  *out = 0;
  mc_smart_subject subject;
  char *rule;
  mc_status st = load(lib, smart_uid, &subject, &rule);
  if (st == MC_OK) st = test(lib, subject, rule, out);
  free(rule);
  if (st == MC_ERR_FORMAT || st == MC_ERR_INVALID_ARG) {
    *out = 0;
    return MC_OK;
  }
  return st;
}

mc_status mc_smart_has_playable(mc_library *lib, const char *smart_uid, int *out) {
  return test_saved(lib, smart_uid, out, rule_has_playable);
}

mc_status mc_smart_listed_here(mc_library *lib, const char *smart_uid, int *out) {
  return test_saved(lib, smart_uid, out, rule_listed_here);
}

mc_status mc_smart_prepare_art(mc_library *lib, const char *smart_uid, mc_smart_subject *subject,
                               sqlite3_stmt **stmt) {
  *stmt = NULL;
  char *rule;
  mc_status st = load(lib, smart_uid, subject, &rule);
  if (st == MC_OK) st = prepare(lib, *subject, rule, M_ART, stmt);
  free(rule);
  return st;
}

/* Runs one write of up to three text parameters. MC_ERR_NOT_FOUND if it changed no row. */
static mc_status write_one(mc_library *lib, const char *sql, const char *p1, const char *p2, const char *p3) {
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(lib->db, sql, -1, &stmt, NULL) != SQLITE_OK) return MC_ERR_DB;
  const char *params[] = {p1, p2, p3};
  for (int i = 0; i < 3; i++)
    if (params[i]) sqlite3_bind_text(stmt, i + 1, params[i], -1, SQLITE_STATIC);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) return MC_ERR_DB;
  return sqlite3_changes(lib->db) ? MC_OK : MC_ERR_NOT_FOUND;
}

static mc_status write_txn(mc_library *lib, const char *sql, const char *p1, const char *p2, const char *p3) {
  mc_status st = mc_library_begin_write(lib);
  if (st != MC_OK) return st;
  if ((st = write_one(lib, sql, p1, p2, p3)) == MC_OK) st = mc_library_commit(lib);
  if (st != MC_OK) mc_library_rollback(lib);
  return st;
}

mc_status mc_smart_create(mc_library *lib, const char *name, mc_smart_subject subject, const char *rule_json,
                          char out_uid[MC_UID_STRLEN]) {
  if (!lib || !name || !*name || (unsigned)subject > MC_SMART_ALBUMS || !rule_json || !out_uid)
    return MC_ERR_INVALID_ARG;
  char uid[MC_UID_STRLEN];
  mc_status st = mc_smart_validate(subject, rule_json);
  if (st == MC_OK) st = mc_uid_new(uid);
  if (st == MC_OK) {
    /* The subject is a constant, never from the caller's text. */
    const char *sql = subject == MC_SMART_TRACKS
                        ? "INSERT INTO smart_lists (uid, name, subject, rule) VALUES (?1, ?2, 'tracks', ?3)"
                        : "INSERT INTO smart_lists (uid, name, subject, rule) VALUES (?1, ?2, 'albums', ?3)";
    st = write_txn(lib, sql, uid, name, rule_json);
  }
  if (st == MC_OK) memcpy(out_uid, uid, MC_UID_STRLEN);
  return st;
}

mc_status mc_smart_rename(mc_library *lib, const char *smart_uid, const char *name) {
  if (!lib || !smart_uid || !name || !*name) return MC_ERR_INVALID_ARG;
  return write_txn(lib, "UPDATE smart_lists SET name = ?2 WHERE uid = ?1", smart_uid, name, NULL);
}

mc_status mc_smart_delete(mc_library *lib, const char *smart_uid) {
  if (!lib || !smart_uid) return MC_ERR_INVALID_ARG;
  return write_txn(lib, "DELETE FROM smart_lists WHERE uid = ?1", smart_uid, NULL, NULL);
}

mc_status mc_smart_set_rule(mc_library *lib, const char *smart_uid, const char *rule_json) {
  if (!lib || !smart_uid || !rule_json) return MC_ERR_INVALID_ARG;
  mc_status st = mc_library_begin_write(lib);
  if (st != MC_OK) return st;
  mc_smart_subject subject;
  char *old;
  st = load(lib, smart_uid, &subject, &old);
  free(old);
  if (st == MC_OK) st = mc_smart_validate(subject, rule_json);
  if (st == MC_OK) st = write_one(lib, "UPDATE smart_lists SET rule = ?2 WHERE uid = ?1", smart_uid, rule_json, NULL);
  if (st == MC_OK) st = mc_library_commit(lib);
  if (st != MC_OK) mc_library_rollback(lib);
  return st;
}

/* rule with a new seed in its last order key, which must be random. Returns the new JSON
 * (malloc'd) or NULL. */
static char *reseed(const char *rule, int64_t seed) {
  yyjson_doc *doc = yyjson_read(rule, strlen(rule), 0);
  yyjson_mut_doc *mut = doc ? yyjson_doc_mut_copy(doc, NULL) : NULL;
  yyjson_doc_free(doc);
  yyjson_mut_val *order = yyjson_mut_obj_get(yyjson_mut_doc_get_root(mut), "order");
  yyjson_mut_val *last = yyjson_mut_arr_get_last(order);
  char *json = NULL;
  if (last && yyjson_mut_equals_str(yyjson_mut_obj_get(last, "field"), "random") &&
      yyjson_mut_obj_put(last, yyjson_mut_strcpy(mut, "seed"), yyjson_mut_sint(mut, seed)))
    json = yyjson_mut_write(mut, 0, NULL);
  yyjson_mut_doc_free(mut);
  return json;
}

mc_status mc_smart_reshuffle(mc_library *lib, const char *smart_uid) {
  if (!lib || !smart_uid) return MC_ERR_INVALID_ARG;
  uint64_t r;
  mc_status st = mc_random_bytes(&r, sizeof r);
  if (st != MC_OK || (st = mc_library_begin_write(lib)) != MC_OK) return st;
  mc_smart_subject subject;
  char *rule, *json = NULL;
  st = load(lib, smart_uid, &subject, &rule);
  /* Kept within 2^53, so JSON readers that use doubles see it exactly. */
  if (st == MC_OK && !(json = reseed(rule, (int64_t)(r & ((1ULL << 53) - 1))))) st = MC_ERR_INVALID_ARG;
  if (st == MC_OK) st = mc_smart_validate(subject, json);
  if (st == MC_OK) st = write_one(lib, "UPDATE smart_lists SET rule = ?2 WHERE uid = ?1", smart_uid, json, NULL);
  free(rule);
  free(json);
  if (st == MC_OK) st = mc_library_commit(lib);
  if (st != MC_OK) mc_library_rollback(lib);
  return st;
}

/* Whether rule's last order key is random, as mc_smart_reshuffle() needs. */
static int is_random(const char *rule) {
  yyjson_doc *doc = rule ? yyjson_read(rule, strlen(rule), 0) : NULL;
  yyjson_val *order = yyjson_obj_get(yyjson_doc_get_root(doc), "order");
  size_t n = yyjson_arr_size(order);
  int random = n && yyjson_equals_str(yyjson_obj_get(yyjson_arr_get(order, n - 1), "field"), "random");
  yyjson_doc_free(doc);
  return random;
}

mc_status mc_library_list_smart(mc_library *lib, unsigned flags, mc_smart_list **out) {
  return mc_library_list_smart_keeping(lib, flags, NULL, out);
}

mc_status mc_library_list_smart_keeping(mc_library *lib, unsigned flags, const char *keep_uid, mc_smart_list **out) {
  if (!lib || !out) return MC_ERR_INVALID_ARG;
  *out = NULL;
  mc_smart_list *list = calloc(1, sizeof *list);
  if (!list) return MC_ERR_NOMEM;
  sqlite3_stmt *stmt = NULL;
  mc_status st = MC_OK;
  if (sqlite3_prepare_v2(lib->db, "SELECT uid, mc_smart_name(uid, name) AS shown, subject, rule FROM smart_lists"
                         " ORDER BY shown COLLATE NOCASE, uid",
                         -1, &stmt, NULL) != SQLITE_OK)
    st = MC_ERR_DB;
  int cap = 0, rc = SQLITE_DONE;
  while (st == MC_OK && (rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    mc_smart_subject subject;
    /* Skip kinds of list from a newer app this build can't show. */
    if (!parse_subject(sqlite3_column_text(stmt, 2), &subject)) continue;
    if ((flags & MC_LIST_PLAYABLE) && !(keep_uid && !strcmp(keep_uid, (const char *)sqlite3_column_text(stmt, 0)))) {
      int listed = 0;
      st = rule_listed_here(lib, subject, (const char *)sqlite3_column_text(stmt, 3), &listed);
      if (st == MC_ERR_FORMAT || st == MC_ERR_INVALID_ARG) st = MC_OK; /* can't be shown */
      if (st != MC_OK) break;
      if (!listed) {
        list->hidden++;
        continue;
      }
    }
    if (list->count == cap) {
      int grown = cap ? cap * 2 : 8;
      mc_smart_info *items = realloc(list->items, (size_t)grown * sizeof *items);
      if (!items) {
        st = MC_ERR_NOMEM;
        break;
      }
      list->items = items;
      cap = grown;
    }
    mc_smart_info *s = &list->items[list->count++];
    memset(s, 0, sizeof *s);
    const unsigned char *uid = sqlite3_column_text(stmt, 0);
    if (uid && strlen((const char *)uid) < MC_UID_STRLEN) strcpy(s->uid, (const char *)uid);
    s->subject = subject;
    s->name = mc_strdup((const char *)sqlite3_column_text(stmt, 1));
    s->rule = mc_strdup((const char *)sqlite3_column_text(stmt, 3));
    if (!s->name || !s->rule) st = MC_ERR_NOMEM;
    else s->random = is_random(s->rule);
  }
  if (st == MC_OK && rc != SQLITE_DONE) st = MC_ERR_DB;
  sqlite3_finalize(stmt);
  if (st != MC_OK) {
    mc_smart_list_free(list);
    return st;
  }
  *out = list;
  return MC_OK;
}

void mc_smart_list_free(mc_smart_list *list) {
  if (!list) return;
  for (int i = 0; i < list->count; i++) {
    free(list->items[i].name);
    free(list->items[i].rule);
  }
  free(list->items);
  free(list);
}

/* Smart lists: a rule (JSON) compiled to one SQL query each time the list is listed. Field
 * names map to fixed SQL expressions in k_fields and every value from the rule is a bound
 * parameter, so nothing from a rule is ever spliced into SQL. Every condition is coalesced to
 * 0 or 1, so "not" behaves as people expect where a value is missing: "not played within 180
 * days" includes tracks never played at all. The format is in docs/schema.md. */
#include "musiccore/smartlist.h"

#include <stdlib.h>
#include <string.h>
#include <yyjson.h>

#include "library/library_internal.h"
#include "util/platform.h"

#define RULE_VERSION 1
#define MAX_DEPTH 32
#define DAY_MS 86400000LL

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
 * order by, if not expr. A field with wrap is true of an album if it's true of any of its
 * tracks: the condition on tt goes inside wrap and a closing parenthesis. */
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

static const field_def k_fields[] = {
  {"title", F_TEXT, {{"t.title", NULL, NULL}, {"a.title", NULL, NULL}}},
  {"artist",
   F_TEXT,
   {{"coalesce(ar.name, aar.name)",
     "CASE WHEN ar.uid IS NOT NULL THEN coalesce(ar.sort_name, ar.name)"
     " ELSE coalesce(aar.sort_name, aar.name) END",
     NULL},
    {"ar.name", "coalesce(ar.sort_name, ar.name)", NULL}}},
  {"album_artist", F_TEXT, {{"aar.name", "coalesce(aar.sort_name, aar.name)", NULL}, {NULL, NULL, NULL}}},
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
  {"singles", F_BOOL, {{"al.kind = 'singles'", NULL, NULL}, {"a.kind = 'singles'", NULL, NULL}}},
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
  "coalesce(aar.sort_name, aar.name) COLLATE NOCASE, al.kind = 'singles', al.year,"
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

static void put_match(query *q, yyjson_val *node, int depth) {
  if (q->st != MC_OK) return;
  if (depth > MAX_DEPTH || !yyjson_is_obj(node)) {
    bad(q);
    return;
  }
  yyjson_val *all = yyjson_obj_get(node, "all"), *any = yyjson_obj_get(node, "any");
  yyjson_val *not = yyjson_obj_get(node, "not");
  if (!all && !any && !not) {
    put_leaf(q, node);
    return;
  }
  if (yyjson_obj_size(node) != 1) {
    bad(q);
    return;
  }
  if (not) {
    put(q, "NOT (");
    put_match(q, not, depth + 1);
    put(q, ")");
    return;
  }
  yyjson_val *list = all ? all : any;
  if (!yyjson_is_arr(list)) {
    bad(q);
    return;
  }
  if (yyjson_arr_size(list) == 0) {
    put(q, all ? "1" : "0");
    return;
  }
  put(q, "(");
  size_t idx, max;
  yyjson_val *child;
  yyjson_arr_foreach(list, idx, max, child) {
    if (idx) put(q, all ? " AND " : " OR ");
    put_match(q, child, depth + 1);
  }
  put(q, ")");
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
    if (!yyjson_is_obj(key) || !f || !sortable(f->type) || s->wrap || !only_keys(key, sort_keys) ||
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

/* Compiles rule_json into q: the whole query, ready to prepare and bind. */
static mc_status compile(query *q, mc_smart_subject subject, const char *rule_json, int64_t now_ms) {
  memset(q, 0, sizeof *q);
  q->subject = subject;
  q->now_ms = now_ms;
  if ((unsigned)subject > MC_SMART_ALBUMS || !rule_json) return q->st = MC_ERR_INVALID_ARG;
  q->doc = yyjson_read(rule_json, strlen(rule_json), 0);
  yyjson_val *root = yyjson_doc_get_root(q->doc);
  static const char *const top_keys[] = {"v", "match", "order", "limit", NULL};
  int64_t v = 0, limit = 0;
  yyjson_val *limit_val = yyjson_obj_get(root, "limit");
  if (!yyjson_is_obj(root) || !only_keys(root, top_keys) || !get_int(yyjson_obj_get(root, "v"), &v) ||
      v != RULE_VERSION || (limit_val && (!get_int(limit_val, &limit) || limit < 1)))
    return q->st = MC_ERR_FORMAT;

  put(q, subject == MC_SMART_TRACKS ? TRACK_COLS " FROM tracks t" TRACK_JOINS
                                    : ALBUM_COLS " FROM albums a" ALBUM_JOINS);
  put(q, " WHERE ");
  yyjson_val *match = yyjson_obj_get(root, "match");
  if (match)
    put_match(q, match, 0);
  else
    put(q, "1");
  put(q, " ORDER BY ");
  put_order(q, yyjson_obj_get(root, "order"));
  if (limit_val) {
    put(q, " LIMIT ");
    put_int(q, limit);
  }
  return q->st;
}

/* Compiles and prepares rule_json; *stmt is NULL on failure. */
static mc_status prepare(mc_library *lib, mc_smart_subject subject, const char *rule_json, sqlite3_stmt **stmt) {
  *stmt = NULL;
  query q;
  mc_status st = compile(&q, subject, rule_json, (int64_t)lib->hlc.wall_ms());
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
  mc_status st = compile(&q, subject, rule_json, 0);
  query_free(&q);
  return st;
}

mc_status mc_smart_preview_tracks(mc_library *lib, const char *rule_json, mc_track_list **out) {
  if (!lib || !rule_json || !out) return MC_ERR_INVALID_ARG;
  *out = NULL;
  sqlite3_stmt *stmt;
  mc_status st = prepare(lib, MC_SMART_TRACKS, rule_json, &stmt);
  return st == MC_OK ? mc_library_read_tracks(lib, stmt, out) : st;
}

mc_status mc_smart_preview_albums(mc_library *lib, const char *rule_json, mc_album_list **out) {
  if (!lib || !rule_json || !out) return MC_ERR_INVALID_ARG;
  *out = NULL;
  sqlite3_stmt *stmt;
  mc_status st = prepare(lib, MC_SMART_ALBUMS, rule_json, &stmt);
  return st == MC_OK ? mc_library_read_albums(lib, stmt, out) : st;
}

/* --- Saved lists --- */

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

/* Whether a rule holds a track with a file here (TRACK_COLS's path, column 5) or an album with
 * one (ALBUM_COLS's file, column 6), stopping at the first. */
static mc_status rule_has_playable(mc_library *lib, mc_smart_subject subject, const char *rule, int *out) {
  *out = 0;
  sqlite3_stmt *stmt;
  mc_status st = prepare(lib, subject, rule, &stmt);
  if (st != MC_OK) return st;
  int col = subject == MC_SMART_TRACKS ? 5 : 6, rc;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
    if (sqlite3_column_type(stmt, col) != SQLITE_NULL) {
      *out = 1;
      break;
    }
  if (rc != SQLITE_ROW && rc != SQLITE_DONE) st = MC_ERR_DB;
  sqlite3_finalize(stmt);
  return st;
}

mc_status mc_smart_has_playable(mc_library *lib, const char *smart_uid, int *out) {
  *out = 0;
  mc_smart_subject subject;
  char *rule;
  mc_status st = load(lib, smart_uid, &subject, &rule);
  if (st == MC_OK) st = rule_has_playable(lib, subject, rule, out);
  free(rule);
  return st == MC_ERR_FORMAT || st == MC_ERR_INVALID_ARG ? MC_OK : st;
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
  if (!lib || !out) return MC_ERR_INVALID_ARG;
  *out = NULL;
  mc_smart_list *list = calloc(1, sizeof *list);
  if (!list) return MC_ERR_NOMEM;
  sqlite3_stmt *stmt = NULL;
  mc_status st = MC_OK;
  if (sqlite3_prepare_v2(lib->db, "SELECT uid, name, subject, rule FROM smart_lists ORDER BY name COLLATE NOCASE, uid",
                         -1, &stmt, NULL) != SQLITE_OK)
    st = MC_ERR_DB;
  int cap = 0, rc = SQLITE_DONE;
  while (st == MC_OK && (rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    mc_smart_subject subject;
    /* Skip kinds of list from a newer app this build can't show. */
    if (!parse_subject(sqlite3_column_text(stmt, 2), &subject)) continue;
    if (flags & MC_LIST_PLAYABLE) {
      int playable = 0;
      st = rule_has_playable(lib, subject, (const char *)sqlite3_column_text(stmt, 3), &playable);
      if (st == MC_ERR_FORMAT || st == MC_ERR_INVALID_ARG) st = MC_OK; /* holds nothing */
      if (st != MC_OK) break;
      if (!playable) {
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

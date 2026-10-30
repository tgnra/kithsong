#include "sync/rows.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mbedtls/sha256.h>

#include "library/library_internal.h"
#include "sync/merge.h"
#include "sync/sync_db.h"

#define UID_LEN (MC_UID_STRLEN - 1)
/* hlc, origin, entity length, row_uid, field length, value tag */
#define ROW_FIXED (8 + UID_LEN + 1 + UID_LEN + 1 + 1)
#define MAX_COLS 32
#define MAX_FKS 8

/* --- Encoding --- */

static void put_u16(unsigned char *p, unsigned v) {
  p[0] = (unsigned char)(v >> 8);
  p[1] = (unsigned char)v;
}

static void put_u32(unsigned char *p, uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (24 - 8 * i));
}

static void put_u64(unsigned char *p, uint64_t v) {
  for (int i = 0; i < 8; i++) p[i] = (unsigned char)(v >> (56 - 8 * i));
}

static uint32_t get_u32(const unsigned char *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint64_t get_u64(const unsigned char *p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v = v << 8 | p[i];
  return v;
}

void mc_rows_writer_init(mc_rows_writer *w, unsigned char *buf, size_t cap, int db) {
  w->buf = buf;
  w->cap = cap;
  w->buf[0] = (unsigned char)db;
  put_u16(w->buf + 1, 0);
  w->len = 3;
  w->count = 0;
}

static size_t value_size(const mc_row *r) {
  switch (r->tag) {
  case MC_VAL_INT:
  case MC_VAL_REAL: return 8;
  case MC_VAL_TEXT:
  case MC_VAL_BLOB: return 4 + (size_t)r->len;
  default: return 0;
  }
}

int mc_rows_writer_add(mc_rows_writer *w, const mc_row *r) {
  size_t elen = strlen(r->entity), flen = strlen(r->field);
  if (strlen(r->origin) != UID_LEN || strlen(r->row_uid) != UID_LEN || !elen ||
      elen > MC_ROWS_NAME_MAX || !flen || flen > MC_ROWS_NAME_MAX || r->tag < MC_VAL_NULL ||
      r->tag > MC_VAL_BLOB)
    return -1;
  size_t need = ROW_FIXED + elen + flen + value_size(r);
  if (w->count == 0xFFFF || w->len + need > w->cap) return 0;

  unsigned char *p = w->buf + w->len;
  put_u64(p, r->hlc);
  p += 8;
  memcpy(p, r->origin, UID_LEN);
  p += UID_LEN;
  *p++ = (unsigned char)elen;
  memcpy(p, r->entity, elen);
  p += elen;
  memcpy(p, r->row_uid, UID_LEN);
  p += UID_LEN;
  *p++ = (unsigned char)flen;
  memcpy(p, r->field, flen);
  p += flen;
  *p++ = (unsigned char)r->tag;
  if (r->tag == MC_VAL_INT) {
    put_u64(p, (uint64_t)r->i);
  } else if (r->tag == MC_VAL_REAL) {
    uint64_t bits;
    memcpy(&bits, &r->r, sizeof bits);
    put_u64(p, bits);
  } else if (r->tag == MC_VAL_TEXT || r->tag == MC_VAL_BLOB) {
    put_u32(p, r->len);
    if (r->len) memcpy(p + 4, r->data, r->len);
  }
  w->len += need;
  w->count++;
  return 1;
}

size_t mc_rows_writer_finish(mc_rows_writer *w) {
  put_u16(w->buf + 1, w->count);
  return w->len;
}

/* Reads a length-prefixed name into out; 0 if it doesn't fit the payload. */
static int read_name(const unsigned char **p, const unsigned char *end, char *out) {
  if (*p >= end) return 0;
  size_t n = **p;
  (*p)++;
  if (!n || (size_t)(end - *p) < n || memchr(*p, '\0', n)) return 0;
  memcpy(out, *p, n);
  out[n] = '\0';
  *p += n;
  return 1;
}

static int read_uid(const unsigned char **p, const unsigned char *end, char *out) {
  if ((size_t)(end - *p) < UID_LEN || memchr(*p, '\0', UID_LEN)) return 0;
  memcpy(out, *p, UID_LEN);
  out[UID_LEN] = '\0';
  *p += UID_LEN;
  return 1;
}

mc_status mc_rows_decode(const unsigned char *payload, size_t len, int *db, mc_row **rows,
                         int *count) {
  if (!payload || !db || !rows || !count) return MC_ERR_INVALID_ARG;
  *rows = NULL;
  *count = 0;
  if (len < 3) return MC_ERR_FORMAT;
  *db = payload[0];
  int n = payload[1] << 8 | payload[2];
  if (*db != MC_ROWS_GROUP && *db != MC_ROWS_LIBRARY) return MC_ERR_FORMAT;
  mc_row *out = n ? calloc((size_t)n, sizeof *out) : NULL;
  if (n && !out) return MC_ERR_NOMEM;

  const unsigned char *p = payload + 3, *end = payload + len;
  for (int i = 0; i < n; i++) {
    mc_row *r = &out[i];
    if ((size_t)(end - p) < 8) goto bad;
    r->hlc = get_u64(p);
    p += 8;
    if (!read_uid(&p, end, r->origin) || !read_name(&p, end, r->entity) ||
        !read_uid(&p, end, r->row_uid) || !read_name(&p, end, r->field) || p >= end)
      goto bad;
    r->tag = *p++;
    switch (r->tag) {
    case MC_VAL_NULL: break;
    case MC_VAL_INT:
    case MC_VAL_REAL: {
      if ((size_t)(end - p) < 8) goto bad;
      uint64_t bits = get_u64(p);
      p += 8;
      if (r->tag == MC_VAL_INT)
        r->i = (int64_t)bits;
      else
        memcpy(&r->r, &bits, sizeof bits);
      break;
    }
    case MC_VAL_TEXT:
    case MC_VAL_BLOB:
      if ((size_t)(end - p) < 4) goto bad;
      r->len = get_u32(p);
      p += 4;
      if ((size_t)(end - p) < r->len) goto bad;
      r->data = p;
      p += r->len;
      break;
    default: goto bad;
    }
  }
  if (p != end) goto bad;
  *rows = out;
  *count = n;
  return MC_OK;

bad:
  free(out);
  return MC_ERR_FORMAT;
}

/* --- Targets --- */

static mc_status lib_begin(void *o) {
  return mc_library_begin_write(o);
}
static mc_status lib_commit(void *o) {
  return mc_library_commit(o);
}
static void lib_rollback(void *o) {
  mc_library_rollback(o);
}

void mc_rows_target_library(mc_library *lib, mc_rows_target *out) {
  memset(out, 0, sizeof *out);
  out->db = lib->db;
  out->hlc = &lib->hlc;
  out->applying_remote = &lib->applying_remote;
  out->tables = mc_library_tracked(&out->ntables);
  out->owner = lib;
  out->merge = 1;
  out->begin = lib_begin;
  out->commit = lib_commit;
  out->rollback = lib_rollback;
}

static mc_status group_begin(void *o) {
  return mc_sync_db_begin_write(o);
}
static mc_status group_commit(void *o) {
  return mc_sync_db_commit(o);
}
static void group_rollback(void *o) {
  mc_sync_db_rollback(o);
}

void mc_rows_target_group(mc_sync_db *sdb, mc_rows_target *out) {
  memset(out, 0, sizeof *out);
  out->db = sdb->db;
  out->hlc = &sdb->hlc;
  out->applying_remote = &sdb->applying_remote;
  out->tables = mc_sync_db_tracked(&out->ntables);
  out->owner = sdb;
  out->begin = group_begin;
  out->commit = group_commit;
  out->rollback = group_rollback;
}

/* --- Applying ---
 *
 * Each batch runs in one transaction:
 *  1. Every row not already in the changelog is inserted there unchanged, noting whether it
 *     beats the current winner of its (entity, row_uid, field).
 *  2. Each row the batch touched is then brought in line with the changelog, table by
 *     table, parents first:
 *     - A row is live if some field change is newer, by (hlc, origin_uid), than its newest
 *       `_deleted`. A dead row that exists is deleted; a delete only wins over a
 *       concurrent edit if it is newer.
 *     - A live row that exists gets the fields the batch won, set to their winners.
 *     - A live row that doesn't exist yet (a new row, or one an edit brings back after an
 *       older delete) is inserted with every field's winner, once the changelog holds a
 *       value for each NOT NULL column without a default. Until then it waits: its fields
 *       are logged, and the batch that brings the last one creates it. So a row's fields
 *       may arrive in any order and across batches.
 *
 * Foreign keys. library.db runs with foreign_keys ON, and ON DELETE actions (cascading a
 * deleted track's files and playlist entries, nulling a deleted album's references) must
 * still happen when a delete is applied, so they stay on. But rows reach us field by field:
 * a track's album_uid may be applied before the album row is created later in the batch.
 * So the batch sets defer_foreign_keys, which moves every check to COMMIT, and applies
 * tables parents first. That order comes from the foreign keys themselves (see
 * order_tables()), so a table added to a tracked list later needs nothing here.
 *
 * Senders stream in hlc order from one snapshot, and a child's reference is always logged
 * after its parent's creation (the device that made the reference had seen the parent, and
 * its clock had observed it), so a parent is never missing at commit, except when it was
 * deleted concurrently with the edit that references it. Before COMMIT each touched row's
 * references are checked, and one to a missing parent is resolved as the schema's ON
 * DELETE action would have: set to NULL, or the row deleted where the column is NOT NULL.
 * Every device resolves it the same way, and none logs it, since each reaches the same
 * state from the same changelog.
 *
 * Duplicates (library.db; see merge.h). After logging, the changelog is scanned for keys
 * that make two uids one. A merged class is one row under its canonical uid: rows naming
 * any uid of the class are applied to the canonical row, whose fields and liveness are
 * worked out from the changelog rows of every uid in the class together, as if they were
 * one row's. Then references to the uids this batch merged away move to the canonical row
 * and the losers' rows go, and any touched row that names an older loser is pointed at its
 * canonical uid. Like the reference check, this is derived from the changelog and never
 * logged. */

typedef struct fk_info {
  int col; /* index into the table's cols */
  char parent[64];
  char parent_col[64];
  int parent_table; /* index into the tracked tables */
  int notnull;
} fk_info;

typedef struct table_meta {
  const mc_changelog_table *t;
  int ncols;
  uint32_t required; /* bit i: cols[i] is NOT NULL without a default */
  fk_info fks[MAX_FKS];
  int nfks;
} table_meta;

typedef struct touched {
  int table;
  char row_uid[MC_UID_STRLEN];
  uint32_t won; /* bit i: the batch brought cols[i] a new winner */
} touched;

struct mc_rows_applier {
  mc_rows_target tg;
  table_meta *tables;
  int *order; /* table indexes, parents before children */
  sqlite3_stmt *dup, *winner, *log, *state;
  sqlite3_stmt *class_state; /* state over a merged class; library.db only */
  touched *touched;
  int ntouched, cap;
};

static int col_index(const table_meta *m, const char *name) {
  for (int i = 0; i < m->ncols; i++)
    if (!strcmp(m->t->cols[i], name)) return i;
  return -1;
}

static mc_status read_meta(sqlite3 *db, const mc_changelog_table *tables, size_t ntables,
                           table_meta *m) {
  for (m->ncols = 0; m->t->cols[m->ncols]; m->ncols++) {
  }
  if (m->ncols > MAX_COLS) return MC_ERR_INVALID_ARG;
  char sql[160];
  sqlite3_stmt *stmt;
  snprintf(sql, sizeof sql, "PRAGMA table_info(%s)", m->t->table);
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return MC_ERR_DB;
  uint32_t notnull = 0;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const char *name = (const char *)sqlite3_column_text(stmt, 1);
    int i = name ? col_index(m, name) : -1;
    if (i < 0) continue;
    if (sqlite3_column_int(stmt, 3)) notnull |= 1u << i;
    if (sqlite3_column_int(stmt, 3) && sqlite3_column_type(stmt, 4) == SQLITE_NULL)
      m->required |= 1u << i;
  }
  sqlite3_finalize(stmt);

  snprintf(sql, sizeof sql, "PRAGMA foreign_key_list(%s)", m->t->table);
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return MC_ERR_DB;
  while (sqlite3_step(stmt) == SQLITE_ROW && m->nfks < MAX_FKS) {
    const char *parent = (const char *)sqlite3_column_text(stmt, 2);
    const char *from = (const char *)sqlite3_column_text(stmt, 3);
    const char *to = (const char *)sqlite3_column_text(stmt, 4);
    int i = from ? col_index(m, from) : -1;
    if (i < 0 || !parent) continue;
    /* Only parents that sync too; and a NULL "to" means the parent's key. */
    const mc_changelog_table *pt = NULL;
    for (size_t k = 0; k < ntables; k++)
      if (!strcmp(tables[k].table, parent)) pt = &tables[k];
    if (!pt) continue;
    fk_info *f = &m->fks[m->nfks++];
    f->col = i;
    f->parent_table = (int)(pt - tables);
    snprintf(f->parent, sizeof f->parent, "%s", parent);
    snprintf(f->parent_col, sizeof f->parent_col, "%s", to ? to : pt->key);
    f->notnull = (notnull >> i) & 1;
  }
  sqlite3_finalize(stmt);
  return MC_OK;
}

/* Orders the tables so each comes after the tables its foreign keys point to. A cycle
 * (none exists today) falls back to list order for what's left; deferred foreign keys
 * keep that correct, only less tidy. */
static mc_status order_tables(mc_rows_applier *a) {
  size_t n = a->tg.ntables;
  a->order = calloc(n ? n : 1, sizeof *a->order);
  char *placed = calloc(n ? n : 1, 1);
  if (!a->order || !placed) {
    free(placed);
    return MC_ERR_NOMEM;
  }
  size_t done = 0;
  while (done < n) {
    size_t before = done;
    for (size_t t = 0; t < n; t++) {
      if (placed[t]) continue;
      int ready = 1;
      for (int k = 0; k < a->tables[t].nfks; k++) {
        int p = a->tables[t].fks[k].parent_table;
        if (p != (int)t && !placed[p]) ready = 0;
      }
      if (!ready) continue;
      placed[t] = 1;
      a->order[done++] = (int)t;
    }
    if (done == before) /* a cycle */
      for (size_t t = 0; t < n; t++)
        if (!placed[t]) {
          placed[t] = 1;
          a->order[done++] = (int)t;
        }
  }
  free(placed);
  return MC_OK;
}

mc_status mc_rows_applier_new(const mc_rows_target *target, mc_rows_applier **out) {
  if (!target || !out) return MC_ERR_INVALID_ARG;
  *out = NULL;
  mc_rows_applier *a = calloc(1, sizeof *a);
  if (!a) return MC_ERR_NOMEM;
  a->tg = *target;
  a->tables = calloc(target->ntables, sizeof *a->tables);
  mc_status st = a->tables ? MC_OK : MC_ERR_NOMEM;
  for (size_t i = 0; st == MC_OK && i < target->ntables; i++) {
    a->tables[i].t = &target->tables[i];
    st = read_meta(target->db, target->tables, target->ntables, &a->tables[i]);
  }
  if (st == MC_OK) st = order_tables(a);
  sqlite3 *db = target->db;
  if (st == MC_OK &&
      (sqlite3_prepare_v2(db,
                          "SELECT 1 FROM changelog WHERE entity = ?1 AND row_uid = ?2"
                          " AND field = ?3 AND hlc = ?4 AND origin_uid = ?5",
                          -1, &a->dup, NULL) != SQLITE_OK ||
       sqlite3_prepare_v2(db,
                          "SELECT hlc, origin_uid FROM changelog WHERE entity = ?1"
                          " AND row_uid = ?2 AND field = ?3"
                          " ORDER BY hlc DESC, origin_uid DESC LIMIT 1",
                          -1, &a->winner, NULL) != SQLITE_OK ||
       sqlite3_prepare_v2(db,
                          "INSERT INTO changelog (hlc, origin_uid, entity, row_uid, field, value)"
                          " VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
                          -1, &a->log, NULL) != SQLITE_OK ||
       sqlite3_prepare_v2(db,
                          "SELECT field, value, hlc, origin_uid FROM changelog"
                          " WHERE entity = ?1 AND row_uid = ?2"
                          " ORDER BY field, hlc DESC, origin_uid DESC",
                          -1, &a->state, NULL) != SQLITE_OK))
    st = MC_ERR_DB;
  /* row_uid last only to make ties between a class's uids deterministic. */
  if (st == MC_OK && target->merge &&
      sqlite3_prepare_v2(db,
                         "SELECT field, value, hlc, origin_uid FROM changelog"
                         " WHERE entity = ?1 AND row_uid IN"
                         " (SELECT ?2 UNION ALL SELECT uid FROM aliases WHERE canonical = ?2)"
                         " ORDER BY field, hlc DESC, origin_uid DESC, row_uid",
                         -1, &a->class_state, NULL) != SQLITE_OK)
    st = MC_ERR_DB;
  if (st != MC_OK) {
    mc_rows_applier_free(a);
    return st;
  }
  *out = a;
  return MC_OK;
}

void mc_rows_applier_free(mc_rows_applier *a) {
  if (!a) return;
  sqlite3_finalize(a->dup);
  sqlite3_finalize(a->winner);
  sqlite3_finalize(a->log);
  sqlite3_finalize(a->state);
  sqlite3_finalize(a->class_state);
  free(a->tables);
  free(a->order);
  free(a->touched);
  free(a);
}

/* (hlc, origin) order: <0, 0 or >0. */
static int cmp_stamp(uint64_t h1, const char *o1, uint64_t h2, const char *o2) {
  if (h1 != h2) return h1 < h2 ? -1 : 1;
  return strcmp(o1, o2);
}

static void bind_value(sqlite3_stmt *stmt, int idx, const mc_row *r) {
  switch (r->tag) {
  case MC_VAL_INT: sqlite3_bind_int64(stmt, idx, r->i); break;
  case MC_VAL_REAL: sqlite3_bind_double(stmt, idx, r->r); break;
  case MC_VAL_TEXT:
    sqlite3_bind_text(stmt, idx, (const char *)r->data, (int)r->len, SQLITE_STATIC);
    break;
  case MC_VAL_BLOB:
    sqlite3_bind_blob(stmt, idx, r->len ? (const void *)r->data : "", (int)r->len,
                      SQLITE_STATIC);
    break;
  default: sqlite3_bind_null(stmt, idx); break;
  }
}

static int find_table(const mc_rows_applier *a, const char *entity) {
  for (size_t i = 0; i < a->tg.ntables; i++)
    if (!strcmp(a->tables[i].t->table, entity)) return (int)i;
  return -1;
}

static touched *touch(mc_rows_applier *a, int table, const char *row_uid) {
  for (int i = 0; i < a->ntouched; i++)
    if (a->touched[i].table == table && !strcmp(a->touched[i].row_uid, row_uid))
      return &a->touched[i];
  if (a->ntouched == a->cap) {
    int grown = a->cap ? a->cap * 2 : 64;
    touched *more = realloc(a->touched, (size_t)grown * sizeof *more);
    if (!more) return NULL;
    a->touched = more;
    a->cap = grown;
  }
  touched *t = &a->touched[a->ntouched++];
  t->table = table;
  memcpy(t->row_uid, row_uid, MC_UID_STRLEN);
  t->won = 0;
  return t;
}

/* Logs one row. *is_new = 0 if the changelog already had it; *won = 1 if it beats the
 * field's previous winner. */
static mc_status log_row(mc_rows_applier *a, const mc_row *r, int *is_new, int *won) {
  *is_new = *won = 0;
  sqlite3_stmt *s = a->dup;
  sqlite3_reset(s);
  sqlite3_bind_text(s, 1, r->entity, -1, SQLITE_STATIC);
  sqlite3_bind_text(s, 2, r->row_uid, -1, SQLITE_STATIC);
  sqlite3_bind_text(s, 3, r->field, -1, SQLITE_STATIC);
  sqlite3_bind_int64(s, 4, (sqlite3_int64)r->hlc);
  sqlite3_bind_text(s, 5, r->origin, -1, SQLITE_STATIC);
  int rc = sqlite3_step(s);
  sqlite3_reset(s);
  if (rc == SQLITE_ROW) return MC_OK;
  if (rc != SQLITE_DONE) return MC_ERR_DB;

  s = a->winner;
  sqlite3_reset(s);
  sqlite3_bind_text(s, 1, r->entity, -1, SQLITE_STATIC);
  sqlite3_bind_text(s, 2, r->row_uid, -1, SQLITE_STATIC);
  sqlite3_bind_text(s, 3, r->field, -1, SQLITE_STATIC);
  rc = sqlite3_step(s);
  if (rc == SQLITE_ROW) {
    const char *origin = (const char *)sqlite3_column_text(s, 1);
    *won = cmp_stamp(r->hlc, r->origin, (uint64_t)sqlite3_column_int64(s, 0),
                     origin ? origin : "") > 0;
  } else if (rc == SQLITE_DONE) {
    *won = 1;
  }
  sqlite3_reset(s);
  if (rc != SQLITE_ROW && rc != SQLITE_DONE) return MC_ERR_DB;

  s = a->log;
  sqlite3_reset(s);
  sqlite3_bind_int64(s, 1, (sqlite3_int64)r->hlc);
  sqlite3_bind_text(s, 2, r->origin, -1, SQLITE_STATIC);
  sqlite3_bind_text(s, 3, r->entity, -1, SQLITE_STATIC);
  sqlite3_bind_text(s, 4, r->row_uid, -1, SQLITE_STATIC);
  sqlite3_bind_text(s, 5, r->field, -1, SQLITE_STATIC);
  bind_value(s, 6, r);
  rc = sqlite3_step(s);
  sqlite3_reset(s);
  sqlite3_clear_bindings(s);
  if (rc != SQLITE_DONE) return MC_ERR_DB;
  *is_new = 1;
  return MC_OK;
}

/* What the changelog says a row should be. */
typedef struct row_state {
  sqlite3_value *val[MAX_COLS]; /* winners; NULL pointer: no change logged */
  uint32_t has;                 /* bit i: val[i] is set */
  int live;
} row_state;

static void state_free(row_state *rs) {
  for (int i = 0; i < MAX_COLS; i++) sqlite3_value_free(rs->val[i]);
}

static mc_status read_state(mc_rows_applier *a, const table_meta *m, const char *row_uid,
                            row_state *rs) {
  memset(rs, 0, sizeof *rs);
  uint64_t del_hlc = 0, last_hlc = 0;
  char del_origin[MC_UID_STRLEN] = "", last_origin[MC_UID_STRLEN] = "";
  int has_del = 0, has_change = 0;
  char prev[MC_ROWS_NAME_MAX + 1] = "";

  sqlite3_stmt *s = a->class_state && mc_merge_table(m->t->table) ? a->class_state : a->state;
  sqlite3_reset(s);
  sqlite3_bind_text(s, 1, m->t->table, -1, SQLITE_STATIC);
  sqlite3_bind_text(s, 2, row_uid, -1, SQLITE_STATIC);
  int rc;
  while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
    const char *field = (const char *)sqlite3_column_text(s, 0);
    const char *origin = (const char *)sqlite3_column_text(s, 3);
    if (!field || !origin || !strcmp(field, prev)) continue; /* only each field's winner */
    snprintf(prev, sizeof prev, "%s", field);
    uint64_t hlc = (uint64_t)sqlite3_column_int64(s, 2);
    if (!strcmp(field, "_deleted")) {
      has_del = 1;
      del_hlc = hlc;
      snprintf(del_origin, sizeof del_origin, "%s", origin);
      continue;
    }
    int i = col_index(m, field);
    if (i < 0) continue; /* a column this version doesn't know */
    rs->val[i] = sqlite3_value_dup(sqlite3_column_value(s, 1));
    if (!rs->val[i]) {
      sqlite3_reset(s);
      state_free(rs);
      return MC_ERR_NOMEM;
    }
    rs->has |= 1u << i;
    if (!has_change || cmp_stamp(hlc, origin, last_hlc, last_origin) > 0) {
      has_change = 1;
      last_hlc = hlc;
      snprintf(last_origin, sizeof last_origin, "%s", origin);
    }
  }
  sqlite3_reset(s);
  if (rc != SQLITE_DONE) {
    state_free(rs);
    return MC_ERR_DB;
  }
  rs->live = has_change && (!has_del || cmp_stamp(last_hlc, last_origin, del_hlc, del_origin) > 0);
  return MC_OK;
}

static int row_exists(sqlite3 *db, const table_meta *m, const char *row_uid, mc_status *st) {
  char sql[160];
  snprintf(sql, sizeof sql, "SELECT 1 FROM %s WHERE %s = ?1", m->t->table, m->t->key);
  sqlite3_stmt *s;
  if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) {
    *st = MC_ERR_DB;
    return 0;
  }
  sqlite3_bind_text(s, 1, row_uid, -1, SQLITE_STATIC);
  int rc = sqlite3_step(s);
  sqlite3_finalize(s);
  if (rc != SQLITE_ROW && rc != SQLITE_DONE) *st = MC_ERR_DB;
  return rc == SQLITE_ROW;
}

/* Runs a statement built for one row. A constraint failure only skips that row (and is
 * logged); it doesn't fail the batch, so one bad row can't stall sync. */
static mc_status run_row_sql(sqlite3 *db, const char *sql, const char *row_uid,
                             const row_state *rs, uint32_t cols, int ncols) {
  sqlite3_stmt *s;
  if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return MC_ERR_DB;
  int idx = 1;
  sqlite3_bind_text(s, idx++, row_uid, -1, SQLITE_STATIC);
  for (int i = 0; i < ncols; i++)
    if (cols >> i & 1) sqlite3_bind_value(s, idx++, rs->val[i]);
  int rc = sqlite3_step(s);
  sqlite3_finalize(s);
  if (rc == SQLITE_DONE) return MC_OK;
  if ((rc & 0xff) == SQLITE_CONSTRAINT) {
    fprintf(stderr, "musiccore: sync: skipped row %s: %s\n", row_uid, sqlite3_errmsg(db));
    return MC_OK;
  }
  return MC_ERR_DB;
}

static mc_status materialize(mc_rows_applier *a, const table_meta *m, const touched *t) {
  sqlite3 *db = a->tg.db;
  row_state rs;
  mc_status st = read_state(a, m, t->row_uid, &rs);
  if (st != MC_OK) return st;
  int exists = row_exists(db, m, t->row_uid, &st);
  char sql[2048];
  size_t n = 0;

  if (st != MC_OK) {
  } else if (!rs.live) {
    if (exists) {
      snprintf(sql, sizeof sql, "DELETE FROM %s WHERE %s = ?1", m->t->table, m->t->key);
      st = run_row_sql(db, sql, t->row_uid, &rs, 0, m->ncols);
    }
  } else if (exists) {
    uint32_t cols = t->won & rs.has;
    if (cols) {
      n += (size_t)snprintf(sql + n, sizeof sql - n, "UPDATE %s SET ", m->t->table);
      int idx = 2;
      for (int i = 0; i < m->ncols; i++) {
        if (!(cols >> i & 1)) continue;
        n += (size_t)snprintf(sql + n, sizeof sql - n, "%s%s = ?%d", idx > 2 ? ", " : "",
                              m->t->cols[i], idx);
        idx++;
      }
      snprintf(sql + n, sizeof sql - n, " WHERE %s = ?1", m->t->key);
      st = run_row_sql(db, sql, t->row_uid, &rs, cols, m->ncols);
    }
  } else if ((rs.has & m->required) == m->required) {
    n += (size_t)snprintf(sql + n, sizeof sql - n, "INSERT INTO %s (%s", m->t->table, m->t->key);
    for (int i = 0; i < m->ncols; i++)
      if (rs.has >> i & 1) n += (size_t)snprintf(sql + n, sizeof sql - n, ", %s", m->t->cols[i]);
    n += (size_t)snprintf(sql + n, sizeof sql - n, ") VALUES (?1");
    int idx = 2;
    for (int i = 0; i < m->ncols; i++)
      if (rs.has >> i & 1) n += (size_t)snprintf(sql + n, sizeof sql - n, ", ?%d", idx++);
    snprintf(sql + n, sizeof sql - n, ")");
    st = run_row_sql(db, sql, t->row_uid, &rs, rs.has, m->ncols);
  }
  state_free(&rs);
  return st;
}

/* A touched row's references to parents that don't exist; see the comment above. */
static mc_status fix_references(mc_rows_applier *a, const table_meta *m, const touched *t) {
  sqlite3 *db = a->tg.db;
  for (int k = 0; k < m->nfks; k++) {
    const fk_info *f = &m->fks[k];
    const char *col = m->t->cols[f->col];
    char sql[512];
    snprintf(sql, sizeof sql,
             "SELECT 1 FROM %s c WHERE c.%s = ?1 AND c.%s IS NOT NULL"
             " AND NOT EXISTS (SELECT 1 FROM %s p WHERE p.%s = c.%s)",
             m->t->table, m->t->key, col, f->parent, f->parent_col, col);
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return MC_ERR_DB;
    sqlite3_bind_text(s, 1, t->row_uid, -1, SQLITE_STATIC);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    if (rc == SQLITE_DONE) continue;
    if (rc != SQLITE_ROW) return MC_ERR_DB;
    if (f->notnull)
      snprintf(sql, sizeof sql, "DELETE FROM %s WHERE %s = ?1", m->t->table, m->t->key);
    else
      snprintf(sql, sizeof sql, "UPDATE %s SET %s = NULL WHERE %s = ?1", m->t->table, col,
               m->t->key);
    mc_status st = run_row_sql(db, sql, t->row_uid, NULL, 0, 0);
    if (st != MC_OK) return st;
    if (f->notnull) break; /* the row is gone */
  }
  return MC_OK;
}

/* Finds the classes the changelog now joins, and makes the touched rows of mergeable tables
 * their classes' canonical rows, with every field to be set from the class. Each uid merged
 * away now is touched too, so its class's canonical row and recents row are rebuilt. */
static mc_status merge_classes(mc_rows_applier *a, mc_merge_pair **pairs, int *npairs) {
  mc_status st = mc_merge_scan(a->tg.db, pairs, npairs);
  for (int i = 0; st == MC_OK && i < *npairs; i++) {
    const char *names[] = {(*pairs)[i].table, "recents"};
    for (int k = 0; k < 2; k++) {
      int t = find_table(a, names[k]);
      if (t >= 0 && !touch(a, t, (*pairs)[i].loser)) st = MC_ERR_NOMEM;
    }
  }
  for (int i = 0; st == MC_OK && i < a->ntouched; i++) {
    touched *tc = &a->touched[i];
    const table_meta *m = &a->tables[tc->table];
    if (!mc_merge_table(m->t->table)) continue;
    char c[MC_UID_STRLEN];
    int merged;
    if ((st = mc_merge_canonical(a->tg.db, tc->row_uid, c, &merged)) != MC_OK || !merged)
      continue;
    memcpy(tc->row_uid, c, MC_UID_STRLEN);
    tc->won = m->ncols < 32 ? (1u << m->ncols) - 1 : ~0u;
  }
  return st;
}

static mc_status settle(mc_rows_applier *a, mc_status st);

mc_status mc_rows_apply(mc_rows_applier *a, const mc_row *rows, int count, int *logged) {
  if (!a || (!rows && count) || count < 0) return MC_ERR_INVALID_ARG;
  if (logged) *logged = 0;
  mc_status st = a->tg.begin(a->tg.owner);
  if (st != MC_OK) return st;
  *a->tg.applying_remote = 1;
  a->ntouched = 0;
  int n_new = 0;
  if (sqlite3_exec(a->tg.db, "PRAGMA defer_foreign_keys = ON", NULL, NULL, NULL) != SQLITE_OK)
    st = MC_ERR_DB;

  for (int i = 0; st == MC_OK && i < count; i++) {
    const mc_row *r = &rows[i];
    mc_hlc_observe(a->tg.hlc, r->hlc);
    int is_new, won;
    if ((st = log_row(a, r, &is_new, &won)) != MC_OK) break;
    if (!is_new) continue;
    n_new++;
    int t = find_table(a, r->entity);
    if (t < 0) continue; /* kept for peers that know it */
    int col = -1;
    if (strcmp(r->field, "_deleted") && (col = col_index(&a->tables[t], r->field)) < 0) continue;
    touched *tc = touch(a, t, r->row_uid);
    if (!tc) {
      st = MC_ERR_NOMEM;
      break;
    }
    if (won && col >= 0) tc->won |= 1u << col;
  }

  st = settle(a, st);
  if (st == MC_OK && logged) *logged = n_new;
  return st;
}

mc_status mc_rows_reapply(mc_rows_applier *a, const char *where) {
  if (!a || !where) return MC_ERR_INVALID_ARG;
  mc_status st = a->tg.begin(a->tg.owner);
  if (st != MC_OK) return st;
  *a->tg.applying_remote = 1;
  a->ntouched = 0;
  if (sqlite3_exec(a->tg.db, "PRAGMA defer_foreign_keys = ON", NULL, NULL, NULL) != SQLITE_OK)
    st = MC_ERR_DB;
  char *sql = sqlite3_mprintf("SELECT DISTINCT entity, row_uid, field FROM changelog WHERE %s", where);
  sqlite3_stmt *s = NULL;
  if (st == MC_OK && (!sql || sqlite3_prepare_v2(a->tg.db, sql, -1, &s, NULL) != SQLITE_OK))
    st = MC_ERR_DB;
  sqlite3_free(sql);
  int rc = SQLITE_DONE;
  while (st == MC_OK && (rc = sqlite3_step(s)) == SQLITE_ROW) {
    const char *entity = (const char *)sqlite3_column_text(s, 0);
    const char *row_uid = (const char *)sqlite3_column_text(s, 1);
    const char *field = (const char *)sqlite3_column_text(s, 2);
    int t = entity ? find_table(a, entity) : -1;
    if (t < 0 || !row_uid || strlen(row_uid) != UID_LEN || !field) continue;
    int col = strcmp(field, "_deleted") ? col_index(&a->tables[t], field) : -1;
    if (col < 0 && strcmp(field, "_deleted")) continue;
    touched *tc = touch(a, t, row_uid);
    if (!tc) st = MC_ERR_NOMEM;
    else if (col >= 0) tc->won |= 1u << col; /* every field is set to its winner */
  }
  if (st == MC_OK && rc != SQLITE_DONE) st = MC_ERR_DB;
  sqlite3_finalize(s);
  return settle(a, st);
}

/* The rest of a batch once the touched rows are known: merges, rows brought in line with
 * the changelog, references; then the commit (or, if st or any of it failed, the rollback). */
static mc_status settle(mc_rows_applier *a, mc_status st) {
  mc_merge_pair *pairs = NULL;
  int npairs = 0;
  if (st == MC_OK && a->tg.merge) st = merge_classes(a, &pairs, &npairs);

  /* Parents before children, then the merges, then the references. */
  for (size_t t = 0; st == MC_OK && t < a->tg.ntables; t++)
    for (int i = 0; st == MC_OK && i < a->ntouched; i++)
      if (a->touched[i].table == a->order[t])
        st = materialize(a, &a->tables[a->order[t]], &a->touched[i]);
  for (int i = 0; st == MC_OK && i < npairs; i++) st = mc_merge_rewrite(a->tg.db, &pairs[i]);
  free(pairs);
  for (int i = 0; st == MC_OK && a->tg.merge && i < a->ntouched; i++)
    st = mc_merge_map_row(a->tg.db, a->tables[a->touched[i].table].t->table,
                          a->touched[i].row_uid);
  for (int i = 0; st == MC_OK && i < a->ntouched; i++) {
    const table_meta *m = &a->tables[a->touched[i].table];
    if (m->nfks) st = fix_references(a, m, &a->touched[i]);
  }

  *a->tg.applying_remote = 0;
  if (st == MC_OK) st = a->tg.commit(a->tg.owner);
  if (st != MC_OK) {
    fprintf(stderr, "musiccore: sync: applying rows failed: %s\n", sqlite3_errmsg(a->tg.db));
    a->tg.rollback(a->tg.owner);
    return st;
  }
  return MC_OK;
}

/* --- Digests --- */

void mc_rows_vv_digest_of(const mc_origin_hlc *vv, int count, char out[MC_DIGEST_STRLEN]) {
  static const char hex[] = "0123456789abcdef";
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  for (int i = 0; i < count; i++) {
    char line[MC_UID_STRLEN + 24];
    int n = snprintf(line, sizeof line, "%s=%llu\n", vv[i].origin_uid,
                     (unsigned long long)vv[i].hlc);
    mbedtls_sha256_update(&ctx, (const unsigned char *)line, (size_t)n);
  }
  unsigned char digest[32];
  mbedtls_sha256_finish(&ctx, digest);
  mbedtls_sha256_free(&ctx);
  for (int i = 0; i < 32; i++) {
    out[2 * i] = hex[digest[i] >> 4];
    out[2 * i + 1] = hex[digest[i] & 15];
  }
  out[64] = '\0';
}

mc_status mc_rows_vv_digest(sqlite3 *db, char out[MC_DIGEST_STRLEN]) {
  mc_origin_hlc *vv;
  int count;
  mc_status st = mc_changelog_version_vector(db, &vv, &count);
  if (st != MC_OK) return st;
  mc_rows_vv_digest_of(vv, count, out);
  free(vv);
  return MC_OK;
}

/* --- Reading --- */

struct mc_rows_reader {
  sqlite3_stmt *stmt;
};

static int64_t peer_hlc(const mc_origin_hlc *vv, int count, const char *origin) {
  for (int i = 0; i < count; i++)
    if (!strcmp(vv[i].origin_uid, origin)) return (int64_t)vv[i].hlc;
  return -1;
}

mc_status mc_rows_reader_open(sqlite3 *db, const mc_origin_hlc *peer_vv, int peer_count,
                              int resend, mc_rows_reader **out) {
  if (!db || (!peer_vv && peer_count) || !out) return MC_ERR_INVALID_ARG;
  *out = NULL;
  mc_origin_hlc *mine;
  int n;
  mc_status st = mc_changelog_version_vector(db, &mine, &n);
  if (st != MC_OK) return st;

  /* From which hlc to send each origin we have. */
  st = MC_ERR_DB;
  sqlite3_stmt *ins = NULL;
  int64_t lowest = INT64_MAX;
  if (sqlite3_exec(db,
                   "CREATE TEMP TABLE IF NOT EXISTS mc_send_from"
                   " (origin TEXT PRIMARY KEY, hlc INTEGER NOT NULL);"
                   "DELETE FROM temp.mc_send_from;",
                   NULL, NULL, NULL) != SQLITE_OK ||
      sqlite3_prepare_v2(db, "INSERT INTO temp.mc_send_from (origin, hlc) VALUES (?1, ?2)", -1,
                         &ins, NULL) != SQLITE_OK)
    goto done;
  for (int i = 0; i < n; i++) {
    int64_t from = peer_hlc(peer_vv, peer_count, mine[i].origin_uid);
    if (resend && from != (int64_t)mine[i].hlc) from = -1;
    if (from >= (int64_t)mine[i].hlc) continue; /* nothing they lack */
    if (from < lowest) lowest = from;
    sqlite3_reset(ins);
    sqlite3_bind_text(ins, 1, mine[i].origin_uid, -1, SQLITE_STATIC);
    sqlite3_bind_int64(ins, 2, from);
    if (sqlite3_step(ins) != SQLITE_DONE) goto done;
  }

  mc_rows_reader *rd = calloc(1, sizeof *rd);
  if (!rd) {
    st = MC_ERR_NOMEM;
    goto done;
  }
  if (sqlite3_prepare_v2(db,
                         "SELECT c.hlc, c.origin_uid, c.entity, c.row_uid, c.field, c.value"
                         " FROM changelog c JOIN temp.mc_send_from s ON s.origin = c.origin_uid"
                         " WHERE c.hlc > ?1 AND c.hlc > s.hlc"
                         " ORDER BY c.hlc, c.origin_uid, c.seq",
                         -1, &rd->stmt, NULL) != SQLITE_OK) {
    free(rd);
    goto done;
  }
  sqlite3_bind_int64(rd->stmt, 1, lowest); /* INT64_MAX when there's nothing to send */
  *out = rd;
  st = MC_OK;

done:
  sqlite3_finalize(ins);
  free(mine);
  return st;
}

static int copy_text(char *dst, size_t cap, const unsigned char *src, size_t exact) {
  if (!src) return 0;
  size_t n = strlen((const char *)src);
  if (n == 0 || n >= cap || (exact && n != exact)) return 0;
  memcpy(dst, src, n + 1);
  return 1;
}

mc_status mc_rows_reader_next(mc_rows_reader *rd, mc_row *r, int *got) {
  *got = 0;
  for (;;) {
    int rc = sqlite3_step(rd->stmt);
    if (rc == SQLITE_DONE) return MC_OK;
    if (rc != SQLITE_ROW) return MC_ERR_DB;
    sqlite3_stmt *s = rd->stmt;
    memset(r, 0, sizeof *r);
    r->hlc = (uint64_t)sqlite3_column_int64(s, 0);
    if (!copy_text(r->origin, sizeof r->origin, sqlite3_column_text(s, 1), UID_LEN) ||
        !copy_text(r->entity, sizeof r->entity, sqlite3_column_text(s, 2), 0) ||
        !copy_text(r->row_uid, sizeof r->row_uid, sqlite3_column_text(s, 3), UID_LEN) ||
        !copy_text(r->field, sizeof r->field, sqlite3_column_text(s, 4), 0)) {
      fprintf(stderr, "musiccore: sync: skipped a changelog row that can't be sent\n");
      continue;
    }
    switch (sqlite3_column_type(s, 5)) {
    case SQLITE_INTEGER:
      r->tag = MC_VAL_INT;
      r->i = sqlite3_column_int64(s, 5);
      break;
    case SQLITE_FLOAT:
      r->tag = MC_VAL_REAL;
      r->r = sqlite3_column_double(s, 5);
      break;
    case SQLITE_TEXT:
      r->tag = MC_VAL_TEXT;
      r->data = sqlite3_column_text(s, 5);
      r->len = (uint32_t)sqlite3_column_bytes(s, 5);
      break;
    case SQLITE_BLOB:
      r->tag = MC_VAL_BLOB;
      r->data = sqlite3_column_blob(s, 5);
      r->len = (uint32_t)sqlite3_column_bytes(s, 5);
      break;
    default: r->tag = MC_VAL_NULL; break;
    }
    *got = 1;
    return MC_OK;
  }
}

void mc_rows_reader_close(mc_rows_reader *rd) {
  if (!rd) return;
  sqlite3_finalize(rd->stmt);
  free(rd);
}

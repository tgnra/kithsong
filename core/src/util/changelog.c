#include "util/changelog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static mc_status exec(sqlite3 *db, const char *sql) {
  return sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK ? MC_OK : MC_ERR_DB;
}

/* --- SQL functions used by the change-log triggers --- */

static void sql_hlc_now(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  (void)argc;
  (void)argv;
  const mc_changelog_ctx *c = sqlite3_user_data(ctx);
  sqlite3_result_int64(ctx, (sqlite3_int64)mc_hlc_now(c->hlc));
}

static void sql_node_uid(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  (void)argc;
  (void)argv;
  const mc_changelog_ctx *c = sqlite3_user_data(ctx);
  sqlite3_result_text(ctx, c->node_uid, -1, SQLITE_TRANSIENT);
}

static void sql_applying_remote(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  (void)argc;
  (void)argv;
  const mc_changelog_ctx *c = sqlite3_user_data(ctx);
  sqlite3_result_int(ctx, *c->applying_remote);
}

mc_status mc_changelog_register(sqlite3 *db, const mc_changelog_ctx *ctx) {
  static const struct {
    const char *name;
    void (*fn)(sqlite3_context *, int, sqlite3_value **);
  } fns[] = {
    {"mc_hlc_now", sql_hlc_now},
    {"mc_node_uid", sql_node_uid},
    {"mc_applying_remote", sql_applying_remote},
  };
  for (size_t i = 0; i < sizeof fns / sizeof fns[0]; i++) {
    /* SQLITE_INNOCUOUS is required for use inside triggers while trusted_schema is OFF. */
    int rc = sqlite3_create_function(db, fns[i].name, 0, SQLITE_UTF8 | SQLITE_INNOCUOUS,
                                     (void *)ctx, fns[i].fn, NULL, NULL);
    if (rc != SQLITE_OK) return MC_ERR_DB;
  }
  return MC_OK;
}

/* --- Triggers --- */

/* Whether the trigger called name exists as create (its CREATE TRIGGER statement, which
 * sqlite_master keeps as written) already. */
static int trigger_is(sqlite3 *db, const char *name, const char *create) {
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db, "SELECT sql FROM sqlite_master WHERE type = 'trigger' AND name = ?1", -1, &stmt,
                         NULL) != SQLITE_OK)
    return 0;
  sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
  const unsigned char *sql = sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_text(stmt, 0) : NULL;
  int same = sql && strcmp((const char *)sql, create) == 0;
  sqlite3_finalize(stmt);
  return same;
}

/* Makes the trigger called name be create, replacing one that differs. One that's already as
 * wanted is left alone: every open would otherwise rewrite the schema, and with it invalidate
 * every other connection's prepared statements. */
static mc_status ensure_trigger(sqlite3 *db, const char *name, const char *create) {
  if (trigger_is(db, name, create)) return MC_OK;
  char drop[160];
  snprintf(drop, sizeof drop, "DROP TRIGGER IF EXISTS %s", name);
  mc_status st = exec(db, drop);
  return st == MC_OK ? exec(db, create) : st;
}

mc_status mc_changelog_install_triggers(sqlite3 *db, const mc_changelog_table *tables,
                                        size_t count) {
  char name[128], sql[1024];
  if (exec(db, "BEGIN IMMEDIATE") != MC_OK) return MC_ERR_DB;

  for (size_t t = 0; t < count; t++) {
    const char *table = tables[t].table, *key = tables[t].key;
    for (const char *const *c = tables[t].cols; *c; c++) {
      snprintf(name, sizeof name, "mc_log_ins_%s_%s", table, *c);
      snprintf(sql, sizeof sql,
               "CREATE TRIGGER %s AFTER INSERT ON %s"
               " WHEN new.%s IS NOT NULL AND mc_applying_remote() = 0 BEGIN"
               " INSERT INTO changelog (hlc, origin_uid, entity, row_uid, field, value)"
               " VALUES (mc_hlc_now(), mc_node_uid(), '%s', new.%s, '%s', new.%s);"
               " END",
               name, table, *c, table, key, *c, *c);
      if (ensure_trigger(db, name, sql) != MC_OK) goto fail;

      snprintf(name, sizeof name, "mc_log_upd_%s_%s", table, *c);
      snprintf(sql, sizeof sql,
               "CREATE TRIGGER %s AFTER UPDATE OF %s ON %s"
               " WHEN old.%s IS NOT new.%s AND mc_applying_remote() = 0 BEGIN"
               " INSERT INTO changelog (hlc, origin_uid, entity, row_uid, field, value)"
               " VALUES (mc_hlc_now(), mc_node_uid(), '%s', new.%s, '%s', new.%s);"
               " END",
               name, *c, table, *c, *c, table, key, *c, *c);
      if (ensure_trigger(db, name, sql) != MC_OK) goto fail;
    }

    snprintf(name, sizeof name, "mc_log_del_%s", table);
    snprintf(sql, sizeof sql,
             "CREATE TRIGGER %s AFTER DELETE ON %s"
             " WHEN mc_applying_remote() = 0 BEGIN"
             " INSERT INTO changelog (hlc, origin_uid, entity, row_uid, field, value)"
             " VALUES (mc_hlc_now(), mc_node_uid(), '%s', old.%s, '_deleted', 1);"
             " END",
             name, table, table, key);
    if (ensure_trigger(db, name, sql) != MC_OK) goto fail;
  }
  return exec(db, "COMMIT");

fail:
  exec(db, "ROLLBACK");
  return MC_ERR_DB;
}

/* --- Clock and version vector --- */

uint64_t mc_changelog_max_hlc(sqlite3 *db) {
  sqlite3_stmt *stmt;
  uint64_t hlc = 0;
  if (sqlite3_prepare_v2(db, "SELECT max(hlc) FROM changelog", -1, &stmt, NULL) != SQLITE_OK)
    return 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) hlc = (uint64_t)sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  return hlc;
}

mc_status mc_changelog_begin_write(sqlite3 *db, mc_hlc *hlc) {
  if (exec(db, "BEGIN IMMEDIATE") != MC_OK) return MC_ERR_DB;
  uint64_t seen = mc_changelog_max_hlc(db);
  if (seen > hlc->last) hlc->last = seen;
  return MC_OK;
}

mc_status mc_changelog_version_vector(sqlite3 *db, mc_origin_hlc **out, int *count) {
  if (!db || !out || !count) return MC_ERR_INVALID_ARG;
  *out = NULL;
  *count = 0;

  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db,
                         "SELECT origin_uid, max(hlc) FROM changelog"
                         " GROUP BY origin_uid ORDER BY origin_uid",
                         -1, &stmt, NULL) != SQLITE_OK)
    return MC_ERR_DB;

  mc_origin_hlc *items = NULL;
  int n = 0, cap = 0, rc;
  mc_status st = MC_OK;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const unsigned char *origin = sqlite3_column_text(stmt, 0);
    /* Origins are always uids; anything else would be a corrupt row, and can't be sent. */
    if (!origin || strlen((const char *)origin) != MC_UID_STRLEN - 1) {
      st = MC_ERR_FORMAT;
      break;
    }
    if (n == cap) {
      int grown = cap ? cap * 2 : 8;
      mc_origin_hlc *more = realloc(items, (size_t)grown * sizeof *more);
      if (!more) {
        st = MC_ERR_NOMEM;
        break;
      }
      items = more;
      cap = grown;
    }
    memcpy(items[n].origin_uid, origin, MC_UID_STRLEN);
    items[n].hlc = (uint64_t)sqlite3_column_int64(stmt, 1);
    n++;
  }
  if (st == MC_OK && rc != SQLITE_DONE) st = MC_ERR_DB;
  sqlite3_finalize(stmt);
  if (st != MC_OK) {
    free(items);
    return st;
  }
  *out = items;
  *count = n;
  return MC_OK;
}

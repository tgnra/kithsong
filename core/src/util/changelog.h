/* Field-level change logging shared by every replicated database (library.db, sync.db).
 *
 * Each database has a `changelog` table of the same shape (seq, hlc, origin_uid, entity,
 * row_uid, field, value) and AFTER INSERT/UPDATE/DELETE triggers on its tracked tables that
 * write one row per changed column. The triggers call three SQL functions, registered per
 * connection and bound to that connection's clock and identity: mc_hlc_now(),
 * mc_node_uid() and mc_applying_remote(). */
#ifndef MC_UTIL_CHANGELOG_H
#define MC_UTIL_CHANGELOG_H

#include <sqlite3.h>
#include <stddef.h>
#include <stdint.h>

#include "musiccore/musiccore.h"
#include "util/hlc.h"

/* A tracked table: changes to cols (NULL-terminated) are logged under row_uid = key. */
typedef struct mc_changelog_table {
  const char *table;
  const char *key;
  const char *const *cols;
} mc_changelog_table;

/* What the trigger functions read. The owner keeps it alive as long as the connection and
 * may change what it points at (e.g. a rotated node uid) at any time. */
typedef struct mc_changelog_ctx {
  mc_hlc *hlc;
  const char *node_uid;
  /* Set while applying changes received from a peer, so triggers don't log them a second
   * time as local edits; sync inserts those changelog rows itself. */
  const int *applying_remote;
} mc_changelog_ctx;

/* Registers mc_hlc_now(), mc_node_uid() and mc_applying_remote() on db. */
mc_status mc_changelog_register(sqlite3 *db, const mc_changelog_ctx *ctx);

/* One INSERT and one UPDATE trigger per tracked column, plus a DELETE trigger per table, in
 * one transaction. Recreated on every open so changes to the column lists take effect
 * immediately. */
mc_status mc_changelog_install_triggers(sqlite3 *db, const mc_changelog_table *tables,
                                        size_t count);

/* Newest hlc in the changelog, from any origin; 0 if it's empty. */
uint64_t mc_changelog_max_hlc(sqlite3 *db);

/* BEGIN IMMEDIATE, then catches hlc up to the newest logged change. Several connections to
 * one database can exist, each with its own clock; taking the write lock first keeps
 * changelog hlcs strictly increasing per device. */
mc_status mc_changelog_begin_write(sqlite3 *db, mc_hlc *hlc);

/* The newest change seen from each origin. Derived from the changelog itself rather than
 * stored, so it can never disagree with the data. */
typedef struct mc_origin_hlc {
  char origin_uid[MC_UID_STRLEN];
  uint64_t hlc;
} mc_origin_hlc;

/* Sets *out to a malloc'd array of *count entries, ordered by origin_uid (NULL if there are
 * none). Free with free(). */
mc_status mc_changelog_version_vector(sqlite3 *db, mc_origin_hlc **out, int *count);

#endif

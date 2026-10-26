/* Changelog rows on the wire and applying them: the ROWS encoding, field-level
 * last-writer-wins merging, version vector digests, and reading the rows a peer is
 * missing. Works the same on library.db and sync.db. See "Session" and "Clocks and
 * conflict resolution" in docs/sync-protocol.md. Internal to core. */
#ifndef MC_SYNC_ROWS_H
#define MC_SYNC_ROWS_H

#include <sqlite3.h>
#include <stddef.h>
#include <stdint.h>

#include "musiccore/musiccore.h"
#include "util/changelog.h"
#include "util/hlc.h"

/* The <db> byte of ROWS. */
#define MC_ROWS_GROUP 0
#define MC_ROWS_LIBRARY 1

/* Value tags. */
#define MC_VAL_NULL 0
#define MC_VAL_INT 1
#define MC_VAL_REAL 2
#define MC_VAL_TEXT 3
#define MC_VAL_BLOB 4

/* Longest entity or field name: its length is one byte. */
#define MC_ROWS_NAME_MAX 255
/* Hex SHA-256 and its NUL. */
#define MC_DIGEST_STRLEN 65

typedef struct mc_row {
  uint64_t hlc;
  char origin[MC_UID_STRLEN];
  char entity[MC_ROWS_NAME_MAX + 1];
  char row_uid[MC_UID_STRLEN];
  char field[MC_ROWS_NAME_MAX + 1];
  int tag;
  int64_t i;
  double r;
  const unsigned char *data; /* text (not NUL-terminated) or blob; points into the payload */
  uint32_t len;
} mc_row;

/* --- Encoding --- */

/* Builds one ROWS payload in a caller's buffer. */
typedef struct mc_rows_writer {
  unsigned char *buf;
  size_t cap, len;
  unsigned count;
} mc_rows_writer;

void mc_rows_writer_init(mc_rows_writer *w, unsigned char *buf, size_t cap, int db);
/* Appends r: 1 if it fit, 0 if the payload is full (or at 65,535 rows). MC_ERR_FORMAT-worthy
 * rows (names too long, uids of the wrong length) are reported as -1 and not added. */
int mc_rows_writer_add(mc_rows_writer *w, const mc_row *r);
/* Patches in the count and returns the payload length. */
size_t mc_rows_writer_finish(mc_rows_writer *w);

/* Parses a ROWS payload. *rows is malloc'd (free with free()) and points into payload, so
 * keep it alive while using them. MC_ERR_FORMAT if it is malformed. */
mc_status mc_rows_decode(const unsigned char *payload, size_t len, int *db, mc_row **rows,
                         int *count);

/* --- Applying --- */

/* A database that rows are applied to, and the hooks to write to it. */
typedef struct mc_rows_target {
  sqlite3 *db;
  mc_hlc *hlc;
  int *applying_remote;
  const mc_changelog_table *tables; /* in any order; foreign keys give the dependencies */
  size_t ntables;
  void *owner;
  mc_status (*begin)(void *owner);
  mc_status (*commit)(void *owner);
  void (*rollback)(void *owner);
} mc_rows_target;

typedef struct mc_library mc_library;
typedef struct mc_sync_db mc_sync_db;
void mc_rows_target_library(mc_library *lib, mc_rows_target *out);
void mc_rows_target_group(mc_sync_db *sdb, mc_rows_target *out);

typedef struct mc_rows_applier mc_rows_applier;

/* Reads the tables' columns, NOT NULL constraints and foreign keys. */
mc_status mc_rows_applier_new(const mc_rows_target *target, mc_rows_applier **out);
void mc_rows_applier_free(mc_rows_applier *a);

/* Applies one batch in one transaction, with applying_remote set. Every row new to the
 * changelog is logged there unchanged; a field only reaches its table if its row beats
 * the current winner for (entity, row_uid, field) by (hlc, origin_uid). Rows naming an
 * unknown entity or field are logged but never applied. *logged counts the new rows. */
mc_status mc_rows_apply(mc_rows_applier *a, const mc_row *rows, int count, int *logged);

/* --- Reading and comparing --- */

/* hex SHA-256 over the version vector: origins sorted, each "<origin>=<hlc>\n". */
mc_status mc_rows_vv_digest(sqlite3 *db, char out[MC_DIGEST_STRLEN]);
/* The digest of a version vector already in hand (sorted by origin). */
void mc_rows_vv_digest_of(const mc_origin_hlc *vv, int count, char out[MC_DIGEST_STRLEN]);

/* Iterates the changelog rows a peer is missing, in hlc order, from one read snapshot.
 * peer_vv is the peer's version vector. With resend, origins whose newest hlc differs
 * between the two vectors are sent in full, from hlc 0. */
typedef struct mc_rows_reader mc_rows_reader;
mc_status mc_rows_reader_open(sqlite3 *db, const mc_origin_hlc *peer_vv, int peer_count,
                              int resend, mc_rows_reader **out);
/* Fills r (whose data points into the reader until the next call): MC_OK and *got = 1 for
 * a row, *got = 0 at the end. */
mc_status mc_rows_reader_next(mc_rows_reader *rd, mc_row *r, int *got);
void mc_rows_reader_close(mc_rows_reader *rd);

#endif

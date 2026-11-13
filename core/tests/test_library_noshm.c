/* A library in a folder whose file system can't share SQLite's -shm file, as the document
 * portal's FUSE mount can't (a folder a Flatpak gets through the file chooser): the library
 * opens with a rollback journal instead of WAL, new or already in WAL, and two connections
 * still work together. A VFS over the default one stands in for that file system, failing
 * every -shm lock with SQLITE_IOERR_LOCK as the portal does. Opened without the background
 * refresh, whose connection could open after the test changes file systems. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kithcore/kithcore.h"
#include "library/library_internal.h"
#include "test.h"

#ifndef _WIN32 /* the library opens through a named VFS there, which this can't stand in for */

/* --- A file system without shared memory --- */

typedef struct shim_file {
  sqlite3_file base;
  sqlite3_file *real; /* follows this struct, in the same allocation */
} shim_file;

static sqlite3_vfs *real_vfs;
static sqlite3_io_methods shim_methods;
static int shm_failures;

#define REAL(f) (((shim_file *)(f))->real)

static int shim_close(sqlite3_file *f) {
  int rc = REAL(f)->pMethods ? REAL(f)->pMethods->xClose(REAL(f)) : SQLITE_OK;
  return rc;
}
static int shim_read(sqlite3_file *f, void *buf, int n, sqlite3_int64 off) {
  return REAL(f)->pMethods->xRead(REAL(f), buf, n, off);
}
static int shim_write(sqlite3_file *f, const void *buf, int n, sqlite3_int64 off) {
  return REAL(f)->pMethods->xWrite(REAL(f), buf, n, off);
}
static int shim_truncate(sqlite3_file *f, sqlite3_int64 size) {
  return REAL(f)->pMethods->xTruncate(REAL(f), size);
}
static int shim_sync(sqlite3_file *f, int flags) {
  return REAL(f)->pMethods->xSync(REAL(f), flags);
}
static int shim_size(sqlite3_file *f, sqlite3_int64 *size) {
  return REAL(f)->pMethods->xFileSize(REAL(f), size);
}
static int shim_lock(sqlite3_file *f, int lock) {
  return REAL(f)->pMethods->xLock(REAL(f), lock);
}
static int shim_unlock(sqlite3_file *f, int lock) {
  return REAL(f)->pMethods->xUnlock(REAL(f), lock);
}
static int shim_check_lock(sqlite3_file *f, int *out) {
  return REAL(f)->pMethods->xCheckReservedLock(REAL(f), out);
}
static int shim_control(sqlite3_file *f, int op, void *arg) {
  return REAL(f)->pMethods->xFileControl(REAL(f), op, arg);
}
static int shim_sector(sqlite3_file *f) {
  return REAL(f)->pMethods->xSectorSize(REAL(f));
}
static int shim_characteristics(sqlite3_file *f) {
  return REAL(f)->pMethods->xDeviceCharacteristics(REAL(f));
}
/* The -shm file opens and maps, as on the portal; taking a lock on it fails. */
static int shim_shm_map(sqlite3_file *f, int page, int size, int extend, void volatile **out) {
  return REAL(f)->pMethods->xShmMap(REAL(f), page, size, extend, out);
}
static int shim_shm_lock(sqlite3_file *f, int offset, int n, int flags) {
  (void)f;
  (void)offset;
  (void)n;
  (void)flags;
  shm_failures++;
  return SQLITE_IOERR_LOCK;
}
static void shim_shm_barrier(sqlite3_file *f) {
  REAL(f)->pMethods->xShmBarrier(REAL(f));
}
static int shim_shm_unmap(sqlite3_file *f, int del) {
  return REAL(f)->pMethods->xShmUnmap(REAL(f), del);
}

static int shim_open(sqlite3_vfs *vfs, const char *name, sqlite3_file *f, int flags, int *out_flags) {
  (void)vfs;
  shim_file *s = (shim_file *)f;
  s->real = (sqlite3_file *)(s + 1);
  int rc = real_vfs->xOpen(real_vfs, name, s->real, flags, out_flags);
  s->base.pMethods = s->real->pMethods ? &shim_methods : NULL;
  return rc;
}

static sqlite3_vfs shim_vfs;

static void shim_install(void) {
  real_vfs = sqlite3_vfs_find(NULL);
  shim_methods = (sqlite3_io_methods){
    .iVersion = 2,
    .xClose = shim_close,
    .xRead = shim_read,
    .xWrite = shim_write,
    .xTruncate = shim_truncate,
    .xSync = shim_sync,
    .xFileSize = shim_size,
    .xLock = shim_lock,
    .xUnlock = shim_unlock,
    .xCheckReservedLock = shim_check_lock,
    .xFileControl = shim_control,
    .xSectorSize = shim_sector,
    .xDeviceCharacteristics = shim_characteristics,
    .xShmMap = shim_shm_map,
    .xShmLock = shim_shm_lock,
    .xShmBarrier = shim_shm_barrier,
    .xShmUnmap = shim_shm_unmap,
  };
  shim_vfs = *real_vfs;
  shim_vfs.zName = "noshm";
  shim_vfs.szOsFile = (int)sizeof(shim_file) + real_vfs->szOsFile;
  shim_vfs.xOpen = shim_open;
  shim_vfs.pNext = NULL;
}

/* Which file system new connections use. */
static void use_noshm(int on) {
  sqlite3_vfs_register(on ? &shim_vfs : real_vfs, 1);
}

/* --- Tests --- */

static int query_int(sqlite3 *db, const char *sql) {
  sqlite3_stmt *stmt;
  int n = -1;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
  if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return n;
}

static int is_mode(mc_library *lib, const char *mode) {
  sqlite3_stmt *stmt;
  int same = 0;
  if (sqlite3_prepare_v2(mc_library_db(lib), "PRAGMA journal_mode", -1, &stmt, NULL) != SQLITE_OK) return 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) same = !strcmp((const char *)sqlite3_column_text(stmt, 0), mode);
  sqlite3_finalize(stmt);
  return same;
}

static int exec(mc_library *lib, const char *sql) {
  return sqlite3_exec(mc_library_db(lib), sql, NULL, NULL, NULL);
}

static void remove_db(const char *dir) {
  const char *const names[] = {MC_LIBRARY_DB_NAME, MC_LIBRARY_DB_NAME "-wal", MC_LIBRARY_DB_NAME "-shm",
                               MC_LIBRARY_DB_NAME "-journal"};
  char file[1100];
  for (int i = 0; i < 4; i++) {
    snprintf(file, sizeof file, "%s/%s", dir, names[i]);
    remove(file);
  }
}

/* A new library there: rollback journal, and a second connection writes while the first
 * reads, as the refresh and import threads do. */
static void test_new(const char *scratch) {
  char dir[1024];
  snprintf(dir, sizeof dir, "%s/test_library_noshm_new.d", scratch);
  remove_db(dir);
  use_noshm(1);
  shm_failures = 0;

  mc_library *lib = NULL, *other = NULL;
  CHECK(mc_library_open_internal(dir, &lib) == MC_OK);
  CHECK(shm_failures > 0); /* WAL was tried first */
  if (!lib) goto done;
  CHECK(is_mode(lib, "delete"));
  CHECK(mc_library_schema_version(lib) == mc_library_schema_latest());
  CHECK(exec(lib, "INSERT INTO artists (uid, name) VALUES ('a1', 'Low')") == SQLITE_OK);

  /* The second connection finds it in DELETE mode and leaves it so, never trying WAL. */
  shm_failures = 0;
  CHECK(mc_library_open_internal(dir, &other) == MC_OK);
  CHECK(shm_failures == 0);
  if (!other) goto done;
  CHECK(is_mode(other, "delete"));
  CHECK(query_int(mc_library_db(other), "SELECT count(*) FROM artists") == 1);
  CHECK(exec(other, "INSERT INTO artists (uid, name) VALUES ('a2', 'Duster')") == SQLITE_OK);
  CHECK(query_int(mc_library_db(lib), "SELECT count(*) FROM artists") == 2);
  CHECK(exec(lib, "INSERT INTO artists (uid, name) VALUES ('a3', 'Grouper')") == SQLITE_OK);
  CHECK(query_int(mc_library_db(other), "SELECT count(*) FROM artists") == 3);

done:
  mc_library_close(other);
  mc_library_close(lib);
  use_noshm(0);
}

/* A library made in WAL on a local disk, then opened there: it goes to DELETE with
 * everything in it, an uncheckpointed write in the -wal file too. Back on the local disk it
 * stays in DELETE (see open_db() in library.c), and a new library there still gets WAL. */
static void test_existing(const char *scratch) {
  char dir[1024], fresh[1100];
  snprintf(dir, sizeof dir, "%s/test_library_noshm_old.d", scratch);
  snprintf(fresh, sizeof fresh, "%s/test_library_noshm_fresh.d", scratch);
  remove_db(dir);
  remove_db(fresh);

  mc_library *lib = NULL, *keep = NULL;
  CHECK(mc_library_open_internal(dir, &lib) == MC_OK);
  if (!lib) return;
  CHECK(is_mode(lib, "wal"));
  CHECK(exec(lib, "PRAGMA wal_autocheckpoint = 0;"
                  "INSERT INTO artists (uid, name) VALUES ('a1', 'Low')") == SQLITE_OK);
  /* Another connection keeps the -wal file from being checkpointed when lib closes. */
  char db_path[1100];
  snprintf(db_path, sizeof db_path, "%s/%s", dir, MC_LIBRARY_DB_NAME);
  sqlite3 *hold = NULL;
  CHECK(sqlite3_open_v2(db_path, &hold, SQLITE_OPEN_READWRITE, NULL) == SQLITE_OK);
  CHECK(query_int(hold, "SELECT count(*) FROM artists") == 1);
  CHECK(sqlite3_db_config(hold, SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE, 1, NULL) == SQLITE_OK);
  mc_library_close(lib);
  lib = NULL;
  sqlite3_close(hold);
  FILE *wal = NULL;
  {
    char wal_path[1200];
    snprintf(wal_path, sizeof wal_path, "%s-wal", db_path);
    wal = fopen(wal_path, "rb");
    CHECK(wal != NULL); /* the row is still only in the -wal file */
    if (wal) {
      fseek(wal, 0, SEEK_END);
      CHECK(ftell(wal) > 0);
      fclose(wal);
    }
  }

  use_noshm(1);
  CHECK(mc_library_open_internal(dir, &lib) == MC_OK);
  if (lib) {
    CHECK(is_mode(lib, "delete"));
    CHECK(query_int(mc_library_db(lib), "SELECT count(*) FROM artists WHERE name = 'Low'") == 1);
    CHECK(exec(lib, "INSERT INTO artists (uid, name) VALUES ('a2', 'Duster')") == SQLITE_OK);
    mc_library_close(lib);
    lib = NULL;
  }
  use_noshm(0);

  CHECK(mc_library_open_internal(dir, &lib) == MC_OK);
  if (lib) {
    CHECK(is_mode(lib, "delete"));
    CHECK(query_int(mc_library_db(lib), "SELECT count(*) FROM artists") == 2);
  }
  CHECK(mc_library_open_internal(fresh, &keep) == MC_OK);
  if (keep) CHECK(is_mode(keep, "wal"));
  mc_library_close(keep);
  mc_library_close(lib);
}

int main(int argc, char **argv) {
  const char *scratch = argc > 1 ? argv[1] : ".";
  shim_install();
  test_new(scratch);
  test_existing(scratch);
  return TEST_RESULT();
}

#else

int main(void) {
  return 0;
}

#endif

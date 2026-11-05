#include "util/db_open.h"

#include <stdlib.h>

#include "util/platform.h"

#ifdef _WIN32
#define VFS "win32-longpath"
#else
#define VFS NULL
#endif

int mc_db_open(const char *path, sqlite3 **db, int flags) {
  *db = NULL;
  char *p = mc_open_path(path);
  if (!p) return SQLITE_NOMEM;
  int rc = sqlite3_open_v2(p, db, flags, VFS);
  free(p);
  return rc;
}

int mc_db_bind_path(sqlite3_stmt *stmt, int index, const char *path) {
  char *p = mc_open_path(path);
  return p ? sqlite3_bind_text(stmt, index, p, -1, free) : SQLITE_NOMEM;
}

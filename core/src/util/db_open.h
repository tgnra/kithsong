/* Opening SQLite databases by path, whatever the length of the path. */
#ifndef MC_UTIL_DB_OPEN_H
#define MC_UTIL_DB_OPEN_H

#include <sqlite3.h>

/* sqlite3_open_v2() on Windows' long-path VFS, with path as that wants it (mc_open_path), so a
 * library folder can be as deep as its files. It's the same VFS otherwise. *db is set
 * unless out of memory, when it's NULL. */
int mc_db_open(const char *path, sqlite3 **db, int flags);
/* Binds a path for ATTACH or VACUUM INTO on such a connection. */
int mc_db_bind_path(sqlite3_stmt *stmt, int index, const char *path);

#endif

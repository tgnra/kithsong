/* Internals of library/delete.c shared with tests. */
#ifndef MC_DELETE_INTERNAL_H
#define MC_DELETE_INTERNAL_H

#include "musiccore/musiccore.h"

/* How deleted files are trashed: mc_trash, or a stand-in for tests. */
extern mc_status (*mc_delete_trash)(const char *path);
/* How a permanent delete removes them: mc_remove, or a stand-in for tests. */
extern mc_status (*mc_delete_remove)(const char *path);

#endif

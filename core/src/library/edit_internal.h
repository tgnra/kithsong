/* Internals of library/edit.c: what opening a library and mc_init() call, and test hooks. */
#ifndef MC_LIBRARY_EDIT_INTERNAL_H
#define MC_LIBRARY_EDIT_INTERNAL_H

#include "musiccore/musiccore.h"

/* Called from mc_init(). */
void mc_edit_global_init(void);

/* Puts right an edit that a crash stopped half way through swapping its files: before its rows
 * were committed the files go back as they were, after it the old ones left over are deleted.
 * Called when a frontend opens the library. */
void mc_edit_recover(mc_library *lib);

/* The file holding album_uid's chosen cover, of SHA-256 hash, in a folder of its files here;
 * NULL if none does. Free it. */
char *mc_edit_chosen_cover(mc_library *lib, const char *album_uid, const char *hash);

/* > 0: writing the nth file's tags fails, as a full disk would. */
extern int mc_edit_test_fail_tags;
/* > 0: the nth rename of an edit's swap fails, as a file opened at that moment would. */
extern int mc_edit_test_fail_rename;
/* > 0: the edit stops dead before the nth rename of its swap, as a crash would. */
extern int mc_edit_test_abandon_rename;

#endif

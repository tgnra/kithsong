/* Whether two library paths could be the same file, whatever the file system does to names. */
#ifndef MC_UTIL_PATHKEY_H
#define MC_UTIL_PATHKEY_H

/* path's comparison key, malloc'd, or NULL: '/' for '\', decomposed (so NFC and NFD agree),
 * case folded, and combining marks dropped. Equal keys may be different files ("resume" and
 * "résumé"); different keys are never the same file on a file system the app runs on. That
 * is the way round to be wrong for deciding what may be deleted, so it's the same on every
 * platform rather than tuned to one; don't use it to decide two files are the same for
 * anything else. Broken UTF-8 is taken leniently, the same way each time. */
char *mc_path_key(const char *path);

#endif

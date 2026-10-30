/* Uids beyond mc_uid_new() (musiccore.h). Internal to core. */
#ifndef MC_UTIL_UID_H
#define MC_UTIL_UID_H

#include "musiccore/musiccore.h"

/* A uid derived from (owner, position) alone: a name-based UUID (version 8) from the SHA-256
 * of "<owner>/<position>" (position in decimal), in mc_uid_new()'s text form. Every device derives the same uid
 * for the same owner and position, so the rows two devices write under it are one row to
 * sync (the artist credit tables, docs/schema.md). */
mc_status mc_uid_derive(const char *owner, int position, char out[MC_UID_STRLEN]);

#endif

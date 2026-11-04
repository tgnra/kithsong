/* The orphan sweep: after a complete sync session or import, every file in the library folder
 * that the library doesn't know goes, of any type: to the Recycle Bin or trash on desktops,
 * deleted outright on Android, whose library is the app's private folder. Internal to core.
 * What counts as known and when it refuses is at the top of orphans.c; see "Files" in
 * docs/sync-protocol.md. */
#ifndef MC_SYNC_ORPHANS_H
#define MC_SYNC_ORPHANS_H

#include "library/library_internal.h"

/* THE GUARD. A sweep refuses when more than this many orphans stand for each known file found.
 * An existing install may point at a folder of unimported music, where a sweep would otherwise
 * take nearly everything in it. Not something the user asked for: it was added for safety, and
 * this is the one place to change it. */
#define MC_ORPHANS_MAX_PER_KNOWN 1

/* Whether a sweep on an archive only reports. The first archive is the device that held the
 * library when the group began, so usually the main computer: with this at 1 that computer
 * never loses a file to a sweep. It's what sync did for unknown files before there was a
 * sweep, kept as the safe choice until someone decides otherwise. 0 sweeps archives too. */
#define MC_ORPHANS_ARCHIVE_ONLY_REPORTS 1

/* Younger files are left for a later sweep. Age is the newest of modified and created, so a
 * file copied in with an old modified time still counts as new. */
#define MC_ORPHANS_GRACE_MS ((int64_t)60 * 60 * 1000)

typedef struct mc_orphans_args {
  mc_library *lib;
  const char *config_dir; /* this device's: never swept should it be inside the library */
  int report_only;        /* an archive: count and log them, remove nothing */
  int own_holds;          /* mc_folder_hold()s the caller has on the library folder */
  int (*stopping)(void *user); /* may be NULL; non-zero stops the sweep where it is */
  void *user;
} mc_orphans_args;

typedef struct mc_orphans_result {
  int known;   /* files found that a files row names */
  int orphans; /* files found that nothing explains, old enough to go */
  int removed; /* of those, gone */
  int failed;  /* of those, couldn't be removed; they stay until the next sweep */
  int young;   /* left for now for being too new */
} mc_orphans_result;

/* Runs a sweep on this thread, which mustn't be a UI thread: it walks the whole folder. MC_OK
 * if it ran to the end, whatever failed on single files. Any other status means it removed
 * nothing, or stopped part-way as someone came, and the reason is logged: MC_ERR_LOCKED while
 * anyone else is working in the folder, MC_ERR_IN_USE if stopping said so, MC_ERR_NOT_FOUND
 * or MC_ERR_IO for a folder it couldn't walk whole, MC_ERR_DB for a library it couldn't read,
 * and MC_ERR_INVALID_ARG for the guards. */
mc_status mc_orphans_sweep(const mc_orphans_args *args, mc_orphans_result *out);

/* After an import finished, on its thread: restores tracks the user took back out of the
 * trash, works out what sync.db says of this device, and sweeps. Skips the sweep, logged, if
 * sync isn't running on this library, or the device's profile isn't confirmed (a device
 * deletes nothing for one that isn't), or any of sources (the import's paths) overlaps the
 * library folder: what the import left there isn't for a sweep to judge. */
void mc_orphans_after_import(mc_library *lib, const char *const *sources, int source_count,
                             int own_holds, int (*stopping)(void *user), void *user);

/* Tests only. Whether a removal is for good: 1 on Android, 0 elsewhere, set so a test can take
 * Android's way. mtime_only ages files by their modified time alone, since a test can't make
 * a file's creation time old on every platform. */
extern int mc_orphans_permanent;
extern int mc_orphans_test_mtime_only;

#endif

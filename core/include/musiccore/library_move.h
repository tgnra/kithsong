/* Moving a library to another folder: the music, library.db (with its -wal and -shm) and
 * anything else in the library folder, such as cover art. sync.db, the transcode cache and
 * settings.json live in the per-device config folder, so they stay where they are.
 *
 * The frontend checks the folder the user picked with mc_library_move_check(), closes the
 * library (stopping playback, sync, imports and anything else that has it open), then runs
 * mc_library_move() on a worker thread. The library's paths are relative to its folder
 * (files.path), so the database needs no changes; any absolute ones an older version left
 * are made relative on the way.
 *
 * On the same volume the library's top-level folders and files are renamed into the new
 * folder, which is quick and needs no free space. Otherwise (or if a rename fails, e.g. on
 * a locked file) everything is copied, each copy's size is checked, the copied database is
 * opened and checked, settings are pointed at the new folder and saved, and only then are
 * the old files removed. Until settings are saved a failure undoes everything: the old
 * library is left as it was and the partial copy is removed. The old folder itself is kept,
 * empty, as it may be a folder the user or the OS made (e.g. Music).
 *
 * Symbolic links and junctions in the library aren't followed or moved, and neither is a
 * desktop.ini or .directory at the top (they belong to the folder, e.g. Windows' name for Music
 * or KDE's icon for it); they stay behind in the old folder. */
#ifndef MUSICCORE_LIBRARY_MOVE_H
#define MUSICCORE_LIBRARY_MOVE_H

#include "musiccore/musiccore.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mc_settings mc_settings; /* settings.h */

/* Why a folder can't take the library. */
typedef enum mc_move_problem {
  MC_MOVE_OK = 0,
  MC_MOVE_SAME_FOLDER,      /* it's the library's folder */
  MC_MOVE_INSIDE_LIBRARY,   /* it's inside the library's folder */
  MC_MOVE_CONTAINS_LIBRARY, /* the library's folder is inside it */
  MC_MOVE_NOT_EMPTY,        /* it has files or folders in it, or is a file */
  MC_MOVE_NOT_WRITABLE,     /* it (or, if it doesn't exist yet, the folder it would be made in) can't be written */
  MC_MOVE_NO_SPACE          /* its volume has less free space than needed_bytes */
} mc_move_problem;

typedef struct mc_library_move_info {
  mc_move_problem problem;
  int64_t library_bytes; /* what the library folder holds */
  int file_count;
  int same_volume;       /* the move will rename rather than copy */
  int64_t needed_bytes;  /* library_bytes plus a margin (2% and 64 MiB); 0 on the same volume */
  int64_t free_bytes;    /* on the destination's volume; -1 if unknown */
} mc_library_move_info;

/* Whether dir is an empty folder, or doesn't exist (which mc_library_move() is also happy
 * with, if the folder it would be made in can be written). */
int mc_folder_is_empty(const char *dir);
/* Whether a library may be opened in dir: it holds one already, or it's empty or missing, so
 * one can be made there. Anything else is a folder of other things: a new library in it would
 * have them all count as orphans (see "Files" in docs/sync-protocol.md). For the routes where
 * the user picks a folder; an install already in such a folder keeps working. */
int mc_folder_fits_library(const char *dir);
/* Whether Create a new library may use dir: it's empty or missing, or holds nothing but a library
 * with no tracks, as an earlier try leaves it. That library is opened as it is. */
int mc_folder_takes_new_library(const char *dir);

/* Bytes free for this user on the volume that holds dir, or would hold it: a folder that
 * doesn't exist yet is measured at its nearest existing parent. */
mc_status mc_folder_free_space(const char *dir, int64_t *out);

/* The size of everything under dir, as mc_library_move() would move it. */
mc_status mc_folder_size(const char *dir, int64_t *bytes, int *files);

/* Checks whether the library in src can move to dst: dst must be an empty folder, or one that
 * doesn't exist yet but whose nearest existing parent can be written; mustn't be src, inside
 * it, or hold it; and its volume must have room for the library (unless it's src's volume).
 * Reads the whole library folder's listing, so it may take a moment on a big library.
 * Returns MC_OK with info->problem set, or an error if src can't be read. */
mc_status mc_library_move_check(const char *src, const char *dst, mc_library_move_info *info);

typedef enum mc_move_phase {
  MC_MOVE_PHASE_COPYING,  /* or renaming, on the same volume */
  MC_MOVE_PHASE_CHECKING, /* opening the moved database */
  MC_MOVE_PHASE_REMOVING, /* removing the old files */
  MC_MOVE_PHASE_UNDOING   /* something failed: removing the partial copy */
} mc_move_phase;

typedef struct mc_library_move_progress {
  mc_move_phase phase;
  int files_done, files_total;
  int64_t bytes_done, bytes_total;
  const char *current; /* the file being copied, relative to the library folder; NULL between files */
  int can_cancel;      /* a non-zero return from the callback stops the move now */
} mc_library_move_progress;

/* Called on mc_library_move()'s thread, before each file, every few MB of a big one and as the
 * phase changes. Returning non-zero cancels the move while can_cancel says it may: while
 * copying, but not during a rename (which is quick) or after the last file. */
typedef int (*mc_library_move_fn)(void *user, const mc_library_move_progress *progress);

typedef struct mc_library_move_result {
  mc_move_problem problem; /* if the check failed (the move returns MC_ERR_INVALID_ARG) */
  char *failed_path;       /* on failure, the file it failed on, if any; free with mc_free() */
  int renamed;             /* moved by renaming, on the same volume */
  int left_behind;         /* after a move: old files that couldn't be removed */
} mc_library_move_result;

/* Moves the library in folder src to folder dst, as described at the top. The library must
 * be closed everywhere in the process. If settings isn't NULL its library folder is set to
 * dst and saved once the copy is complete and checked; failing to save undoes the move.
 * progress may be NULL. *result is always filled; free it with mc_library_move_result_free().
 * Returns MC_OK if the library is now in dst (some old files may be left behind, see
 * left_behind), else an error with the library still in src: MC_ERR_CANCELLED if the callback
 * cancelled it, with the partial copy removed. */
mc_status mc_library_move(const char *src, const char *dst, mc_settings *settings,
                          mc_library_move_fn progress, void *user, mc_library_move_result *result);
void mc_library_move_result_free(mc_library_move_result *result);

#ifdef __cplusplus
}
#endif

#endif

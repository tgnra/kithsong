/* Duplicates on import: is an incoming release already in the library, is it better, and
 * which files to skip, swap in or add. The rules are in tagger.h ("Duplicates"). Separate
 * from the import thread so it can be tested without the network. */
#ifndef MC_TAGGER_DUPES_H
#define MC_TAGGER_DUPES_H

#include <stdint.h>

#include "library/library_internal.h"
#include "tagger/import_db.h"

/* What one audio file sounds like, as far as comparing quality goes. */
typedef struct mc_audio_quality {
  const char *format; /* lowercase extension: "flac", "mp3", ... */
  int bitrate_kbps;
  int sample_rate;
  int bits_per_sample; /* 0 if unknown or lossy */
} mc_audio_quality;

int mc_quality_is_lossless(const mc_audio_quality *q);
/* > 0 if a is better than b, < 0 if worse, 0 if neither. */
int mc_quality_compare(const mc_audio_quality *a, const mc_audio_quality *b);

/* One incoming file. */
typedef struct mc_dupe_in {
  const mc_db_track *row; /* the library row it would get: ids, title, disc and track */
  const char *src_path;   /* absolute path of the file to import */
  int64_t size;
  mc_audio_quality quality;
} mc_dupe_in;

typedef enum mc_dupe_action {
  MC_DUPE_IMPORT, /* new to the library: import as usual */
  MC_DUPE_SKIP,   /* the library already has it, as good or better, or byte for byte */
  MC_DUPE_SWAP,   /* replaces track_uid's file file_uid (at old_path) */
  MC_DUPE_ATTACH  /* belongs to track_uid, which has no file on this device */
} mc_dupe_action;

typedef struct mc_dupe_decision {
  mc_dupe_action action;
  char track_uid[MC_UID_STRLEN];
  char file_uid[MC_UID_STRLEN];
  char *old_path; /* relative */
} mc_dupe_decision;

typedef struct mc_dupe_plan {
  char album_uid[MC_UID_STRLEN]; /* the release's album in the library, or "" */
  char *album_dir;               /* the folder (relative) its files are in here, or NULL */
  mc_dupe_decision *d;           /* one per incoming file, in order */
  int count;
  int swaps, skips;
} mc_dupe_plan;

/* Works out what to do with an incoming release (album describes it as mc_import_db_write
 * would get it). Reads the library's files from disk to tell their quality and compare
 * bytes. Free *out with mc_dupe_plan_free(). */
mc_status mc_dupes_plan(mc_library *lib, const mc_db_album *album, const mc_dupe_in *in,
                        int count, mc_dupe_plan *out);
void mc_dupe_plan_free(mc_dupe_plan *plan);

/* The last step of a swap, once its rows are saved (mc_import_db_write with
 * replace_file_uid): the old file (old_rel) goes to the trash, and the new one, which waited
 * at *dest_rel (absolute: *dest_abs) because old_rel was taken, moves to final_rel if that's
 * free now; *dest_rel and *dest_abs follow it. A file that can't be trashed stays where it
 * is, out of the library (sync's reconcile treats it as an unknown file). */
void mc_dupes_finish_swap(mc_library *lib, const char *old_rel, const char *final_rel,
                          char **dest_rel, char **dest_abs);

/* How old files are trashed: mc_trash, or a stand-in for tests. */
extern mc_status (*mc_dupes_trash)(const char *path);

#endif

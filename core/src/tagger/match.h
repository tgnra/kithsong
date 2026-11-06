/* Scoring for the importer: how far a MusicBrainz candidate is from the files' existing
 * tags. Distances run from 0 (identical) to 1 (nothing in common); confidence = 1 - distance.
 * Pure functions, no I/O. */
#ifndef MC_TAGGER_MATCH_H
#define MC_TAGGER_MATCH_H

#include "kithcore/tagger.h"
#include "tagger/musicbrainz.h"

/* Normalised edit distance. Ignores case, punctuation, common Latin accents, a leading
 * "The", DJ-mix markers like "(Mixed)", digit 0 for letter O and, if that brings the strings
 * closer, suffixes like "(Remastered)" or "～Subtitle", and a translation or a ": Subtitle"
 * added to one of them (each at a cost of 0.1). Returns -1 if either string is missing,
 * meaning "unknown, don't count it". */
double mc_string_distance(const char *a, const char *b);

/* mc_string_distance for album titles, which also allows the same words in another order (at
 * a cost of 0.05) and one title quoted in the other, "「Title」" (0.1). */
double mc_album_distance(const char *a, const char *b);

/* Distance between one file and one release track. */
double mc_track_distance(const mc_tags *file, const mb_track *track, int abs_index);

/* Minimum-cost assignment of rows to columns (Hungarian algorithm). cost is rows x cols,
 * row-major; any shape. out[row] receives the column, or -1 if the row is unassigned (only
 * possible when rows > cols). */
mc_status mc_assign(const double *cost, int rows, int cols, int *out);

/* Whether release track i is left out when scoring audio files: a video (a DVD-Video's
 * tracks, say) on a release that has audio tracks too. A left-out track isn't missing when
 * no file has it, and mc_score_release pairs no file with it. */
int mc_track_left_out(const mb_release *rel, int i);

typedef struct mc_release_score {
  double distance;
  int missing; /* release tracks with no file, left-out ones aside */
  int extra;   /* files with no release track */
} mc_release_score;

/* Scores a release against a group of files and fills map[file] with the release track
 * index, or -1. album/artist are the group's existing tags (may be NULL). */
mc_status mc_score_release(const mb_release *rel, const mc_tags *files, int file_count,
                           const char *album, const char *artist, int *map,
                           mc_release_score *out);

/* Scores a release against files already assigned to its tracks (map as filled by
 * mc_score_release, or changed by the user): the same distance, without the assignment, and
 * every pair in map counts however far apart they are. */
mc_status mc_score_release_map(const mb_release *rel, const mc_tags *files, int file_count,
                               const char *album, const char *artist, const int *map,
                               mc_release_score *out);

/* Scores a one-track release (a recording found for a loose track) against one file. */
double mc_score_recording(const mb_release *rec, const mc_tags *file);

#endif

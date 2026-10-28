/* Search across the library: artists, albums, tracks, playlists and album groups (manual and
 * smart), by name, forgiving typos and matching the last word as a prefix so results can
 * follow each keystroke.
 *
 * Each library handle keeps an in-memory index of the names, rebuilt on a core thread after
 * any of them changes, here or on another connection (an import, sync). Plays and recents
 * don't count as changes. A build reads the whole library, so it takes far longer than a
 * search (a few hundred ms for 100k tracks), and searches don't wait for one: until it's
 * done they search the index before the change. Only the handle's first search waits, as
 * there's nothing to search before its first build.
 *
 * Call mc_library_search_prepare() whenever the library changes (where the album grid and
 * playlists are listed again) and at startup, so the index is ready before the user types.
 * A search that finds the index out of date starts a build too. Results can name an item
 * deleted since the index was built; opening it gives MC_ERR_NOT_FOUND like any stale uid. */
#ifndef MUSICCORE_SEARCH_H
#define MUSICCORE_SEARCH_H

#include "musiccore/musiccore.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Defined in library.h, which includes this through musiccore.h before it defines it. */
typedef struct mc_library mc_library;

typedef enum mc_search_kind {
  MC_SEARCH_ARTIST,
  MC_SEARCH_ALBUM,
  MC_SEARCH_TRACK,
  MC_SEARCH_PLAYLIST,   /* a playlist or a smart playlist */
  MC_SEARCH_ALBUM_GROUP /* an album group or a smart album group */
} mc_search_kind;

typedef struct mc_search_hit {
  mc_search_kind kind;
  char uid[MC_UID_STRLEN];
  char *name;   /* its title or name */
  char *artist; /* an album's or track's artist; NULL for other kinds, or if unknown */
  char *album;  /* a track's album; NULL for other kinds, or if it has none */
  char album_uid[MC_UID_STRLEN]; /* a track's album; "" for other kinds, or if it has none */
  int is_smart; /* a smart playlist or smart album group */
  double score; /* higher is better; only comparable within one result list */
} mc_search_hit;

typedef struct mc_search_results {
  mc_search_hit *items;
  int count;
} mc_search_results;

/* What matches query, best first, at most limit_per_kind of each kind (all if <= 0). Every
 * word of the query must match a word of the item's name, or of its artist or album: exactly,
 * with 1 typo for words of 4 to 7 letters, or 2 for longer ones. Case, accents and punctuation
 * don't count ("bjork" finds Björk, "acdc" AC/DC), and "&" is "and". The last word also
 * matches the start of a word unless the query ends with a space. A query with no letters or
 * digits finds nothing. Free with mc_search_results_free(). */
mc_status mc_library_search(mc_library *lib, const char *query, int limit_per_kind,
                            mc_search_results **out);
void mc_search_results_free(mc_search_results *results);

/* Starts rebuilding the index in the background if the library changed since it was built,
 * or builds the first one. Returns at once, and costs two small queries when nothing
 * changed. Call it from the same thread as the handle's other calls. */
mc_status mc_library_search_prepare(mc_library *lib);

#ifdef __cplusplus
}
#endif

#endif

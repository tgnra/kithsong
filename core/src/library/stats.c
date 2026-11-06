/* See mc_library_read_stats() in musiccore/library.h. */
#include <string.h>

#include "library/library_internal.h"
#include "musiccore/musiccore.h"

/* Whether track t, or any track of album a, has a file here: what gives mc_track_info a path
 * and mc_album_info a folder. */
#define TRACK_HERE "EXISTS (SELECT 1 FROM files f WHERE f.track_uid = t.uid)"
#define ALBUM_HERE \
  "EXISTS (SELECT 1 FROM tracks t JOIN files f ON f.track_uid = t.uid WHERE t.album_uid = a.uid)"

/* One statement, so every number is of the same moment even with sync writing on another
 * connection. Each part gives one row. A sum over no rows is NULL, which reads as 0. Genres
 * are compared without case, ASCII's anyway, and a blank one is none. */
static const char k_stats_sql[] =
  "SELECT * FROM"
  " (SELECT count(*), sum(here), sum(duration_ms), sum(CASE WHEN here THEN duration_ms END),"
  "   count(DISTINCT genre), count(DISTINCT CASE WHEN here THEN genre END),"
  "   sum(album_uid IS NULL), sum(album_uid IS NULL AND here)"
  "  FROM (SELECT t.album_uid, t.duration_ms, nullif(lower(trim(t.genre)), '') AS genre,"
  "   " TRACK_HERE " AS here FROM tracks t)),"
  /* As mc_library_list_albums() and the singles of mc_library_list_artist_singles(). */
  " (SELECT sum(NOT single), sum(NOT single AND here), sum(single), sum(single AND here)"
  "  FROM (SELECT " ALBUM_IS_SINGLE " AS single, " ALBUM_HERE " AS here FROM albums a)),"
  /* As mc_library_list_artists(). */
  " (SELECT count(*) FROM artists ar"
  "  WHERE EXISTS (SELECT 1 FROM albums a WHERE a.artist_uid = ar.uid)"
  "   OR EXISTS (SELECT 1 FROM tracks t WHERE t.artist_uid = ar.uid)"
  "   OR EXISTS (SELECT 1 FROM track_artists c WHERE c.artist_uid = ar.uid AND c.position > 0)"
  "   OR EXISTS (SELECT 1 FROM album_artists c WHERE c.artist_uid = ar.uid AND c.position > 0)),"
  /* As mc_library_list_artists_here(), asked of each artist so that the first thing of theirs
   * found here ends it, which makes this quick while most of the library is here. A track with
   * no artist of its own goes by its album's, and an album here covers those. */
  " (SELECT count(*) FROM artists ar"
  "  WHERE EXISTS (SELECT 1 FROM tracks t WHERE t.artist_uid = ar.uid AND " TRACK_HERE ")"
  "   OR EXISTS (SELECT 1 FROM albums a WHERE a.artist_uid = ar.uid AND " ALBUM_HERE ")"
  "   OR EXISTS (SELECT 1 FROM track_artists c JOIN tracks t ON t.uid = c.track_uid"
  "    WHERE c.artist_uid = ar.uid AND c.position > 0 AND " TRACK_HERE ")"
  "   OR EXISTS (SELECT 1 FROM album_artists c JOIN albums a ON a.uid = c.album_uid"
  "    WHERE c.artist_uid = ar.uid AND c.position > 0 AND " ALBUM_HERE ")),"
  " (SELECT count(*), sum(size_bytes) FROM files),"
  " (SELECT count(*) FROM playlists),"
  " (SELECT count(*) FROM album_groups),"
  " (SELECT sum(subject = 'tracks'), sum(subject = 'albums') FROM smart_lists),"
  " (SELECT sum(play_count), count(DISTINCT CASE WHEN play_count > 0 THEN track_uid END)"
  "  FROM track_plays),"
  " (SELECT sum(listened_ms) FROM play_events)";

mc_status mc_library_read_stats(mc_library *lib, mc_library_stats *out) {
  if (!lib || !out) return MC_ERR_INVALID_ARG;
  memset(out, 0, sizeof *out);
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(lib->db, k_stats_sql, -1, &stmt, NULL) != SQLITE_OK) return MC_ERR_DB;
  mc_status st = MC_ERR_DB;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    /* In the query's order. */
    int64_t *const columns[] = {
      &out->all.tracks, &out->here.tracks, &out->all.duration_ms, &out->here.duration_ms,
      &out->all.genres, &out->here.genres, &out->all.singles, &out->here.singles,
      &out->all.albums, &out->here.albums, NULL, NULL,
      &out->all.artists, &out->here.artists,
      &out->files, &out->size_bytes,
      &out->playlists, &out->album_groups, &out->smart_playlists, &out->smart_album_groups,
      &out->plays, &out->tracks_played, &out->listened_ms,
    };
    for (int i = 0; i < (int)(sizeof columns / sizeof *columns); i++)
      if (columns[i]) *columns[i] = sqlite3_column_int64(stmt, i);
    /* The albums of one track come on top of the tracks with no album. */
    out->all.singles += sqlite3_column_int64(stmt, 10);
    out->here.singles += sqlite3_column_int64(stmt, 11);
    st = MC_OK;
  }
  sqlite3_finalize(stmt);
  return st;
}

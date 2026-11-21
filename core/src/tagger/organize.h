/* Where imported files go inside the library folder:
 *
 *   <album artist>/<album> (<year>)/<track> <title>.<ext>      01 Airbag.flac
 *                                   <disc>-<track> <title>.<ext>  2-01 ... (multi-disc)
 *   <artist>/<title>.<ext>                                     a track with no album
 *
 * Returned paths are relative and '/'-separated (see mc_library_abspath). Each component is
 * made safe for Windows, which has the strictest rules of the platforms we run on, so a
 * library folder can move between them. All functions return malloc'd strings. */
#ifndef MC_TAGGER_ORGANIZE_H
#define MC_TAGGER_ORGANIZE_H

/* s made safe as one path component: no reserved characters, no leading dots, no trailing
 * dots or spaces, no device names, at most ~100 bytes. Empty results become fallback. */
char *mc_path_component(const char *s, const char *fallback);

/* "Radiohead": the folder of an artist's albums, and of their tracks with no album. */
char *mc_artist_dir(const char *artist);

/* "Radiohead/OK Computer (1997)"; the year is left out when 0. */
char *mc_album_dir(const char *album_artist, const char *album, int year);

/* "01 Airbag.flac", "2-01 Title.flac" when disc_total > 1, "Title.flac" when track <= 0. */
char *mc_track_file(const char *title, int disc, int disc_total, int track, const char *ext);

#endif

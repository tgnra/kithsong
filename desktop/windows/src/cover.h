/* Album covers: the cover in an album's folder (mc_coverart_find()), decoded with WIC. Playlists'
 * pictures (see mc_playlist_art()) are decoded the same way from their file. */
#ifndef KITHSONG_COVER_H
#define KITHSONG_COVER_H

#include <stdint.h>
#include <wincodec.h>
#include <windows.h>

/* Puts the path of dir's cover in path (n characters with the NUL): FALSE if there's none.
 * Lists the folder. */
BOOL cover_find(const wchar_t *dir, wchar_t *path, size_t n);
/* Decodes dir's cover to a size x size premultiplied, top-down DIB: scaled so the short side
 * fits, centre-cropped, and with rounded corners. Sets *pixels to its bits. Returns NULL if
 * there's no cover or it can't be decoded. Safe on any thread with COM initialised. */
HBITMAP cover_decode(IWICImagingFactory *wic, const wchar_t *dir, int size, int radius,
                     const uint32_t **pixels);
/* The same for the image file at path (a playlist's picture, say). */
HBITMAP cover_decode_file(IWICImagingFactory *wic, const wchar_t *path, int size, int radius,
                          const uint32_t **pixels);

/* A loader thread's own connection to the library, for playlists' and groups' pictures: making
 * one decodes four covers, which would hold up the UI thread's connection. Starts zeroed. */
typedef struct cover_art_source {
  struct mc_library *lib;
  char *dir;
} cover_art_source;

/* The picture file of playlist uid (see mc_playlist_art()), or for group of album group uid,
 * or of smart list uid (mc_smart_art()), or NULL if it has none. Opens the library in lib_dir
 * first, if it isn't the one open. */
wchar_t *cover_art_file(cover_art_source *src, const char *lib_dir, const char *art_dir, const wchar_t *uid,
                        BOOL group);
void cover_art_close(cover_art_source *src);

#endif

/* Album covers: the cover.jpg (or cover.png) in an album's folder, decoded on a worker
 * thread into square textures, and kept in a cache bounded by memory, so the album grid
 * scrolls smoothly however big the library is. The _file functions do the same for an
 * image file, such as a playlist's picture (mc_playlist_art()). Call everything on the UI
 * thread. */
#ifndef KITHSONG_COVER_H
#define KITHSONG_COVER_H

#include <gtk/gtk.h>

/* dir's cover image, or NULL if it has none. Free with g_free(). */
char *cover_path(const char *dir);

/* Looks for dir's cover at size x size in the cache. Returns TRUE if it's there, with
 * *texture set to a new reference, or to NULL if the cover couldn't be decoded. */
gboolean cover_lookup(const char *dir, int size, GdkTexture **texture);
gboolean cover_lookup_file(const char *path, int size, GdkTexture **texture);

/* Called on the UI thread with the cover (borrowed), or NULL if it couldn't be decoded. */
typedef void (*cover_done)(GdkTexture *texture, gpointer user);

/* Decodes dir's cover to size x size: scaled so the short side fits, centre-cropped. done
 * isn't called if cancellable is cancelled first, and the decoding is skipped if that
 * happens before it starts, so a fast scroll leaves no backlog. The result is cached
 * either way. */
void cover_load(const char *dir, int size, GCancellable *cancellable, cover_done done, gpointer user);
void cover_load_file(const char *path, int size, GCancellable *cancellable, cover_done done, gpointer user);

/* The cached cover if there is one, otherwise starts cover_load() and returns NULL. For
 * one-off images (the track list header, the now-playing strip). */
GdkTexture *cover_get(const char *dir, int size, GCancellable *cancellable, cover_done done,
                      gpointer user);
GdkTexture *cover_get_file(const char *path, int size, GCancellable *cancellable, cover_done done,
                           gpointer user);

void cover_cache_free(void);

/* A cover in a rounded square, size x size, with a music note behind it that shows until
 * (or unless) there is art. Sets *picture to the GtkPicture to give the cover to. */
GtkWidget *cover_frame_new(int size, int icon_size, GtkWidget **picture);

#endif

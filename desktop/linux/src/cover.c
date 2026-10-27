#include "cover.h"

#include <glib/gstdio.h>
#include <math.h>

/* Decoded covers kept for scrolling back: about three screens of a big window at 2x. */
enum { CACHE_BYTES = 96 * 1024 * 1024 };

typedef struct entry {
  char *key; /* "<size>:<path>" */
  GdkTexture *texture; /* NULL if decoding failed */
  gint64 mtime;        /* of the file when decoded; a newer one is decoded again */
  size_t bytes;
  GList *link; /* in s.lru, most recently used first */
} entry;

typedef struct job {
  char *path;
  char *key;
  int size;
  gint64 mtime;
  cover_done done;
  gpointer user;
} job;

static struct {
  GHashTable *cache; /* key -> entry */
  GQueue lru;
  size_t bytes;
} s;

char *cover_path(const char *dir) {
  static const char *const names[] = {"cover.jpg", "cover.png"};
  if (!dir) return NULL;
  for (size_t i = 0; i < G_N_ELEMENTS(names); i++) {
    char *path = g_build_filename(dir, names[i], NULL);
    if (g_file_test(path, G_FILE_TEST_IS_REGULAR)) return path;
    g_free(path);
  }
  return NULL;
}

static gint64 file_mtime(const char *path) {
  GStatBuf st;
  return g_stat(path, &st) == 0 ? (gint64)st.st_mtime : -1;
}

static void entry_free(gpointer data) {
  entry *e = data;
  g_clear_object(&e->texture);
  g_free(e->key);
  g_free(e);
}

static void ensure_cache(void) {
  if (!s.cache) s.cache = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, entry_free);
}

static void remove_entry(entry *e) {
  g_queue_delete_link(&s.lru, e->link);
  s.bytes -= e->bytes;
  g_hash_table_remove(s.cache, e->key); /* frees e */
}

static void cache_put(char *key, GdkTexture *texture, gint64 mtime) {
  ensure_cache();
  entry *old = g_hash_table_lookup(s.cache, key);
  if (old) remove_entry(old);

  entry *e = g_new0(entry, 1);
  e->key = key;
  e->texture = texture ? g_object_ref(texture) : NULL;
  e->mtime = mtime;
  e->bytes = texture ? (size_t)gdk_texture_get_width(texture) * (size_t)gdk_texture_get_height(texture) * 4 : 0;
  g_queue_push_head(&s.lru, e);
  e->link = s.lru.head;
  s.bytes += e->bytes;
  g_hash_table_insert(s.cache, e->key, e);

  while (s.bytes > CACHE_BYTES && s.lru.tail && s.lru.tail->data != e) remove_entry(s.lru.tail->data);
}

static char *cache_key(const char *path, int size) {
  return g_strdup_printf("%d:%s", size, path);
}

gboolean cover_lookup(const char *dir, int size, GdkTexture **texture) {
  *texture = NULL;
  if (!s.cache) return FALSE;
  char *path = cover_path(dir);
  gboolean found = cover_lookup_file(path, size, texture);
  g_free(path);
  return found;
}

gboolean cover_lookup_file(const char *path, int size, GdkTexture **texture) {
  *texture = NULL;
  if (!s.cache || !path) return FALSE;
  char *key = cache_key(path, size);
  entry *e = g_hash_table_lookup(s.cache, key);
  gboolean found = e && e->mtime == file_mtime(path);
  if (found) {
    g_queue_unlink(&s.lru, e->link);
    g_queue_push_head_link(&s.lru, e->link);
    *texture = e->texture ? g_object_ref(e->texture) : NULL;
  }
  g_free(key);
  return found;
}

/* --- Decoding (worker thread) --- */

/* gdk-pixbuf's JPEG loader does most of the downscaling while decoding, which is what makes
 * this fast. */
static GdkTexture *decode(const char *path, int size) {
  int w = 0, h = 0;
  if (!gdk_pixbuf_get_file_info(path, &w, &h) || w <= 0 || h <= 0 || size <= 0) return NULL;
  double scale = (double)size / (double)MIN(w, h);
  int sw = MAX(size, (int)floor(w * scale + 0.5)), sh = MAX(size, (int)floor(h * scale + 0.5));
  GdkPixbuf *scaled = gdk_pixbuf_new_from_file_at_scale(path, sw, sh, FALSE, NULL);
  if (!scaled) return NULL;
  /* The loader may round differently; crop whatever came out. */
  sw = gdk_pixbuf_get_width(scaled);
  sh = gdk_pixbuf_get_height(scaled);
  int side = MIN(MIN(sw, sh), size);
  GdkPixbuf *square = gdk_pixbuf_new(GDK_COLORSPACE_RGB, gdk_pixbuf_get_has_alpha(scaled), 8, side, side);
  GdkTexture *texture = NULL;
  if (square) {
    gdk_pixbuf_copy_area(scaled, (sw - side) / 2, (sh - side) / 2, side, side, square, 0, 0);
    GBytes *bytes = gdk_pixbuf_read_pixel_bytes(square);
    texture = gdk_memory_texture_new(side, side,
                                     gdk_pixbuf_get_has_alpha(square) ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8,
                                     bytes, (gsize)gdk_pixbuf_get_rowstride(square));
    g_bytes_unref(bytes);
    g_object_unref(square);
  }
  g_object_unref(scaled);
  return texture;
}

static void job_free(gpointer data) {
  job *j = data;
  g_free(j->path);
  g_free(j->key);
  g_free(j);
}

static void decode_thread(GTask *task, gpointer source, gpointer data, GCancellable *cancellable) {
  (void)source;
  job *j = data;
  if (g_cancellable_is_cancelled(cancellable)) {
    g_task_return_pointer(task, NULL, NULL); /* never started: nothing to cache */
    return;
  }
  GdkTexture *texture = decode(j->path, j->size);
  g_task_return_pointer(task, texture ? texture : (gpointer)g_object_new(G_TYPE_OBJECT, NULL), g_object_unref);
}

/* On the UI thread. A plain GObject stands for "decoded, but failed". */
static void decode_done(GObject *source, GAsyncResult *result, gpointer user) {
  (void)source;
  (void)user;
  GTask *task = G_TASK(result);
  job *j = g_task_get_task_data(task);
  GObject *obj = g_task_propagate_pointer(task, NULL);
  if (!obj) return; /* skipped */
  GdkTexture *texture = GDK_IS_TEXTURE(obj) ? GDK_TEXTURE(obj) : NULL;
  cache_put(g_strdup(j->key), texture, j->mtime);
  GCancellable *cancellable = g_task_get_cancellable(task);
  if (j->done && !g_cancellable_is_cancelled(cancellable)) j->done(texture, j->user);
  g_object_unref(obj);
}

void cover_load(const char *dir, int size, GCancellable *cancellable, cover_done done, gpointer user) {
  char *path = cover_path(dir);
  cover_load_file(path, size, cancellable, done, user);
  g_free(path);
}

void cover_load_file(const char *path, int size, GCancellable *cancellable, cover_done done, gpointer user) {
  if (!path) {
    if (done) done(NULL, user);
    return;
  }
  job *j = g_new0(job, 1);
  j->path = g_strdup(path);
  j->key = cache_key(path, size);
  j->size = size;
  j->mtime = file_mtime(path);
  j->done = done;
  j->user = user;
  GTask *task = g_task_new(NULL, cancellable, decode_done, NULL);
  g_task_set_check_cancellable(task, FALSE); /* a finished decode is cached even if unwanted */
  g_task_set_task_data(task, j, job_free);
  g_task_run_in_thread(task, decode_thread);
  g_object_unref(task);
}

GdkTexture *cover_get(const char *dir, int size, GCancellable *cancellable, cover_done done,
                      gpointer user) {
  GdkTexture *texture = NULL;
  if (cover_lookup(dir, size, &texture)) return texture;
  cover_load(dir, size, cancellable, done, user);
  return NULL;
}

GdkTexture *cover_get_file(const char *path, int size, GCancellable *cancellable, cover_done done,
                           gpointer user) {
  GdkTexture *texture = NULL;
  if (cover_lookup_file(path, size, &texture)) return texture;
  cover_load_file(path, size, cancellable, done, user);
  return NULL;
}

void cover_cache_free(void) {
  if (!s.cache) return;
  g_queue_clear(&s.lru);
  g_hash_table_destroy(s.cache);
  s.cache = NULL;
  s.bytes = 0;
}

GtkWidget *cover_frame_new(int size, int icon_size, GtkWidget **picture) {
  GtkWidget *frame = gtk_overlay_new();
  gtk_widget_add_css_class(frame, "album-cover");
  gtk_widget_set_overflow(frame, GTK_OVERFLOW_HIDDEN);
  gtk_widget_set_size_request(frame, size, size);
  gtk_widget_set_halign(frame, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(frame, GTK_ALIGN_CENTER);
  GtkWidget *note = gtk_image_new_from_icon_name("folder-music-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(note), icon_size);
  gtk_widget_add_css_class(note, "dim-label");
  gtk_overlay_set_child(GTK_OVERLAY(frame), note);
  *picture = gtk_picture_new();
  gtk_picture_set_content_fit(GTK_PICTURE(*picture), GTK_CONTENT_FIT_COVER);
  gtk_picture_set_can_shrink(GTK_PICTURE(*picture), TRUE); /* overlays don't size the frame */
  gtk_overlay_add_overlay(GTK_OVERLAY(frame), *picture);
  return frame;
}

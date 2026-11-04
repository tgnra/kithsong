#include "art_backdrop.h"

#include <adwaita.h>
#include <string.h>

#include "cover.h"

enum {
  PX = 128,         /* the art is looked at this big: about what Android scales a wallpaper to */
  CACHE_SIZE = 512, /* pieces of art remembered: a few screens of albums */
  FADE_MS = 700,    /* from one set of colours to the next */
  ARRIVE_MS = 250,  /* the first colours, from clear, when they come after SNAP_MS */
  SNAP_MS = 150,    /* colours this soon after the backdrop is shown are taken at once */
};

/* --- Colours, found off the UI thread and remembered --- */

typedef struct found {
  char *path;
  gint64 mtime; /* of the file when looked at; a newer one is looked at again */
  mc_art_color colors[MC_ART_COLORS_MAX];
  int count;
  GList *link; /* in lru, most recently used first */
} found;

static GHashTable *cache; /* path -> found */
static GQueue lru;

static void found_free(gpointer data) {
  found *f = data;
  g_free(f->path);
  g_free(f);
}

static void cache_put(const char *path, gint64 mtime, const mc_art_color *colors, int count) {
  if (!cache) cache = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, found_free);
  found *old = g_hash_table_lookup(cache, path);
  if (old) {
    g_queue_delete_link(&lru, old->link);
    g_hash_table_remove(cache, path);
  }
  found *f = g_new0(found, 1);
  f->path = g_strdup(path);
  f->mtime = mtime;
  f->count = count;
  memcpy(f->colors, colors, sizeof(mc_art_color) * (size_t)count);
  g_queue_push_head(&lru, f);
  f->link = lru.head;
  g_hash_table_insert(cache, f->path, f);
  while (lru.length > CACHE_SIZE) {
    found *last = g_queue_pop_tail(&lru);
    g_hash_table_remove(cache, last->path);
  }
}

gboolean art_colors_cached(const char *path, mc_art_color out[MC_ART_COLORS_MAX], int *count) {
  *count = 0;
  found *f = cache && path ? g_hash_table_lookup(cache, path) : NULL;
  if (!f || f->mtime != cover_mtime(path)) return FALSE;
  g_queue_unlink(&lru, f->link);
  g_queue_push_head_link(&lru, f->link);
  memcpy(out, f->colors, sizeof(mc_art_color) * (size_t)f->count);
  *count = f->count;
  return TRUE;
}

void art_colors_cache_free(void) {
  if (!cache) return;
  g_queue_clear(&lru);
  g_clear_pointer(&cache, g_hash_table_destroy);
}

typedef struct job {
  char *path;        /* NULL for a texture's pixels */
  GBytes *pixels;    /* a texture's, R8G8B8A8 */
  int width, height; /* of pixels */
  gsize stride;
  gint64 mtime;
  art_colors_done done;
  gpointer user;
  mc_art_color colors[MC_ART_COLORS_MAX];
  int count;
} job;

static void job_free(gpointer data) {
  job *j = data;
  g_free(j->path);
  g_clear_pointer(&j->pixels, g_bytes_unref);
  g_free(j);
}

/* width x height RGBA pixels, every step-th of src's, so a big image costs no more than PX. */
static int colors_of(const guint8 *src, int width, int height, gsize stride, int channels, mc_art_color *out) {
  int step = MAX(1, MAX(width, height) / PX);
  int w = width / step, h = height / step;
  if (w <= 0 || h <= 0) return 0;
  guint8 *rgba = g_malloc((gsize)w * (gsize)h * 4);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      const guint8 *p = src + (gsize)(y * step) * stride + (gsize)(x * step) * (gsize)channels;
      guint8 *q = rgba + ((gsize)y * (gsize)w + (gsize)x) * 4;
      q[0] = p[0];
      q[1] = p[1];
      q[2] = p[2];
      q[3] = channels == 4 ? p[3] : 255;
    }
  int count = mc_art_colors(rgba, w, h, (size_t)w * 4, out);
  g_free(rgba);
  return count;
}

static void find_thread(GTask *task, gpointer source, gpointer data, GCancellable *cancellable) {
  (void)source;
  job *j = data;
  if (g_cancellable_is_cancelled(cancellable)) {
    g_task_return_boolean(task, FALSE); /* never started: nothing to remember */
    return;
  }
  if (j->pixels) {
    j->count = colors_of(g_bytes_get_data(j->pixels, NULL), j->width, j->height, j->stride, 4, j->colors);
  } else {
    /* gdk-pixbuf's JPEG loader scales while decoding, so this is quick even for big art. */
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file_at_scale(j->path, PX, PX, TRUE, NULL);
    if (pixbuf && gdk_pixbuf_get_bits_per_sample(pixbuf) == 8 && gdk_pixbuf_get_n_channels(pixbuf) >= 3)
      j->count = colors_of(gdk_pixbuf_read_pixels(pixbuf), gdk_pixbuf_get_width(pixbuf), gdk_pixbuf_get_height(pixbuf),
                           (gsize)gdk_pixbuf_get_rowstride(pixbuf), gdk_pixbuf_get_n_channels(pixbuf), j->colors);
    g_clear_object(&pixbuf);
  }
  g_task_return_boolean(task, TRUE);
}

/* On the UI thread. */
static void find_done(GObject *source, GAsyncResult *result, gpointer user) {
  (void)source;
  (void)user;
  GTask *task = G_TASK(result);
  job *j = g_task_get_task_data(task);
  if (!g_task_propagate_boolean(task, NULL)) return; /* skipped */
  if (j->path) cache_put(j->path, j->mtime, j->colors, j->count);
  if (j->done && !g_cancellable_is_cancelled(g_task_get_cancellable(task))) j->done(j->colors, j->count, j->user);
}

static void run(job *j, GCancellable *cancellable) {
  GTask *task = g_task_new(NULL, cancellable, find_done, NULL);
  g_task_set_check_cancellable(task, FALSE); /* colours found are remembered even if unwanted */
  g_task_set_task_data(task, j, job_free);
  g_task_run_in_thread(task, find_thread);
  g_object_unref(task);
}

void art_colors_find(const char *path, GCancellable *cancellable, art_colors_done done, gpointer user) {
  mc_art_color colors[MC_ART_COLORS_MAX];
  int count = 0;
  if (!path || art_colors_cached(path, colors, &count)) {
    if (done) done(colors, count, user);
    return;
  }
  job *j = g_new0(job, 1);
  j->path = g_strdup(path);
  j->mtime = cover_mtime(path);
  j->done = done;
  j->user = user;
  run(j, cancellable);
}

void art_colors_find_texture(GdkTexture *texture, GCancellable *cancellable, art_colors_done done, gpointer user) {
  if (!texture) {
    if (done) done(NULL, 0, user);
    return;
  }
  job *j = g_new0(job, 1);
  GdkTextureDownloader *down = gdk_texture_downloader_new(texture);
  gdk_texture_downloader_set_format(down, GDK_MEMORY_R8G8B8A8);
  j->pixels = gdk_texture_downloader_download_bytes(down, &j->stride);
  gdk_texture_downloader_free(down);
  j->width = gdk_texture_get_width(texture);
  j->height = gdk_texture_get_height(texture);
  j->done = done;
  j->user = user;
  run(j, cancellable);
}

/* --- The backdrop --- */

/* Where a source's colours go when they're found. */
typedef struct arrival {
  ArtBackdrop *self;
  int index;
} arrival;

struct _ArtBackdrop {
  GtkWidget parent;
  GtkWidget *child;
  double fade_bottom;

  /* The art's colours, one source after another (several for set_files), as found. */
  char **paths;
  gint64 *mtimes; /* of paths, as a cover replaced keeps its path */
  int npaths;
  mc_art_color (*colors)[MC_ART_COLORS_MAX];
  int *counts;
  arrival *arrivals;      /* one per source, for art_colors_find()'s callback */
  GCancellable *loading; /* cancelled before those go, so no callback outlives them */

  /* Glows: shown is from + (to - from) * progress. A clear glow has alpha 0. */
  GdkRGBA from[MC_ART_GLOWS], to[MC_ART_GLOWS];
  double progress;
  AdwAnimation *animation;
  gboolean lit;     /* has had colours since it was shown, so new ones change rather than arrive */
  gint64 shown_at;  /* g_get_monotonic_time() when mapped; 0 while not */
};

G_DEFINE_FINAL_TYPE(ArtBackdrop, art_backdrop, GTK_TYPE_WIDGET)

static GdkRGBA rgba_of(uint32_t argb) {
  GdkRGBA c = {(float)((argb >> 16) & 255) / 255.0f, (float)((argb >> 8) & 255) / 255.0f, (float)(argb & 255) / 255.0f,
               (float)(argb >> 24) / 255.0f};
  return c;
}

static GdkRGBA mix(const GdkRGBA *a, const GdkRGBA *b, double t) {
  GdkRGBA c = {(float)(a->red + (b->red - a->red) * t), (float)(a->green + (b->green - a->green) * t),
               (float)(a->blue + (b->blue - a->blue) * t), (float)(a->alpha + (b->alpha - a->alpha) * t)};
  return c;
}

static void on_progress(double value, gpointer user) {
  ArtBackdrop *self = user;
  self->progress = value;
  gtk_widget_queue_draw(GTK_WIDGET(self));
}

/* Moves to the glows of the colours found so far: at once while the backdrop is out of
 * sight or only just shown, otherwise in a fade, a quick one if it was clear. */
static void retarget(ArtBackdrop *self, gboolean at_once) {
  int total = 0;
  for (int i = 0; i < self->npaths; i++) total += self->counts[i];
  mc_art_color *all = g_new(mc_art_color, MAX(total, 1));
  for (int i = 0, n = 0; i < self->npaths; i++) {
    memcpy(all + n, self->colors[i], sizeof(mc_art_color) * (size_t)self->counts[i]);
    n += self->counts[i];
  }
  uint32_t glows[MC_ART_GLOWS];
  gboolean dark = adw_style_manager_get_dark(adw_style_manager_get_default());
  int lit = mc_art_glows(all, total, dark, glows) == MC_ART_GLOWS;
  g_free(all);

  for (int i = 0; i < MC_ART_GLOWS; i++) {
    GdkRGBA shown = mix(&self->from[i], &self->to[i], self->progress);
    GdkRGBA target = lit ? rgba_of(glows[i]) : shown;
    if (!lit) target.alpha = 0;
    /* A glow that comes or goes keeps its colour while its alpha changes. */
    if (shown.alpha == 0) {
      shown = target;
      shown.alpha = 0;
    }
    self->from[i] = shown;
    self->to[i] = target;
  }
  self->progress = 0;
  adw_animation_reset(self->animation);
  gint64 since = self->shown_at ? (g_get_monotonic_time() - self->shown_at) / 1000 : 0;
  if (at_once || !self->shown_at || since < SNAP_MS) {
    self->progress = 1;
  } else {
    adw_timed_animation_set_duration(ADW_TIMED_ANIMATION(self->animation), self->lit ? FADE_MS : ARRIVE_MS);
    adw_animation_play(self->animation);
  }
  if (lit && self->shown_at) self->lit = TRUE;
  gtk_widget_queue_draw(GTK_WIDGET(self));
}

static void clear_sources(ArtBackdrop *self) {
  if (self->loading) g_cancellable_cancel(self->loading);
  g_clear_object(&self->loading);
  g_strfreev(self->paths);
  self->paths = NULL;
  g_clear_pointer(&self->mtimes, g_free);
  g_clear_pointer(&self->colors, g_free);
  g_clear_pointer(&self->counts, g_free);
  g_clear_pointer(&self->arrivals, g_free);
  self->npaths = 0;
}

/* count sources, colours to come. */
static void start_sources(ArtBackdrop *self, int count) {
  clear_sources(self);
  self->npaths = count;
  self->colors = g_malloc0(sizeof *self->colors * (gsize)MAX(count, 1));
  self->counts = g_new0(int, MAX(count, 1));
  self->arrivals = g_new0(arrival, MAX(count, 1));
  for (int i = 0; i < count; i++) self->arrivals[i] = (arrival){self, i};
  self->loading = g_cancellable_new();
}

static void on_found(const mc_art_color *colors, int count, gpointer user) {
  arrival *a = user;
  ArtBackdrop *self = a->self;
  memcpy(self->colors[a->index], colors, sizeof(mc_art_color) * (size_t)count);
  self->counts[a->index] = count;
  retarget(self, FALSE);
}

/* A source's colours: at once if they're remembered, else when found. */
static void find_source(ArtBackdrop *self, int index, const char *path) {
  if (art_colors_cached(path, self->colors[index], &self->counts[index])) return;
  art_colors_find(path, self->loading, on_found, &self->arrivals[index]);
}

void art_backdrop_set_files(ArtBackdrop *self, const char *const *paths, int count) {
  g_return_if_fail(ART_IS_BACKDROP(self));
  if (count < 0) count = 0;
  /* The same art again (a reload): nothing changes. */
  if (self->npaths == count && self->paths) {
    gboolean same = TRUE;
    for (int i = 0; i < count && same; i++)
      same = !g_strcmp0(self->paths[i], paths[i]) && self->mtimes[i] == cover_mtime(paths[i]);
    if (same) return;
  }
  start_sources(self, count);
  self->paths = g_new0(char *, (gsize)count + 1);
  self->mtimes = g_new0(gint64, (gsize)count + 1);
  for (int i = 0; i < count; i++) {
    self->paths[i] = g_strdup(paths[i] ? paths[i] : "");
    self->mtimes[i] = cover_mtime(paths[i]);
  }
  for (int i = 0; i < count; i++)
    if (paths[i] && *paths[i]) find_source(self, i, paths[i]);
  retarget(self, FALSE);
}

void art_backdrop_set_file(ArtBackdrop *self, const char *path) {
  art_backdrop_set_files(self, &path, path ? 1 : 0);
}

void art_backdrop_set_dir(ArtBackdrop *self, const char *cover_dir) {
  char *path = cover_path(cover_dir);
  art_backdrop_set_file(self, path);
  g_free(path);
}

void art_backdrop_set_texture(ArtBackdrop *self, GdkTexture *texture) {
  g_return_if_fail(ART_IS_BACKDROP(self));
  start_sources(self, texture ? 1 : 0);
  if (texture) art_colors_find_texture(texture, self->loading, on_found, &self->arrivals[0]);
  retarget(self, FALSE);
}

void art_backdrop_set_colors(ArtBackdrop *self, const mc_art_color *colors, int count) {
  g_return_if_fail(ART_IS_BACKDROP(self));
  count = CLAMP(count, 0, MC_ART_COLORS_MAX);
  start_sources(self, 1);
  if (count) memcpy(self->colors[0], colors, sizeof(mc_art_color) * (size_t)count);
  self->counts[0] = count;
  retarget(self, FALSE);
}

void art_backdrop_set_child(ArtBackdrop *self, GtkWidget *child) {
  g_return_if_fail(ART_IS_BACKDROP(self));
  if (self->child == child) return;
  g_clear_pointer(&self->child, gtk_widget_unparent);
  self->child = child;
  if (child) gtk_widget_set_parent(child, GTK_WIDGET(self));
}

void art_backdrop_set_fade_bottom(ArtBackdrop *self, double share) {
  g_return_if_fail(ART_IS_BACKDROP(self));
  self->fade_bottom = CLAMP(share, 0.0, 1.0);
  gtk_widget_queue_draw(GTK_WIDGET(self));
}

static void art_backdrop_snapshot(GtkWidget *widget, GtkSnapshot *snapshot) {
  ArtBackdrop *self = ART_BACKDROP(widget);
  float w = (float)gtk_widget_get_width(widget), h = (float)gtk_widget_get_height(widget);
  gboolean any = FALSE;
  GdkRGBA shown[MC_ART_GLOWS];
  for (int i = 0; i < MC_ART_GLOWS; i++) {
    shown[i] = mix(&self->from[i], &self->to[i], self->progress);
    any |= shown[i].alpha > 0.001f;
  }
  if (any && w > 0 && h > 0 && !adw_style_manager_get_high_contrast(adw_style_manager_get_default())) {
    graphene_rect_t bounds = GRAPHENE_RECT_INIT(0, 0, w, h);
    if (self->fade_bottom > 0) {
      gtk_snapshot_push_mask(snapshot, GSK_MASK_MODE_ALPHA);
      GskColorStop mask[2] = {{(float)(1.0 - self->fade_bottom), {0, 0, 0, 1}}, {1.0f, {0, 0, 0, 0}}};
      gtk_snapshot_append_linear_gradient(snapshot, &bounds, &GRAPHENE_POINT_INIT(0, 0), &GRAPHENE_POINT_INIT(0, h),
                                          mask, 2);
      gtk_snapshot_pop(snapshot);
    }
    float radius = MAX(w, h) * 0.75f;
    for (int i = 0; i < MC_ART_GLOWS; i++) {
      if (shown[i].alpha <= 0.001f) continue;
      float cx, cy;
      mc_art_glow_center(i, &cx, &cy);
      GdkRGBA clear = shown[i];
      clear.alpha = 0;
      GskColorStop stops[2] = {{0.0f, shown[i]}, {1.0f, clear}};
      gtk_snapshot_append_radial_gradient(snapshot, &bounds, &GRAPHENE_POINT_INIT(cx * w, cy * h), radius, radius, 0.0f,
                                          1.0f, stops, 2);
    }
    if (self->fade_bottom > 0) gtk_snapshot_pop(snapshot);
  }
  if (self->child) gtk_widget_snapshot_child(widget, self->child, snapshot);
}

static void art_backdrop_map(GtkWidget *widget) {
  ArtBackdrop *self = ART_BACKDROP(widget);
  GTK_WIDGET_CLASS(art_backdrop_parent_class)->map(widget);
  self->shown_at = g_get_monotonic_time();
}

/* Out of sight it starts afresh: shown again, its colours are taken at once. */
static void art_backdrop_unmap(GtkWidget *widget) {
  ArtBackdrop *self = ART_BACKDROP(widget);
  self->shown_at = 0;
  self->lit = FALSE;
  adw_animation_skip(self->animation);
  GTK_WIDGET_CLASS(art_backdrop_parent_class)->unmap(widget);
}

static void on_style_changed(ArtBackdrop *self) {
  retarget(self, TRUE);
}

static void art_backdrop_dispose(GObject *object) {
  ArtBackdrop *self = ART_BACKDROP(object);
  clear_sources(self);
  g_clear_object(&self->animation);
  g_clear_pointer(&self->child, gtk_widget_unparent);
  G_OBJECT_CLASS(art_backdrop_parent_class)->dispose(object);
}

static void art_backdrop_class_init(ArtBackdropClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  object_class->dispose = art_backdrop_dispose;
  widget_class->snapshot = art_backdrop_snapshot;
  widget_class->map = art_backdrop_map;
  widget_class->unmap = art_backdrop_unmap;
  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
}

static void art_backdrop_init(ArtBackdrop *self) {
  self->progress = 1;
  AdwAnimationTarget *target = adw_callback_animation_target_new(on_progress, self, NULL);
  self->animation = adw_timed_animation_new(GTK_WIDGET(self), 0, 1, FADE_MS, target);
  adw_timed_animation_set_easing(ADW_TIMED_ANIMATION(self->animation), ADW_EASE_IN_OUT_CUBIC);
  AdwStyleManager *style = adw_style_manager_get_default();
  g_signal_connect_object(style, "notify::dark", G_CALLBACK(on_style_changed), self, G_CONNECT_SWAPPED);
  g_signal_connect_object(style, "notify::high-contrast", G_CALLBACK(gtk_widget_queue_draw), self, G_CONNECT_SWAPPED);
}

GtkWidget *art_backdrop_new(void) {
  return g_object_new(ART_TYPE_BACKDROP, NULL);
}

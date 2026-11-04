#include "perf_ui.h"

#include <stdlib.h>

#include "album_grid.h"
#include "artist_view.h"
#include "track_list.h"

enum { MAX_WATCHED = 8, RUN_DELAY_MS = 2000, DIALOG_CLOSE_MS = 500 };

typedef struct watched {
  const char *series;
  gboolean moved; /* since the last frame painted */
} watched;

/* The scroll test's views, in order. */
enum { TEST_ALBUMS, TEST_ARTISTS, TEST_TRACKS, TEST_VIEWS };

static struct {
  app *a;
  watched lists[MAX_WATCHED];
  int nlists;
  gboolean first_frame, content_ready;
  guint tick;
  int view, frame;
  GtkAdjustment *adj; /* what the test is scrolling */
  gboolean settling;  /* the view just opened: its first frames only lay it out */
} p;

static void on_moved(GtkAdjustment *adj, gpointer data) {
  (void)adj;
  ((watched *)data)->moved = TRUE;
}

void perf_ui_watch(GtkWidget *scrolled, const char *series) {
  if (p.nlists == MAX_WATCHED) return;
  watched *w = &p.lists[p.nlists++];
  w->series = series;
  g_signal_connect(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scrolled)), "value-changed",
                   G_CALLBACK(on_moved), w);
}

static void on_after_paint(GdkFrameClock *clock, gpointer data) {
  (void)clock;
  (void)data;
  if (!mc_perf_enabled()) return;
  if (!p.first_frame) {
    p.first_frame = TRUE;
    mc_perf_mark("first_frame_painted");
  }
  if (p.content_ready) {
    p.content_ready = FALSE;
    mc_perf_mark("first_content_painted");
  }
  for (int i = 0; i < p.nlists; i++)
    if (p.lists[i].moved) {
      p.lists[i].moved = FALSE;
      mc_perf_frame(p.lists[i].series, 0);
    }
  mc_perf_view_end();
}

static void on_realize(GtkWidget *win, gpointer data) {
  (void)data;
  GdkFrameClock *clock = gtk_widget_get_frame_clock(win);
  if (clock) g_signal_connect(clock, "after-paint", G_CALLBACK(on_after_paint), NULL);
  if (!mc_perf_enabled()) return;
  GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(win));
  GdkMonitor *monitor = surface ? gdk_display_get_monitor_at_surface(gdk_surface_get_display(surface), surface) : NULL;
  int mhz = monitor ? gdk_monitor_get_refresh_rate(monitor) : 0;
  if (mhz > 0) mc_perf_set_refresh_hz(mhz / 1000.0);
}

static gboolean start_run(gpointer data) {
  (void)data;
  perf_ui_run_scroll_test();
  return G_SOURCE_REMOVE;
}

void perf_ui_init(app *a) {
  p.a = a;
  GtkWidget *win = GTK_WIDGET(a->win);
  if (gtk_widget_get_realized(win)) on_realize(win, NULL);
  else g_signal_connect(win, "realize", G_CALLBACK(on_realize), NULL);
  const char *run = g_getenv("KITHSONG_PERF_RUN");
  if (run && *run) g_timeout_add(RUN_DELAY_MS, start_run, NULL);
}

void perf_ui_view(const char *view) {
  if (!mc_perf_enabled()) return;
  mc_perf_view_begin(view);
  if (p.a) gtk_widget_queue_draw(GTK_WIDGET(p.a->win)); /* so there's a frame to end it */
}

void perf_ui_content_ready(void) {
  if (!mc_perf_enabled()) return;
  mc_perf_mark("albums_listed");
  p.content_ready = TRUE;
}

/* --- The scroll test --- */

static GtkAdjustment *adjustment_of(GtkWidget *scrolled) {
  return scrolled ? gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scrolled)) : NULL;
}

/* Opens the view the test has got to. */
static void enter_view(void) {
  app *a = p.a;
  p.frame = 0;
  p.settling = TRUE;
  if (p.view == TEST_ALBUMS) {
    app_show_view(a, APP_VIEW_ALBUMS);
    p.adj = adjustment_of(album_grid_scrolled(a->album_grid));
  } else if (p.view == TEST_ARTISTS) {
    app_show_view(a, APP_VIEW_ARTISTS);
    p.adj = adjustment_of(artist_view_scrolled());
  } else {
    char uid[MC_UID_STRLEN];
    app_show_view(a, APP_VIEW_ALBUMS);
    if (a->lib && mc_library_biggest_album(a->lib, uid) == MC_OK) app_open(a, FALSE, uid);
    p.adj = adjustment_of(track_list_scrolled());
  }
  if (p.adj) gtk_adjustment_set_value(p.adj, 0);
}

static gboolean show_settings(gpointer data) {
  (void)data;
  app_show_testing(p.a);
  return G_SOURCE_REMOVE;
}

static void test_ended(void) {
  mc_perf_auto_end();
  p.tick = 0;
  const char *out = g_getenv("KITHSONG_PERF_RUN");
  if (out && *out) {
    mc_perf_save(out);
    g_application_quit(G_APPLICATION(p.a->gapp));
    return;
  }
  /* A test on an empty library is over at once, and a dialog presented while Settings is still
   * closing doesn't show. */
  g_timeout_add(DIALOG_CLOSE_MS, show_settings, NULL);
}

static gboolean on_tick(GtkWidget *widget, GdkFrameClock *clock, gpointer data) {
  (void)widget;
  (void)clock;
  (void)data;
  if (p.settling) { /* one frame for what opened to be laid out */
    p.settling = FALSE;
    for (int i = 0; i < p.nlists; i++) p.lists[i].moved = FALSE;
    return G_SOURCE_CONTINUE;
  }
  double max = p.adj ? gtk_adjustment_get_upper(p.adj) - gtk_adjustment_get_page_size(p.adj) : 0;
  double value = p.adj ? gtk_adjustment_get_value(p.adj) : 0;
  if (p.frame >= MC_PERF_SCROLL_FRAMES || value >= max) {
    static const char *const names[TEST_VIEWS] = {"albums", "artists", "tracks"};
    mc_perf_frame_break(names[p.view]);
    if (++p.view == TEST_VIEWS) {
      test_ended();
      return G_SOURCE_REMOVE;
    }
    enter_view();
    return G_SOURCE_CONTINUE;
  }
  gtk_adjustment_set_value(p.adj, value + MC_PERF_SCROLL_DP_PER_FRAME);
  p.frame++;
  return G_SOURCE_CONTINUE;
}

void perf_ui_run_scroll_test(void) {
  if (!mc_perf_enabled() || p.tick || !p.a || !p.a->lib) return;
  mc_perf_auto_begin();
  p.view = TEST_ALBUMS;
  enter_view();
  p.tick = gtk_widget_add_tick_callback(GTK_WIDGET(p.a->win), on_tick, NULL, NULL);
}

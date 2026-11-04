#include "home_view.h"

#include <math.h>
#include <string.h>

#include "brand_backdrop.h"
#include "cover.h"
#include "musiccore/home_picks.h"

enum {
  COVER_SIZE = 160, /* the album grid's tiles */
  PAD = 20,         /* the page's edge, to the covers */
  TILE_PAD = 8,     /* around a cover: .home-tile's padding, which its hover fills */
  TILE_GAP = 4,     /* with the padding, 20 between covers */
  TITLE_GAP = 6,    /* with the padding, 14 under a row's title */
  ROW_GAP = 22,     /* with the padding, 30 between rows */
  ARROW = 36,       /* a row's arrows, round */
  ARROW_INSET = 16,
  BADGE_INSET = 8,  /* a station's mark, on its cover's corner */
  BADGE_ICON = 18,
  PITCH = COVER_SIZE + 2 * TILE_PAD + TILE_GAP,
  EASE_MS = 250,
  FLING_MS = 220,   /* how far a let-go drag carries on: the speed it had, for this long */
  STILL_MS = 80,    /* held still this long before letting go, it stays put */
};

struct row;

typedef struct tile {
  struct row *row;
  int index;
  GtkWidget *button, *frame, *picture, *title, *sub, *badge;
  mc_recent_kind kind;
  gboolean station;
  char uid[MC_UID_STRLEN];
  char *artist;  /* the line under its title */
  char *cover;   /* the file showing, or on its way; NULL for none */
  gint64 cover_mtime;
  GCancellable *loading;
} tile;

typedef struct row {
  GtkWidget *box, *scroller, *tiles, *left, *right;
  GtkAdjustment *adj;
  GPtrArray *items; /* tile *, in order */
  AdwAnimation *ease;
  double goal;      /* where ease is heading */
  gboolean hover;   /* the pointer's over it, so its arrows show */

  /* Dragging it, once the press has moved far enough to count. */
  gboolean panning;
  double press_value, pan_start, last_x;
  gint64 last_ms;
  double speed;     /* pixels a millisecond, smoothed */
} row;

static struct {
  app *a;
  home_view_callbacks cb;
  GtkWidget *page; /* a stack: "blank" until the rows are first read | "rows" | "welcome" */
  GtkWidget *scroller, *hint;
  AdwStatusPage *welcome;
  GtkWidget *import;
  row rows[MC_HOME_ROW_COUNT];
  AdwAnimation *ease; /* the page's, for Page Up and so on */
  double goal;
  tile *roving; /* the one tile Tab reaches */
  unsigned seed;
  gboolean dirty;   /* read again when next shown */
  gboolean loading; /* a read is under way */
  gboolean again;   /* something changed since it started: its result is out of date */
  GHashTable *art;  /* playlist or group uid -> its picture, as last looked up */
  guint art_gen;    /* bumped by each lookup, so only the latest one's results are used */
} v;

static double adj_max(GtkAdjustment *adj) {
  return MAX(0, gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj));
}

/* --- Covers --- */

static gboolean by_uid(mc_recent_kind kind) {
  return kind == MC_RECENT_PLAYLIST || kind == MC_RECENT_ALBUM_GROUP;
}

static void tile_cancel(tile *t) {
  if (!t->loading) return;
  g_cancellable_cancel(t->loading);
  g_clear_object(&t->loading);
}

static void on_cover(GdkTexture *texture, gpointer user) {
  tile *t = user;
  g_clear_object(&t->loading);
  gtk_picture_set_paintable(GTK_PICTURE(t->picture), GDK_PAINTABLE(texture));
}

/* Shows file (taken), or nothing with NULL. A cover that's still the same stays as it is. */
static void tile_set_cover(tile *t, char *file) {
  gint64 mtime = cover_mtime(file);
  gboolean same_file = !g_strcmp0(file, t->cover);
  if (same_file && mtime == t->cover_mtime) {
    g_free(file);
    return;
  }
  tile_cancel(t);
  g_free(t->cover);
  t->cover = file;
  t->cover_mtime = mtime;
  if (!same_file) gtk_picture_set_paintable(GTK_PICTURE(t->picture), NULL);
  if (!file) return;
  int size = COVER_SIZE * gtk_widget_get_scale_factor(t->button);
  GdkTexture *texture = NULL;
  if (cover_lookup_file(file, size, &texture)) {
    gtk_picture_set_paintable(GTK_PICTURE(t->picture), GDK_PAINTABLE(texture));
    g_clear_object(&texture);
  } else {
    t->loading = g_cancellable_new();
    cover_load_file(file, size, t->loading, on_cover, t);
  }
}

/* --- Tiles --- */

static void on_tile_clicked(GtkButton *button, gpointer user) {
  (void)button;
  tile *t = user;
  if (t->station) return; /* there's no radio yet */
  char uid[MC_UID_STRLEN];
  g_strlcpy(uid, t->uid, sizeof uid);
  char *artist = g_strdup(t->artist); /* opening can reload the rows */
  v.cb.open(v.cb.user, t->kind, uid, artist);
  g_free(artist);
}

static tile *tile_new(row *r, int index) {
  tile *t = g_new0(tile, 1);
  t->row = r;
  t->index = index;
  t->frame = cover_frame_new(COVER_SIZE, COVER_SIZE / 3, &t->picture);
  t->badge = gtk_image_new_from_icon_name("network-wireless-signal-excellent-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(t->badge), BADGE_ICON);
  gtk_widget_add_css_class(t->badge, "home-badge");
  gtk_widget_set_halign(t->badge, GTK_ALIGN_END);
  gtk_widget_set_valign(t->badge, GTK_ALIGN_END);
  gtk_widget_set_margin_end(t->badge, BADGE_INSET);
  gtk_widget_set_margin_bottom(t->badge, BADGE_INSET);
  gtk_overlay_add_overlay(GTK_OVERLAY(t->frame), t->badge);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_size_request(box, COVER_SIZE, -1);
  gtk_box_append(GTK_BOX(box), t->frame);
  t->title = gtk_label_new(NULL);
  gtk_label_set_ellipsize(GTK_LABEL(t->title), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars(GTK_LABEL(t->title), 1); /* as wide as the cover, no wider */
  gtk_widget_set_margin_top(t->title, 8);
  gtk_box_append(GTK_BOX(box), t->title);
  t->sub = gtk_label_new(NULL);
  gtk_label_set_ellipsize(GTK_LABEL(t->sub), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars(GTK_LABEL(t->sub), 1);
  gtk_widget_add_css_class(t->sub, "dim-label");
  gtk_widget_add_css_class(t->sub, "caption");
  gtk_box_append(GTK_BOX(box), t->sub);

  t->button = gtk_button_new();
  gtk_button_set_child(GTK_BUTTON(t->button), box);
  gtk_widget_add_css_class(t->button, "flat");
  gtk_widget_add_css_class(t->button, "home-tile");
  gtk_widget_set_focusable(t->button, FALSE); /* until it's the roving one */
  g_object_set_data(G_OBJECT(t->button), "home-tile", t);
  g_signal_connect(t->button, "clicked", G_CALLBACK(on_tile_clicked), t);
  gtk_box_append(GTK_BOX(r->tiles), t->button);
  return t;
}

static void tile_free(gpointer data) {
  tile *t = data;
  if (v.roving == t) v.roving = NULL;
  tile_cancel(t);
  gtk_box_remove(GTK_BOX(t->row->tiles), t->button);
  g_free(t->artist);
  g_free(t->cover);
  g_free(t);
}

/* A smart list's bolt, the sidebar's icon for the kind, or a note: what shows without a cover. */
static const char *pick_icon(const mc_home_pick *p) {
  if (p->smart) return "thunderbolt-symbolic";
  switch (p->kind) {
    case MC_RECENT_PLAYLIST: return "view-list-symbolic";
    case MC_RECENT_ALBUM_GROUP: return "folder-symbolic";
    case MC_RECENT_ARTIST: return "avatar-default-symbolic";
    default: return "folder-music-symbolic";
  }
}

/* The tile shows p, whatever it showed before. */
static void tile_set(tile *t, const mc_home_pick *p) {
  const char *title = p->title ? p->title : "", *sub = p->sub ? p->sub : "";
  t->kind = p->kind;
  t->station = p->station;
  g_strlcpy(t->uid, p->uid, sizeof t->uid);
  g_free(t->artist);
  t->artist = g_strdup(sub);
  gtk_label_set_text(GTK_LABEL(t->title), title);
  gtk_label_set_text(GTK_LABEL(t->sub), sub);
  char *name = mc_format_alloc(mc_str(MC_STR_LINUX_ALBUM_TILE_NAME), MC_S(title), MC_S(sub));
  gtk_accessible_update_property(GTK_ACCESSIBLE(t->button), GTK_ACCESSIBLE_PROPERTY_LABEL, name ? name : title, -1);
  mc_free(name);

  gtk_image_set_from_icon_name(GTK_IMAGE(gtk_overlay_get_child(GTK_OVERLAY(t->frame))), pick_icon(p));
  if (p->kind == MC_RECENT_ARTIST && !p->station) gtk_widget_add_css_class(t->frame, "home-round");
  else gtk_widget_remove_css_class(t->frame, "home-round");
  gtk_widget_set_visible(t->badge, p->station);

  char *file = by_uid(p->kind) ? g_strdup(v.art ? g_hash_table_lookup(v.art, p->uid) : NULL) : cover_path(p->cover_dir);
  tile_set_cover(t, file);
}

/* --- The focus --- */

/* Makes t the tile that Tab reaches, and with grab moves the focus to it. The viewports bring
 * it into view. */
static void set_roving(tile *t, gboolean grab) {
  tile *was = v.roving;
  v.roving = t;
  if (t) gtk_widget_set_focusable(t->button, TRUE);
  if (t && grab) gtk_widget_grab_focus(t->button);
  if (was && was != t) gtk_widget_set_focusable(was->button, FALSE);
}

static tile *first_tile(void) {
  for (int i = 0; i < MC_HOME_ROW_COUNT; i++)
    if (v.rows[i].items->len) return g_ptr_array_index(v.rows[i].items, 0);
  return NULL;
}

/* The first tile showing in r, for coming to it from another row. */
static tile *first_shown(row *r) {
  guint index = (guint)((gtk_adjustment_get_value(r->adj) + PITCH - 1) / PITCH);
  return g_ptr_array_index(r->items, MIN(index, r->items->len - 1));
}

static tile *focused_tile(void) {
  GtkRoot *root = gtk_widget_get_root(v.page);
  GtkWidget *focus = root ? gtk_root_get_focus(root) : NULL;
  return focus ? g_object_get_data(G_OBJECT(focus), "home-tile") : NULL;
}

gboolean home_view_is_tile(GtkWidget *widget) {
  return widget && g_object_get_data(G_OBJECT(widget), "home-tile");
}

/* --- Scrolling --- */

static void set_row_value(double value, gpointer user) {
  row *r = user;
  gtk_adjustment_set_value(r->adj, value);
}

static gboolean row_easing(row *r) {
  return adw_animation_get_state(r->ease) == ADW_ANIMATION_PLAYING;
}

static double row_goal(row *r) {
  return row_easing(r) ? r->goal : gtk_adjustment_get_value(r->adj);
}

static void row_animate(row *r, double to) {
  r->goal = CLAMP(to, 0, adj_max(r->adj));
  adw_timed_animation_set_value_from(ADW_TIMED_ANIMATION(r->ease), gtk_adjustment_get_value(r->adj));
  adw_timed_animation_set_value_to(ADW_TIMED_ANIMATION(r->ease), r->goal);
  adw_animation_play(r->ease);
}

/* Scrolls by dir pages of whole tiles, stopping with a tile at the left edge. */
static void row_page(row *r, int dir) {
  int whole = (gtk_widget_get_width(r->scroller) - 2 * PAD + (PITCH - COVER_SIZE)) / PITCH;
  int first = ((int)row_goal(r) + PITCH / 2) / PITCH;
  row_animate(r, (first + dir * MAX(whole, 1)) * PITCH);
}

static void update_arrows(row *r) {
  double value = gtk_adjustment_get_value(r->adj);
  gboolean shown = r->hover && !r->panning;
  gtk_widget_set_visible(r->left, shown && value > 0.5);
  gtk_widget_set_visible(r->right, shown && value < adj_max(r->adj) - 0.5);
}

static void on_row_moved(GtkAdjustment *adj, gpointer user) {
  (void)adj;
  update_arrows(user);
}

static void on_row_enter(GtkEventControllerMotion *motion, double x, double y, gpointer user) {
  (void)motion;
  (void)x;
  (void)y;
  row *r = user;
  r->hover = TRUE;
  update_arrows(r);
}

static void on_row_leave(GtkEventControllerMotion *motion, gpointer user) {
  (void)motion;
  row *r = user;
  r->hover = FALSE;
  update_arrows(r);
}

static void on_left(GtkButton *button, gpointer user) {
  (void)button;
  row_page(user, -1);
}

static void on_right(GtkButton *button, gpointer user) {
  (void)button;
  row_page(user, 1);
}

/* A sideways scroll, or Shift and the wheel, moves the row, a notch a tile. Anything else goes
 * on up to the page, whose wheel it is, however much of a trackpad's swipe was sideways too. */
static gboolean on_row_wheel(GtkEventControllerScroll *scroll, double dx, double dy, gpointer user) {
  row *r = user;
  GdkModifierType state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(scroll));
  double delta = dx;
  if (state & GDK_SHIFT_MASK) delta = dy != 0 ? dy : dx;
  else if (fabs(dx) <= fabs(dy)) return FALSE;
  if (gtk_event_controller_scroll_get_unit(scroll) == GDK_SCROLL_UNIT_WHEEL) {
    row_animate(r, row_goal(r) + delta * PITCH);
  } else {
    adw_animation_pause(r->ease);
    gtk_adjustment_set_value(r->adj, gtk_adjustment_get_value(r->adj) + delta);
  }
  return TRUE;
}

/* A trackpad's swipe carries on when let go, as a drag does. In pixels a second. */
static void on_row_swiped(GtkEventControllerScroll *scroll, double vx, double vy, gpointer user) {
  (void)scroll;
  row *r = user;
  if (fabs(vx) > fabs(vy)) row_animate(r, gtk_adjustment_get_value(r->adj) + vx * FLING_MS / 1000);
}

/* A scrolled window takes every scroll it's offered, even along an axis it can't move, which
 * would stop the wheel dead over a row on the way down the page. So a row's doesn't get them:
 * on_row_wheel() does its work, and lets the page have the rest. */
static void silence_scrolling(GtkWidget *scroller) {
  GListModel *controllers = gtk_widget_observe_controllers(scroller);
  for (guint i = 0, n = g_list_model_get_n_items(controllers); i < n; i++) {
    GtkEventController *controller = g_list_model_get_item(controllers, i);
    if (GTK_IS_EVENT_CONTROLLER_SCROLL(controller))
      gtk_event_controller_set_propagation_phase(controller, GTK_PHASE_NONE);
    g_object_unref(controller);
  }
  g_object_unref(controllers);
}

/* A touchscreen's drag is the scrolled window's own, with its kinetic scrolling. */
static void on_drag_begin(GtkGestureDrag *drag, double x, double y, gpointer user) {
  (void)x;
  (void)y;
  row *r = user;
  GdkDevice *device = gtk_gesture_get_device(GTK_GESTURE(drag));
  if (device && gdk_device_get_source(device) == GDK_SOURCE_TOUCHSCREEN) {
    gtk_gesture_set_state(GTK_GESTURE(drag), GTK_EVENT_SEQUENCE_DENIED);
    return;
  }
  adw_animation_pause(r->ease); /* catches a row that's still moving */
  r->press_value = gtk_adjustment_get_value(r->adj);
  r->panning = FALSE;
}

/* It's in the capture phase, so it sees the press before the tile under it, and claiming the
 * press takes it from the tile: a drag doesn't click. */
static void on_drag_update(GtkGestureDrag *drag, double x, double y, gpointer user) {
  (void)y;
  row *r = user;
  gint64 now = g_get_monotonic_time() / 1000;
  if (!r->panning) {
    if (!gtk_drag_check_threshold(r->scroller, 0, 0, (int)x, 0)) return;
    gtk_gesture_set_state(GTK_GESTURE(drag), GTK_EVENT_SEQUENCE_CLAIMED);
    r->panning = TRUE;
    r->pan_start = x; /* so it doesn't jump by the distance it took to start */
    r->last_x = x;
    r->last_ms = now;
    r->speed = 0;
    update_arrows(r);
  }
  if (now != r->last_ms) {
    r->speed = r->speed / 2 + (r->last_x - x) / (double)(now - r->last_ms) / 2;
    r->last_x = x;
    r->last_ms = now;
  }
  gtk_adjustment_set_value(r->adj, r->press_value - (x - r->pan_start));
}

static void on_drag_end(GtkGestureDrag *drag, double x, double y, gpointer user) {
  (void)drag;
  (void)x;
  (void)y;
  row *r = user;
  if (!r->panning) return;
  r->panning = FALSE;
  if (g_get_monotonic_time() / 1000 - r->last_ms < STILL_MS)
    row_animate(r, gtk_adjustment_get_value(r->adj) + r->speed * FLING_MS);
  update_arrows(r);
}

static void set_page_value(double value, gpointer user) {
  (void)user;
  gtk_adjustment_set_value(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(v.scroller)), value);
}

static void page_animate(double to) {
  GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(v.scroller));
  v.goal = CLAMP(to, 0, adj_max(adj));
  adw_timed_animation_set_value_from(ADW_TIMED_ANIMATION(v.ease), gtk_adjustment_get_value(adj));
  adw_timed_animation_set_value_to(ADW_TIMED_ANIMATION(v.ease), v.goal);
  adw_animation_play(v.ease);
}

static double page_goal(void) {
  GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(v.scroller));
  return adw_animation_get_state(v.ease) == ADW_ANIMATION_PLAYING ? v.goal : gtk_adjustment_get_value(adj);
}

/* --- The keyboard --- */

/* The next row with tiles after r, in direction step, or NULL. */
static row *row_beside(row *r, int step) {
  for (int i = (int)(r - v.rows) + step; i >= 0 && i < MC_HOME_ROW_COUNT; i += step)
    if (v.rows[i].items->len) return &v.rows[i];
  return NULL;
}

/* The window's own arrow keys would move the focus off the row, so these keep them, even at an
 * end. With Shift or Ctrl they're the window's shortcuts, which have had them by now. */
static gboolean on_key(GtkEventControllerKey *keys, guint keyval, guint keycode, GdkModifierType state, gpointer user) {
  (void)keys;
  (void)keycode;
  (void)user;
  if (state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SHIFT_MASK | GDK_SUPER_MASK | GDK_META_MASK)) return FALSE;
  GtkAdjustment *page = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(v.scroller));
  switch (keyval) {
    case GDK_KEY_Page_Down:
    case GDK_KEY_KP_Page_Down: page_animate(page_goal() + gtk_adjustment_get_page_increment(page)); return TRUE;
    case GDK_KEY_Page_Up:
    case GDK_KEY_KP_Page_Up: page_animate(page_goal() - gtk_adjustment_get_page_increment(page)); return TRUE;
    case GDK_KEY_Home:
    case GDK_KEY_KP_Home: page_animate(0); return TRUE;
    case GDK_KEY_End:
    case GDK_KEY_KP_End: page_animate(adj_max(page)); return TRUE;
    default: break;
  }
  tile *t = focused_tile();
  if (!t) return FALSE;
  row *r = t->row;
  if (keyval == GDK_KEY_Left || keyval == GDK_KEY_KP_Left || keyval == GDK_KEY_Right || keyval == GDK_KEY_KP_Right) {
    int index = t->index + (keyval == GDK_KEY_Left || keyval == GDK_KEY_KP_Left ? -1 : 1);
    if (index >= 0 && (guint)index < r->items->len) set_roving(g_ptr_array_index(r->items, (guint)index), TRUE);
    return TRUE;
  }
  if (keyval == GDK_KEY_Up || keyval == GDK_KEY_KP_Up || keyval == GDK_KEY_Down || keyval == GDK_KEY_KP_Down) {
    row *next = row_beside(r, keyval == GDK_KEY_Up || keyval == GDK_KEY_KP_Up ? -1 : 1);
    if (next) set_roving(first_shown(next), TRUE);
    return TRUE;
  }
  return FALSE;
}

/* --- Reading the rows --- */

/* The tiles row r holds become picks, n of them, reusing those it has. */
static void row_fill(row *r, const mc_home_pick *picks, int n) {
  guint count = (guint)n;
  if (r->items->len > count) g_ptr_array_set_size(r->items, count); /* frees the rest */
  for (guint i = 0; i < count; i++) {
    if (i == r->items->len) g_ptr_array_add(r->items, tile_new(r, (int)i));
    tile_set(g_ptr_array_index(r->items, i), &picks[i]);
  }
  gtk_widget_set_visible(r->box, n > 0);
}

/* The welcome, or with no library what went wrong. */
static void set_welcome(void) {
  app *a = v.a;
  if (a->lib) {
    char *title = mc_format_alloc(mc_str(MC_STR_HOME_WELCOME_TITLE), MC_S(mc_str(MC_STR_APP_NAME)));
    adw_status_page_set_title(v.welcome, title ? title : "");
    mc_free(title);
  } else {
    adw_status_page_set_title(v.welcome, mc_str(MC_STR_LIBRARY_OPEN_FAILED));
  }
  adw_status_page_set_description(v.welcome, a->lib ? mc_str(MC_STR_HOME_WELCOME_TEXT) : NULL);
  gtk_widget_set_visible(v.import, a->lib != NULL);
}

static void show_picks(const mc_home_picks *picks) {
  gboolean focused = focused_tile() != NULL;
  for (int i = 0; i < MC_HOME_ROW_COUNT; i++) row_fill(&v.rows[i], picks->rows[i], picks->counts[i]);
  if (!v.roving || focused) set_roving(v.roving ? v.roving : first_tile(), focused);
  gtk_widget_set_visible(v.hint, picks->has_music && !picks->has_plays);
  set_welcome();
  const char *shown = v.a->first_run ? "blank" /* the first run's dialog is over it */
                      : picks->has_music ? "rows"
                                         : "welcome";
  gtk_stack_set_visible_child_name(GTK_STACK(v.page), shown);
}

/* Playlists' and groups' pictures, on a worker thread with a library connection of its own: one
 * that isn't cached yet takes decoding four covers. */
typedef struct art_job {
  GtkWidget *page; /* kept alive until the job is done */
  guint gen;
  char *lib_dir, *art_dir;
  GPtrArray *uids;   /* char * */
  GArray *groups;    /* gboolean, one per uid: an album group's rather than a playlist's */
  GPtrArray *paths;  /* char *, one per uid, NULL for none */
} art_job;

static void art_job_free(gpointer data) {
  art_job *j = data;
  g_object_unref(j->page);
  g_free(j->lib_dir);
  g_free(j->art_dir);
  g_ptr_array_unref(j->uids);
  g_array_unref(j->groups);
  g_ptr_array_unref(j->paths);
  g_free(j);
}

static void art_thread(GTask *task, gpointer source, gpointer data, GCancellable *cancellable) {
  (void)source;
  (void)cancellable;
  art_job *j = data;
  mc_library *lib = NULL;
  if (mc_library_open(j->lib_dir, &lib) == MC_OK) {
    for (guint i = 0; i < j->uids->len; i++) {
      const char *uid = g_ptr_array_index(j->uids, i);
      char *path = NULL;
      mc_status st = g_array_index(j->groups, gboolean, i) ? mc_album_group_art(lib, uid, j->art_dir, &path)
                                                           : mc_playlist_art(lib, uid, j->art_dir, &path);
      if (st == MC_ERR_NOT_FOUND) st = mc_smart_art(lib, uid, j->art_dir, &path); /* uids never clash */
      g_ptr_array_index(j->paths, i) = st == MC_OK ? g_strdup(path) : NULL;
      mc_free(path);
    }
    mc_library_close(lib);
  }
  g_task_return_boolean(task, TRUE);
}

static void art_done(GObject *source, GAsyncResult *result, gpointer user) {
  (void)source;
  (void)user;
  art_job *j = g_task_get_task_data(G_TASK(result));
  if (j->gen != v.art_gen) return; /* a later lookup is on its way */
  for (guint i = 0; i < j->uids->len; i++) {
    const char *path = g_ptr_array_index(j->paths, i);
    g_hash_table_insert(v.art, g_strdup(g_ptr_array_index(j->uids, i)), g_strdup(path));
  }
  for (int i = 0; i < MC_HOME_ROW_COUNT; i++)
    for (guint k = 0; k < v.rows[i].items->len; k++) {
      tile *t = g_ptr_array_index(v.rows[i].items, k);
      if (by_uid(t->kind)) tile_set_cover(t, g_strdup(g_hash_table_lookup(v.art, t->uid)));
    }
}

/* Looks up the pictures of the playlists and groups in the rows, as they may have changed. */
static void art_lookup(void) {
  v.art_gen++;
  if (!v.a->lib || !v.a->art_dir) return;
  art_job *j = g_new0(art_job, 1);
  j->uids = g_ptr_array_new_with_free_func(g_free);
  j->groups = g_array_new(FALSE, FALSE, sizeof(gboolean));
  j->paths = g_ptr_array_new_with_free_func(g_free);
  for (int i = 0; i < MC_HOME_ROW_COUNT; i++)
    for (guint k = 0; k < v.rows[i].items->len; k++) {
      tile *t = g_ptr_array_index(v.rows[i].items, k);
      if (!by_uid(t->kind)) continue;
      gboolean group = t->kind == MC_RECENT_ALBUM_GROUP;
      g_ptr_array_add(j->uids, g_strdup(t->uid));
      g_array_append_val(j->groups, group);
    }
  g_ptr_array_set_size(j->paths, (gint)j->uids->len);
  j->page = g_object_ref(v.page);
  j->gen = v.art_gen;
  j->lib_dir = g_strdup(mc_library_dir(v.a->lib));
  j->art_dir = g_strdup(v.a->art_dir);
  GTask *task = g_task_new(NULL, NULL, art_done, NULL);
  g_task_set_task_data(task, j, art_job_free);
  if (j->uids->len) g_task_run_in_thread(task, art_thread);
  else g_task_return_boolean(task, TRUE);
  g_object_unref(task);
}

/* A read of the rows. */
typedef struct load_job {
  GtkWidget *page; /* kept alive until the job is done */
  char *lib_dir;   /* NULL for no library */
  unsigned seed;
  mc_home_picks picks;
} load_job;

static void load_job_free(gpointer data) {
  load_job *j = data;
  g_object_unref(j->page);
  g_free(j->lib_dir);
  mc_home_picks_free(&j->picks);
  g_free(j);
}

static void load_thread(GTask *task, gpointer source, gpointer data, GCancellable *cancellable) {
  (void)source;
  (void)cancellable;
  load_job *j = data;
  mc_library *lib = NULL;
  if (j->lib_dir && mc_library_open(j->lib_dir, &lib) != MC_OK) lib = NULL;
  mc_home_picks_load(lib, j->seed, &j->picks);
  mc_library_close(lib);
  g_task_return_boolean(task, TRUE);
}

static void load(void);

static void load_done(GObject *source, GAsyncResult *result, gpointer user) {
  (void)source;
  (void)user;
  load_job *j = g_task_get_task_data(G_TASK(result));
  v.loading = FALSE;
  if (v.again) { /* it may even be of another library */
    v.again = FALSE;
    load();
    return;
  }
  show_picks(&j->picks);
  art_lookup();
}

static void load(void) {
  v.dirty = FALSE;
  if (v.loading) {
    v.again = TRUE;
    return;
  }
  v.loading = TRUE;
  load_job *j = g_new0(load_job, 1);
  j->page = g_object_ref(v.page);
  j->lib_dir = v.a->lib ? g_strdup(mc_library_dir(v.a->lib)) : NULL;
  j->seed = v.seed;
  GTask *task = g_task_new(NULL, NULL, load_done, NULL);
  g_task_set_task_data(task, j, load_job_free);
  g_task_run_in_thread(task, load_thread);
  g_object_unref(task);
}

void home_view_refresh(void) {
  v.dirty = TRUE;
  if (v.page && gtk_widget_get_mapped(v.page)) load();
}

static void on_map(GtkWidget *page, gpointer user) {
  (void)page;
  (void)user;
  if (v.dirty) load();
}

/* --- Building it --- */

static void on_import(GtkButton *button, gpointer user) {
  (void)button;
  (void)user;
  v.cb.import(v.cb.user);
}

static GtkWidget *arrow_new(const char *icon, GtkAlign side, GCallback clicked, row *r) {
  GtkWidget *b = gtk_button_new_from_icon_name(icon);
  gtk_widget_add_css_class(b, "circular");
  gtk_widget_add_css_class(b, "osd");
  gtk_widget_set_size_request(b, ARROW, ARROW);
  gtk_widget_set_focusable(b, FALSE); /* the keys move along the row */
  gtk_widget_set_halign(b, side);
  gtk_widget_set_valign(b, GTK_ALIGN_START); /* level with the middle of the covers */
  gtk_widget_set_margin_top(b, TILE_PAD + (COVER_SIZE - ARROW) / 2);
  gtk_widget_set_margin_start(b, ARROW_INSET);
  gtk_widget_set_margin_end(b, ARROW_INSET);
  gtk_widget_set_visible(b, FALSE);
  g_signal_connect(b, "clicked", clicked, r);
  return b;
}

/* Row i, hidden until it has tiles. */
static GtkWidget *row_new(row *r, mc_home_row_id id) {
  r->items = g_ptr_array_new_with_free_func(tile_free);
  r->tiles = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, TILE_GAP);
  gtk_widget_set_margin_start(r->tiles, PAD - TILE_PAD);
  gtk_widget_set_margin_end(r->tiles, PAD - TILE_PAD);
  const char *title = mc_str(mc_home_row_title(id));
  gtk_accessible_update_property(GTK_ACCESSIBLE(r->tiles), GTK_ACCESSIBLE_PROPERTY_LABEL, title, -1);

  /* No scrollbar: there's the arrows, the wheel and dragging. A row's height is its tiles'. */
  r->scroller = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(r->scroller), GTK_POLICY_EXTERNAL, GTK_POLICY_NEVER);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(r->scroller), r->tiles);
  r->adj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(r->scroller));
  g_signal_connect(r->adj, "value-changed", G_CALLBACK(on_row_moved), r);
  g_signal_connect(r->adj, "changed", G_CALLBACK(on_row_moved), r);
  r->ease = adw_timed_animation_new(r->scroller, 0, 0, EASE_MS,
                                    adw_callback_animation_target_new(set_row_value, r, NULL));
  adw_timed_animation_set_easing(ADW_TIMED_ANIMATION(r->ease), ADW_EASE_OUT_CUBIC);

  silence_scrolling(r->scroller);
  GtkEventController *wheel =
    gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES | GTK_EVENT_CONTROLLER_SCROLL_KINETIC);
  g_signal_connect(wheel, "scroll", G_CALLBACK(on_row_wheel), r);
  g_signal_connect(wheel, "decelerate", G_CALLBACK(on_row_swiped), r);
  gtk_widget_add_controller(r->scroller, wheel);

  GtkGesture *drag = gtk_gesture_drag_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(drag), GTK_PHASE_CAPTURE);
  g_signal_connect(drag, "drag-begin", G_CALLBACK(on_drag_begin), r);
  g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), r);
  g_signal_connect(drag, "drag-end", G_CALLBACK(on_drag_end), r);
  gtk_widget_add_controller(r->scroller, GTK_EVENT_CONTROLLER(drag));

  GtkWidget *overlay = gtk_overlay_new();
  gtk_overlay_set_child(GTK_OVERLAY(overlay), r->scroller);
  r->left = arrow_new("go-previous-symbolic", GTK_ALIGN_START, G_CALLBACK(on_left), r);
  r->right = arrow_new("go-next-symbolic", GTK_ALIGN_END, G_CALLBACK(on_right), r);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), r->left);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), r->right);
  GtkEventController *motion = gtk_event_controller_motion_new();
  g_signal_connect(motion, "enter", G_CALLBACK(on_row_enter), r);
  g_signal_connect(motion, "leave", G_CALLBACK(on_row_leave), r);
  gtk_widget_add_controller(overlay, motion);

  GtkWidget *heading = gtk_label_new(title);
  gtk_widget_add_css_class(heading, "title-2");
  gtk_label_set_xalign(GTK_LABEL(heading), 0);
  gtk_label_set_ellipsize(GTK_LABEL(heading), PANGO_ELLIPSIZE_END);
  gtk_widget_set_margin_start(heading, PAD);
  gtk_widget_set_margin_end(heading, PAD);
  gtk_widget_set_margin_bottom(heading, TITLE_GAP);

  r->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append(GTK_BOX(r->box), heading);
  gtk_box_append(GTK_BOX(r->box), overlay);
  gtk_widget_set_visible(r->box, FALSE);
  return r->box;
}

GtkWidget *home_view_new(app *a, const home_view_callbacks *callbacks) {
  v.a = a;
  v.cb = *callbacks;
  v.seed = g_random_int();
  v.dirty = TRUE;
  v.art = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

  GtkWidget *rows = gtk_box_new(GTK_ORIENTATION_VERTICAL, ROW_GAP);
  gtk_widget_set_margin_top(rows, PAD);
  gtk_widget_set_margin_bottom(rows, PAD);
  for (int i = 0; i < MC_HOME_ROW_COUNT; i++) gtk_box_append(GTK_BOX(rows), row_new(&v.rows[i], (mc_home_row_id)i));
  v.hint = gtk_label_new(mc_str(MC_STR_HOME_NO_HISTORY));
  gtk_label_set_xalign(GTK_LABEL(v.hint), 0);
  gtk_label_set_wrap(GTK_LABEL(v.hint), TRUE);
  gtk_widget_add_css_class(v.hint, "dim-label");
  gtk_widget_set_margin_start(v.hint, PAD);
  gtk_widget_set_margin_end(v.hint, PAD);
  gtk_widget_set_visible(v.hint, FALSE);
  gtk_box_append(GTK_BOX(rows), v.hint);

  v.scroller = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(v.scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(v.scroller), rows);
  gtk_widget_add_css_class(v.scroller, "view");
  v.ease = adw_timed_animation_new(v.scroller, 0, 0, EASE_MS, adw_callback_animation_target_new(set_page_value, NULL, NULL));
  adw_timed_animation_set_easing(ADW_TIMED_ANIMATION(v.ease), ADW_EASE_OUT_CUBIC);
  GtkEventController *keys = gtk_event_controller_key_new();
  g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), NULL);
  gtk_widget_add_controller(v.scroller, keys);

  GtkWidget *welcome = adw_status_page_new();
  adw_status_page_set_icon_name(ADW_STATUS_PAGE(welcome), "folder-music-symbolic");
  v.welcome = ADW_STATUS_PAGE(welcome);
  v.import = gtk_button_new_with_label(mc_str(MC_STR_IMPORT_MUSIC));
  gtk_widget_add_css_class(v.import, "pill");
  gtk_widget_add_css_class(v.import, "suggested-action");
  gtk_widget_set_halign(v.import, GTK_ALIGN_CENTER);
  g_signal_connect(v.import, "clicked", G_CALLBACK(on_import), NULL);
  adw_status_page_set_child(v.welcome, v.import);

  v.page = gtk_stack_new();
  GtkWidget *blank = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); /* a moment's blank: the welcome
                                                                * would flash up over a full library */
  gtk_widget_add_css_class(blank, "view");
  gtk_stack_add_named(GTK_STACK(v.page), blank, "blank");
  gtk_stack_add_named(GTK_STACK(v.page), v.scroller, "rows");
  gtk_stack_add_named(GTK_STACK(v.page), brand_backdrop_new(welcome), "welcome");
  g_signal_connect(v.page, "map", G_CALLBACK(on_map), NULL);
  return v.page;
}

GtkWidget *home_view_scrolled(void) {
  return v.scroller;
}

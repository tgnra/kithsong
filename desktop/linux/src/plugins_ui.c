#include "plugins_ui.h"

#include <string.h>

#include "kithcore/plugins.h"

static struct {
  char *settings_dir;
  mc_settings *settings; /* the ones mc_plugins_load() holds */
  void (*changed)(void *user);
  void *user;
  GtkWidget *group;   /* Settings' group while it's shown, else NULL */
  GPtrArray *rows;    /* the group's rows, to take out when it's made again */
  guint rebuild_source;
  gboolean filling;   /* ignore the rows' signals while they're made */
} g_pu;

/* --- Settings --- */

/* Saves the plugins section into settings.json as it is now, keeping the rest of the file. */
static gboolean save(void) {
  if (!g_pu.settings) return FALSE;
  mc_plugins_flush_settings();
  mc_settings *fresh = NULL;
  if (mc_settings_load(g_pu.settings_dir, &fresh) != MC_OK) return FALSE;
  gboolean ok = mc_settings_copy_plugins(fresh, g_pu.settings) == MC_OK && mc_settings_save(fresh) == MC_OK;
  mc_settings_free(fresh);
  return ok;
}

void plugins_ui_load(const char *settings_dir) {
  if (g_pu.settings) return;
  g_pu.settings_dir = g_strdup(settings_dir);
  if (mc_settings_load(settings_dir, &g_pu.settings) != MC_OK) {
    g_pu.settings = NULL;
    return;
  }
  mc_plugins_load(g_pu.settings, NULL, 0);
}

void plugins_ui_unload(void) {
  if (!g_pu.settings) return;
  if (mc_plugins_flush_settings()) save();
  mc_plugins_unload();
  mc_settings_free(g_pu.settings);
  g_pu.settings = NULL;
  g_clear_pointer(&g_pu.settings_dir, g_free);
}

void plugins_ui_init(void (*changed)(void *user), void *user) {
  g_pu.changed = changed;
  g_pu.user = user;
}

/* --- The group --- */

static void rebuild(void);

static gboolean on_rebuild(gpointer data) {
  (void)data;
  g_pu.rebuild_source = 0;
  rebuild();
  return G_SOURCE_REMOVE;
}

/* Makes the rows again once the signal under way is over: never destroy a row in its own. */
static void rebuild_later(void) {
  if (!g_pu.rebuild_source) g_pu.rebuild_source = g_idle_add(on_rebuild, NULL);
}

static void toast(const char *text) {
  GtkWidget *dialog = g_pu.group ? gtk_widget_get_ancestor(g_pu.group, ADW_TYPE_PREFERENCES_DIALOG) : NULL;
  if (!dialog) return;
  AdwToast *t = adw_toast_new("");
  adw_toast_set_use_markup(t, FALSE);
  adw_toast_set_title(t, text);
  adw_preferences_dialog_add_toast(ADW_PREFERENCES_DIALOG(dialog), t);
}

static void failed(mc_status st) {
  char text[256];
  mc_format(text, sizeof text, mc_str(MC_STR_PLUGIN_CHANGE_FAILED), MC_S(mc_status_str(st)));
  toast(text);
}

/* After a change: saved, and the app told. */
static void changed(void) {
  if (!save()) toast(mc_str(MC_STR_SETTINGS_SAVE_FAILED));
  if (g_pu.changed) g_pu.changed(g_pu.user);
}

static void set_enabled(const char *file_id, gboolean on) {
  mc_status st = mc_plugins_set_enabled(file_id, on);
  if (st != MC_OK) failed(st);
  changed();
  rebuild_later();
}

static void on_turn_on_response(AdwAlertDialog *dialog, const char *response, gpointer data) {
  (void)data;
  const char *file_id = g_object_get_data(G_OBJECT(dialog), "file-id");
  if (!strcmp(response, "on") && file_id) set_enabled(file_id, TRUE);
}

/* Turning on asks first, as it runs code with the app's rights; the row shows what is until
 * then. */
static void toggled(const char *file_id, const char *name, const char *path, gboolean on) {
  rebuild_later(); /* back to what is, unless it changes */
  if (!on) {
    set_enabled(file_id, FALSE);
    return;
  }
  char *base = path ? g_path_get_basename(path) : g_strdup(file_id); /* the row has the whole path */
  char *heading = mc_format_alloc(mc_str(MC_STR_PLUGIN_TURN_ON_TITLE), MC_S(name));
  char *body = mc_format_alloc(mc_str(MC_STR_PLUGIN_TURN_ON_TEXT), MC_S(base));
  AdwDialog *dialog = adw_alert_dialog_new(heading, body);
  mc_free(heading);
  mc_free(body);
  g_free(base);
  AdwAlertDialog *alert = ADW_ALERT_DIALOG(dialog);
  adw_alert_dialog_add_responses(alert, "cancel", mc_str_mnemonic(MC_STR_CANCEL), "on",
                                 mc_str_mnemonic(MC_STR_PLUGIN_TURN_ON), NULL);
  adw_alert_dialog_set_default_response(alert, "cancel");
  adw_alert_dialog_set_close_response(alert, "cancel");
  g_object_set_data_full(G_OBJECT(dialog), "file-id", g_strdup(file_id), g_free);
  g_signal_connect(dialog, "response", G_CALLBACK(on_turn_on_response), NULL);
  adw_dialog_present(dialog, g_pu.group);
}

static void on_switch_active(GObject *row, GParamSpec *pspec, gpointer data) {
  (void)pspec, (void)data;
  if (g_pu.filling) return;
  gboolean on = ADW_IS_SWITCH_ROW(row) ? adw_switch_row_get_active(ADW_SWITCH_ROW(row))
                                       : adw_expander_row_get_enable_expansion(ADW_EXPANDER_ROW(row));
  toggled(g_object_get_data(row, "file-id"), g_object_get_data(row, "name"), g_object_get_data(row, "path"), on);
}

/* --- A plugin's settings --- */

static void set_option(GObject *row, const char *value) {
  if (g_pu.filling) return;
  mc_status st = mc_plugins_set_option(g_object_get_data(row, "file-id"), g_object_get_data(row, "key"), value);
  if (st != MC_OK) {
    failed(st);
    rebuild_later(); /* back to what's stored */
    return;
  }
  if (!save()) toast(mc_str(MC_STR_SETTINGS_SAVE_FAILED));
}

static void on_option_switch(GObject *row, GParamSpec *pspec, gpointer data) {
  (void)pspec, (void)data;
  set_option(row, adw_switch_row_get_active(ADW_SWITCH_ROW(row)) ? "1" : "0");
}

static void on_option_choice(GObject *row, GParamSpec *pspec, gpointer data) {
  (void)pspec, (void)data;
  guint i = adw_combo_row_get_selected(ADW_COMBO_ROW(row));
  char **ids = g_object_get_data(row, "ids");
  if (ids && i < g_strv_length(ids)) set_option(row, ids[i]);
}

static void on_option_number(GObject *row, GParamSpec *pspec, gpointer data) {
  (void)pspec, (void)data;
  char text[32];
  g_snprintf(text, sizeof text, "%.0f", adw_spin_row_get_value(ADW_SPIN_ROW(row)));
  set_option(row, text);
}

static void tag(GtkWidget *row, const char *file_id, const char *key) {
  g_object_set_data_full(G_OBJECT(row), "file-id", g_strdup(file_id), g_free);
  g_object_set_data_full(G_OBJECT(row), "key", g_strdup(key), g_free);
}

static GtkWidget *option_row(const char *file_id, const mc_plugin_option *o) {
  GtkWidget *row = NULL;
  switch (o->kind) {
    case MC_PLUGIN_OPTION_SWITCH:
      row = adw_switch_row_new();
      adw_switch_row_set_active(ADW_SWITCH_ROW(row), o->value && !strcmp(o->value, "1"));
      g_signal_connect(row, "notify::active", G_CALLBACK(on_option_switch), NULL);
      break;
    case MC_PLUGIN_OPTION_CHOICE: {
      GtkStringList *labels = gtk_string_list_new(NULL);
      char **ids = g_new0(char *, (gsize)o->choice_count + 1);
      guint sel = 0;
      for (int i = 0; i < o->choice_count; i++) {
        gtk_string_list_append(labels, o->choice_labels[i] ? o->choice_labels[i] : o->choice_ids[i]);
        ids[i] = g_strdup(o->choice_ids[i]);
        if (o->value && !strcmp(o->value, o->choice_ids[i])) sel = (guint)i;
      }
      row = adw_combo_row_new();
      adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(labels));
      g_object_unref(labels);
      adw_combo_row_set_selected(ADW_COMBO_ROW(row), sel);
      g_object_set_data_full(G_OBJECT(row), "ids", ids, (GDestroyNotify)g_strfreev);
      g_signal_connect(row, "notify::selected", G_CALLBACK(on_option_choice), NULL);
      break;
    }
    case MC_PLUGIN_OPTION_NUMBER: {
      row = adw_spin_row_new_with_range((double)o->min, (double)o->max, 1);
      adw_spin_row_set_value(ADW_SPIN_ROW(row), o->value ? g_ascii_strtod(o->value, NULL) : (double)o->min);
      g_signal_connect(row, "notify::value", G_CALLBACK(on_option_number), NULL);
      break;
    }
  }
  if (!row) return NULL;
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), o->label ? o->label : o->key);
  if (o->description) adw_action_row_set_subtitle(ADW_ACTION_ROW(row), o->description);
  tag(row, file_id, o->key);
  return row;
}

/* --- The rows --- */

/* "Version 1.0 · /path/to/it.so" and, on a line of its own, its state; g_free it. */
static char *plugin_subtitle(const mc_plugin_info *p) {
  GString *s = g_string_new(NULL);
  if (p->version) {
    char *v = mc_format_alloc(mc_str(MC_STR_PLUGIN_VERSION), MC_S(p->version));
    g_string_append(s, v);
    g_string_append(s, mc_str(MC_STR_SEPARATOR_DOT));
    mc_free(v);
  }
  g_string_append(s, p->path ? p->path : "");
  g_string_append_c(s, '\n');
  switch (p->state) {
    case MC_PLUGIN_LOADED: g_string_append(s, mc_str(MC_STR_PLUGIN_RUNNING)); break;
    case MC_PLUGIN_UNLOADING: g_string_append(s, mc_str(MC_STR_PLUGIN_UNLOADING)); break;
    case MC_PLUGIN_REFUSED: {
      char *t = mc_format_alloc(mc_str(MC_STR_PLUGIN_REFUSED), MC_S(p->reason ? p->reason : ""));
      g_string_append(s, t);
      mc_free(t);
      break;
    }
    default: g_string_append(s, mc_str(MC_STR_PLUGIN_OFF)); break;
  }
  return g_string_free(s, FALSE);
}

static void add_row(GtkWidget *row) {
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(g_pu.group), row);
  g_ptr_array_add(g_pu.rows, row);
}

static GtkWidget *plugin_row(const mc_plugin_info *p) {
  mc_plugin_option_list *opts = NULL;
  if (p->state != MC_PLUGIN_LOADED || mc_plugins_options(p->file_id, &opts) != MC_OK) opts = NULL;
  char *sub = plugin_subtitle(p);
  GtkWidget *row;
  if (opts && opts->count > 0) {
    row = adw_expander_row_new();
    adw_expander_row_set_show_enable_switch(ADW_EXPANDER_ROW(row), TRUE);
    adw_expander_row_set_enable_expansion(ADW_EXPANDER_ROW(row), p->enabled);
    adw_expander_row_set_subtitle(ADW_EXPANDER_ROW(row), sub);
    adw_expander_row_set_subtitle_lines(ADW_EXPANDER_ROW(row), 0);
    for (int i = 0; i < opts->count; i++) {
      GtkWidget *o = option_row(p->file_id, &opts->items[i]);
      if (o) adw_expander_row_add_row(ADW_EXPANDER_ROW(row), o);
    }
    g_signal_connect(row, "notify::enable-expansion", G_CALLBACK(on_switch_active), NULL);
  } else {
    row = adw_switch_row_new();
    adw_switch_row_set_active(ADW_SWITCH_ROW(row), p->enabled);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), sub);
    adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(row), 0);
    g_signal_connect(row, "notify::active", G_CALLBACK(on_switch_active), NULL);
  }
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), p->name);
  g_object_set_data_full(G_OBJECT(row), "file-id", g_strdup(p->file_id), g_free);
  g_object_set_data_full(G_OBJECT(row), "name", g_strdup(p->name), g_free);
  g_object_set_data_full(G_OBJECT(row), "path", g_strdup(p->path), g_free);
  g_free(sub);
  mc_plugin_option_list_free(opts);
  return row;
}

static void rebuild(void) {
  if (!g_pu.group) return;
  g_pu.filling = TRUE;
  for (guint i = 0; i < g_pu.rows->len; i++)
    adw_preferences_group_remove(ADW_PREFERENCES_GROUP(g_pu.group), g_ptr_array_index(g_pu.rows, i));
  g_ptr_array_set_size(g_pu.rows, 0);
  mc_plugin_list *list = NULL;
  int n = 0;
  if (mc_plugins_list(&list) == MC_OK && list) {
    for (int i = 0; i < list->count; i++, n++) add_row(plugin_row(&list->items[i]));
    mc_plugin_list_free(list);
  }
  if (!n) {
    /* Where a user's own go: $XDG_DATA_HOME/kithsong/plugins. */
    char *dir = g_build_filename(g_get_user_data_dir(), "kithsong", "plugins", NULL);
    char *text = mc_format_alloc(mc_str(MC_STR_PLUGINS_NONE), MC_S(dir));
    GtkWidget *row = adw_action_row_new();
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), text);
    adw_action_row_set_title_lines(ADW_ACTION_ROW(row), 0);
    gtk_widget_add_css_class(row, "dim-label");
    add_row(row);
    mc_free(text);
    g_free(dir);
  }
  g_pu.filling = FALSE;
}

static void on_group_destroy(GtkWidget *widget, gpointer data) {
  (void)widget, (void)data;
  g_clear_handle_id(&g_pu.rebuild_source, g_source_remove);
  g_clear_pointer(&g_pu.rows, g_ptr_array_unref);
  g_pu.group = NULL;
}

GtkWidget *plugins_ui_group(void) {
  GtkWidget *group = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), mc_str(MC_STR_SETTINGS_PLUGINS));
  char *escaped = g_markup_escape_text(mc_str(MC_STR_PLUGINS_TEXT), -1);
  adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(group), escaped);
  g_free(escaped);
  g_pu.group = group;
  g_pu.rows = g_ptr_array_new();
  g_signal_connect(group, "destroy", G_CALLBACK(on_group_destroy), NULL);
  rebuild();
  return group;
}

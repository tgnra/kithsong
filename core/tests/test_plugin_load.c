/* Finding and loading plugins (kithcore/plugins.h): the test plugins in plugins/ beside the
 * tests (plugin_folder.c, built three ways) are found, listed while off and not loaded;
 * turning one on loads it; one with another ABI and one whose init fails are listed with
 * their reasons; a plugin's options and its own settings reach settings.json; what's on is
 * loaded at the next start, and what's off isn't. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kithcore/kithcore.h"
#include "plugin/plugins_internal.h"
#include "settings_internal.h"
#include "test.h"
#include "util/platform.h"

static const mc_plugin_info *find(const mc_plugin_list *l, const char *file_id) {
  for (int i = 0; l && i < l->count; i++)
    if (!strcmp(l->items[i].file_id, file_id)) return &l->items[i];
  return NULL;
}

typedef struct value_find {
  const char *key;
  char value[64];
} value_find;

static void on_value(void *user, const char *key, const char *value) {
  value_find *v = user;
  if (!strcmp(key, v->key)) snprintf(v->value, sizeof v->value, "%s", value);
}

/* The plugin's stored value for key, "" if none. */
static const char *stored(mc_settings *s, const char *plugin_id, const char *key, value_find *v) {
  v->key = key;
  v->value[0] = '\0';
  mc_settings_plugin_values(s, plugin_id, on_value, v);
  return v->value;
}

int main(int argc, char **argv) {
  if (argc < 2) return 2;
  char dir[1024], cfg[1024];
  snprintf(dir, sizeof dir, "%s/plugins", argv[1]);
  snprintf(cfg, sizeof cfg, "%s/plugin_load_cfg", argv[1]);
  mc_remove_tree(cfg);
  const char *dirs[] = {dir};
  value_find v;

  mc_settings *s = NULL;
  CHECK(mc_settings_load(cfg, &s) == MC_OK);

  /* All found, all off, none loaded. */
  CHECK(mc_plugins_load(s, dirs, 1) == MC_OK);
  CHECK(mc_plugins_load(s, dirs, 1) == MC_ERR_INVALID_ARG); /* once */
  mc_plugin_list *l = NULL;
  CHECK(mc_plugins_list(&l) == MC_OK);
  CHECK(l && l->count == 3);
  const mc_plugin_info *folder = find(l, "kith_test_folder");
  CHECK(folder && folder->state == MC_PLUGIN_OFF && !folder->enabled && !folder->id &&
        !folder->version);
  CHECK(folder && !strcmp(folder->name, "kith_test_folder") && folder->path &&
        strstr(folder->path, "kith_test_folder"));
  mc_plugin_list_free(l);
  mc_plugin_encoder enc;
  CHECK(mc_plugins_find_encoder("wav", &enc) == MC_ERR_NOT_FOUND);
  CHECK(mc_plugins_targets(NULL, 0) == 0);
  CHECK(mc_plugins_set_enabled("nonesuch", 1) == MC_ERR_NOT_FOUND);

  /* On: loaded at once, and remembered. */
  CHECK(mc_plugins_set_enabled("kith_test_folder", 1) == MC_OK);
  CHECK(mc_plugins_set_enabled("kith_test_badabi", 1) == MC_OK);
  CHECK(mc_plugins_set_enabled("kith_test_null", 1) == MC_OK);
  CHECK(mc_plugins_list(&l) == MC_OK);
  folder = find(l, "kith_test_folder");
  CHECK(folder && folder->state == MC_PLUGIN_LOADED && folder->enabled && !folder->reason);
  CHECK(folder && folder->id && !strcmp(folder->id, "ng.kithso.test.folder"));
  CHECK(folder && !strcmp(folder->name, "Test folder") && folder->version &&
        !strcmp(folder->version, "1.2.3"));
  CHECK(folder && folder->device_targets == 1 && folder->encoders == 1);
  const mc_plugin_info *bad = find(l, "kith_test_badabi");
  CHECK(bad && bad->state == MC_PLUGIN_REFUSED && bad->reason &&
        !strcmp(bad->reason, mc_str(MC_STR_PLUGIN_REFUSED_ABI)));
  const mc_plugin_info *null = find(l, "kith_test_null");
  CHECK(null && null->state == MC_PLUGIN_REFUSED && null->reason &&
        !strcmp(null->reason, mc_str(MC_STR_PLUGIN_REFUSED_INIT)));
  mc_plugin_list_free(l);
  CHECK(mc_settings_plugin_enabled(s, "kith_test_folder"));
  CHECK(mc_settings_plugin_known(s, "kith_test_folder", "id") &&
        !strcmp(mc_settings_plugin_known(s, "kith_test_folder", "id"), "ng.kithso.test.folder"));
  CHECK(mc_plugins_find_encoder("wav", &enc) == MC_OK && enc.format && enc.format->lossless);
  CHECK(mc_plugins_find_encoder("lofi", &enc) == MC_OK && enc.format &&
        enc.format->setting_count == 2);
  mc_plugin_target targets[4];
  CHECK(mc_plugins_targets(targets, 4) == 1 && !strcmp(targets[0].target->id, "folder"));

  /* Options: declared, defaulted, checked and stored. */
  mc_plugin_option_list *opts = NULL;
  CHECK(mc_plugins_options("kith_test_folder", &opts) == MC_OK);
  CHECK(opts && opts->count == 3);
  if (opts && opts->count == 3) {
    CHECK(!strcmp(opts->items[0].key, "fast") && opts->items[0].kind == MC_PLUGIN_OPTION_SWITCH &&
          !strcmp(opts->items[0].value, "0"));
    CHECK(opts->items[1].kind == MC_PLUGIN_OPTION_CHOICE && opts->items[1].choice_count == 2 &&
          !strcmp(opts->items[1].choice_labels[1], "Choice B") &&
          !strcmp(opts->items[1].description, "How it works"));
    CHECK(opts->items[2].min == 1 && opts->items[2].max == 10);
  }
  mc_plugin_option_list_free(opts);
  CHECK(mc_plugins_options("kith_test_badabi", &opts) == MC_ERR_NOT_FOUND);
  CHECK(mc_plugins_set_option("kith_test_folder", "mode", "b") == MC_OK);
  CHECK(mc_plugins_set_option("kith_test_folder", "mode", "c") == MC_ERR_INVALID_ARG);
  CHECK(mc_plugins_set_option("kith_test_folder", "level", "11") == MC_ERR_INVALID_ARG);
  CHECK(mc_plugins_set_option("kith_test_folder", "level", "7") == MC_OK);
  CHECK(mc_plugins_set_option("kith_test_folder", "nope", "1") == MC_ERR_INVALID_ARG);
  CHECK(!strcmp(stored(s, "ng.kithso.test.folder", "mode", &v), "b"));
  CHECK(mc_plugins_options("kith_test_folder", &opts) == MC_OK);
  CHECK(opts && opts->count == 3 && !strcmp(opts->items[1].value, "b"));
  mc_plugin_option_list_free(opts);

  /* The plugin's own setting (its init counts its starts) waits for a flush. */
  CHECK(!strcmp(stored(s, "ng.kithso.test.folder", "runs", &v), ""));
  CHECK(mc_plugins_flush_settings() == 1);
  CHECK(mc_plugins_flush_settings() == 0);
  CHECK(!strcmp(stored(s, "ng.kithso.test.folder", "runs", &v), "1"));

  /* Off: unused at once, unloaded at the next start. */
  CHECK(mc_plugins_set_enabled("kith_test_null", 0) == MC_OK);
  CHECK(mc_plugins_set_enabled("kith_test_folder", 0) == MC_OK);
  CHECK(mc_plugins_list(&l) == MC_OK);
  folder = find(l, "kith_test_folder");
  CHECK(folder && folder->state == MC_PLUGIN_UNLOADING && !folder->enabled);
  null = find(l, "kith_test_null");
  CHECK(null && null->state == MC_PLUGIN_OFF);
  mc_plugin_list_free(l);
  CHECK(mc_plugins_find_encoder("wav", &enc) == MC_ERR_NOT_FOUND);
  CHECK(mc_plugins_targets(NULL, 0) == 0);
  /* Back on before the restart: the same library, not loaded twice. */
  CHECK(mc_plugins_set_enabled("kith_test_folder", 1) == MC_OK);
  CHECK(mc_plugins_find_encoder("wav", &enc) == MC_OK);
  CHECK(mc_plugins_flush_settings() == 0); /* init didn't run again */
  CHECK(mc_settings_save(s) == MC_OK);

  /* The plugins section copied into settings loaded afresh, the rest of them kept. */
  {
    mc_settings *fresh = NULL;
    CHECK(mc_settings_load(cfg, &fresh) == MC_OK);
    CHECK(mc_settings_set_volume(fresh, 42) == MC_OK);
    CHECK(mc_plugins_set_option("kith_test_folder", "level", "3") == MC_OK); /* in s only */
    CHECK(!strcmp(stored(fresh, "ng.kithso.test.folder", "level", &v), "7"));
    CHECK(mc_settings_copy_plugins(fresh, s) == MC_OK);
    CHECK(mc_settings_volume(fresh) == 42);
    CHECK(mc_settings_plugin_enabled(fresh, "kith_test_folder"));
    CHECK(!strcmp(stored(fresh, "ng.kithso.test.folder", "level", &v), "3"));
    CHECK(mc_settings_copy_plugins(NULL, s) == MC_ERR_INVALID_ARG);
    mc_settings_free(fresh);
    CHECK(mc_plugins_set_option("kith_test_folder", "level", "7") == MC_OK); /* as before */
  }
  mc_plugins_unload();
  mc_settings_free(s);

  /* The next start loads what's on, from the saved settings, with its stored values. */
  CHECK(mc_settings_load(cfg, &s) == MC_OK);
  CHECK(mc_plugins_load(s, dirs, 1) == MC_OK);
  CHECK(mc_plugins_list(&l) == MC_OK);
  folder = find(l, "kith_test_folder");
  CHECK(folder && folder->state == MC_PLUGIN_LOADED);
  bad = find(l, "kith_test_badabi");
  CHECK(bad && bad->state == MC_PLUGIN_REFUSED);
  mc_plugin_list_free(l);
  CHECK(mc_plugins_flush_settings() == 1);
  CHECK(!strcmp(stored(s, "ng.kithso.test.folder", "runs", &v), "2"));
  CHECK(mc_plugins_options("kith_test_folder", &opts) == MC_OK);
  CHECK(opts && opts->count == 3 && !strcmp(opts->items[2].value, "7"));
  mc_plugin_option_list_free(opts);

  /* And what's off isn't loaded at all: its init doesn't run. */
  CHECK(mc_plugins_set_enabled("kith_test_folder", 0) == MC_OK);
  CHECK(mc_plugins_set_enabled("kith_test_badabi", 0) == MC_OK);
  CHECK(mc_settings_save(s) == MC_OK);
  mc_plugins_unload();
  mc_settings_free(s);
  CHECK(mc_settings_load(cfg, &s) == MC_OK);
  CHECK(mc_plugins_load(s, dirs, 1) == MC_OK);
  CHECK(mc_plugins_list(&l) == MC_OK);
  CHECK(l && l->count == 3);
  for (int i = 0; l && i < l->count; i++) CHECK(l->items[i].state == MC_PLUGIN_OFF);
  folder = find(l, "kith_test_folder");
  /* Still shown by the name it gave when it was last loaded. */
  CHECK(folder && !strcmp(folder->name, "Test folder") && folder->version &&
        !strcmp(folder->version, "1.2.3"));
  mc_plugin_list_free(l);
  CHECK(mc_plugins_flush_settings() == 0);
  CHECK(!strcmp(stored(s, "ng.kithso.test.folder", "runs", &v), "2"));
  mc_plugins_unload();
  mc_settings_free(s);

  /* A folder with no plugins in it finds none. */
  CHECK(mc_settings_load(cfg, &s) == MC_OK);
  const char *none[] = {cfg};
  CHECK(mc_plugins_load(s, none, 1) == MC_OK);
  CHECK(mc_plugins_list(&l) == MC_OK);
  CHECK(l && l->count == 0);
  mc_plugin_list_free(l);
  mc_plugins_unload();
  mc_settings_free(s);
  mc_remove_tree(cfg);
  return TEST_RESULT();
}

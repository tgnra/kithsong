/* The plugin loader and registry: kithcore/plugins.h for frontends, plugins_internal.h for
 * the rest of core. See "Loading" in docs/plugins.md.
 *
 * One process-wide list of the plugin files found, under one lock. A loaded plugin stays
 * loaded until mc_plugins_unload(), even once turned off (it's only skipped), so the tables
 * it handed out stay valid for whoever holds them. Each plugin gets a host table of its
 * own, inside its entry, so host calls that need to know who's calling can find the entry
 * from the table. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* dlopen, strdup's kin */
#endif

#include "kithcore/plugins.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plugin/plugins_internal.h"
#include "settings_internal.h"
#include "util/platform.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define PLUGIN_EXT ".dll"
#define LOADING 1
#elif defined(__ANDROID__)
#define LOADING 0 /* no native code from outside the APK */
#else
#include <dlfcn.h>
#define PLUGIN_EXT ".so"
#define LOADING 1
#endif

typedef struct kv {
  char *key, *value;
  int dirty; /* set by the plugin, not yet in the settings */
} kv;

typedef struct entry {
  char *file_id, *path;
  int enabled;
  mc_plugin_state state;
  char *reason;
  void *handle;
  const kith_plugin *plugin;
  kith_host host; /* this plugin's own */
  kv *values;
  int nvalues, values_cap;
} entry;

static struct {
  mc_mutex lock;
  int lock_ready;
  int loaded; /* mc_plugins_load() has run */
  mc_settings *settings;
  entry **items;
  int count, cap;
} g;

static void lock(void) {
  /* The first call is mc_plugins_load()'s, on the UI thread, before any other. */
  if (!g.lock_ready) {
    mc_mutex_init(&g.lock);
    g.lock_ready = 1;
  }
  mc_mutex_lock(&g.lock);
}

static void unlock(void) {
  mc_mutex_unlock(&g.lock);
}

static entry *entry_of_host(const kith_host *host) {
  return host ? (entry *)((char *)host - offsetof(entry, host)) : NULL;
}

/* --- Host functions --- */

static void host_log(const kith_host *self, kith_log_level level, const char *message) {
  static const char *const names[] = {"debug", "info", "warning", "error"};
  entry *e = entry_of_host(self);
  const char *who = e && e->plugin && e->plugin->id ? e->plugin->id : e ? e->file_id : "?";
  const char *lvl = (int)level >= 0 && (int)level < 4 ? names[level] : "info";
  fprintf(stderr, "kithcore: plugin %s: %s: %s\n", who, lvl, message ? message : "");
}

static void host_free(void *ptr) {
  free(ptr);
}

static kv *find_kv(entry *e, const char *key) {
  for (int i = 0; i < e->nvalues; i++)
    if (!strcmp(e->values[i].key, key)) return &e->values[i];
  return NULL;
}

/* Sets key to value (NULL clears it) in e's values. Under lock. */
static mc_status put_kv(entry *e, const char *key, const char *value, int dirty) {
  kv *v = find_kv(e, key);
  if (!v) {
    if (e->nvalues == e->values_cap) {
      int cap = e->values_cap ? e->values_cap * 2 : 8;
      kv *more = realloc(e->values, (size_t)cap * sizeof *more);
      if (!more) return MC_ERR_NOMEM;
      e->values = more;
      e->values_cap = cap;
    }
    v = &e->values[e->nvalues];
    memset(v, 0, sizeof *v);
    if (!(v->key = mc_strdup(key))) return MC_ERR_NOMEM;
    e->nvalues++;
  }
  char *copy = value ? mc_strdup(value) : NULL;
  if (value && !copy) return MC_ERR_NOMEM;
  free(v->value);
  v->value = copy;
  v->dirty = dirty;
  return MC_OK;
}

static char *host_settings_get(const kith_host *self, const char *key) {
  entry *e = entry_of_host(self);
  if (!e || !key) return NULL;
  lock();
  kv *v = find_kv(e, key);
  char *out = v && v->value ? mc_strdup(v->value) : NULL;
  unlock();
  return out;
}

static int host_settings_set(const kith_host *self, const char *key, const char *value) {
  entry *e = entry_of_host(self);
  if (!e || !key || !*key) return KITH_ERR_INVALID_ARG;
  lock();
  mc_status st = put_kv(e, key, value, 1);
  unlock();
  return st == MC_OK ? KITH_OK : KITH_ERR_NOMEM;
}

static const char *host_string(const char *key) {
  if (key && !strcmp(key, "language")) return mc_strings_locale();
  return NULL;
}

int mc_plugin_job_cancelled(const kith_job *job) {
  return job && job->stop && job->stop(job->user);
}

static void fill_host(entry *e) {
  kith_host *h = &e->host;
  memset(h, 0, sizeof *h);
  h->abi = KITH_PLUGIN_ABI;
  h->size = (int)sizeof *h;
  h->log = host_log;
  h->free = host_free;
  h->decode_open = mc_source_open;
  h->decode_info = mc_source_info;
  h->decode_set_format = mc_source_set_format;
  h->decode_read = mc_source_read;
  h->decode_close = mc_source_close;
  h->decode_tags = mc_source_tags;
  h->decode_cover = mc_source_cover;
  h->settings_get = host_settings_get;
  h->settings_set = host_settings_set;
  h->string = host_string;
  h->cancelled = mc_plugin_job_cancelled;
}

/* --- Finding --- */

static void entry_free(entry *e) {
  if (!e) return;
  for (int i = 0; i < e->nvalues; i++) {
    free(e->values[i].key);
    free(e->values[i].value);
  }
  free(e->values);
  free(e->file_id);
  free(e->path);
  free(e->reason);
  free(e);
}

static entry *find_entry(const char *file_id) {
  for (int i = 0; i < g.count; i++)
    if (!strcmp(g.items[i]->file_id, file_id)) return g.items[i];
  return NULL;
}

typedef struct scan {
  const char *dir;
  int oom;
} scan;

#if LOADING
static int ends_with_ci(const char *s, const char *suffix) {
  size_t ls = strlen(s), lx = strlen(suffix);
  if (ls <= lx) return 0;
  for (size_t i = 0; i < lx; i++)
    if ((s[ls - lx + i] | 0x20) != (suffix[i] | 0x20)) return 0;
  return 1;
}

static void on_file(void *user, const char *name, int is_dir) {
  scan *s = user;
  if (is_dir || !ends_with_ci(name, PLUGIN_EXT)) return;
  size_t stem = strlen(name) - strlen(PLUGIN_EXT);
  char *file_id = malloc(stem + 1);
  if (!file_id) {
    s->oom = 1;
    return;
  }
  memcpy(file_id, name, stem);
  file_id[stem] = '\0';
  if (find_entry(file_id)) { /* the first folder's wins */
    free(file_id);
    return;
  }
  size_t n = strlen(s->dir) + strlen(name) + 2;
  entry *e = calloc(1, sizeof *e);
  char *path = malloc(n);
  if (!e || !path || (g.count == g.cap && !(g.cap = g.cap ? g.cap * 2 : 8))) {
    free(e);
    free(path);
    free(file_id);
    s->oom = 1;
    return;
  }
  entry **more = realloc(g.items, (size_t)g.cap * sizeof *more);
  if (!more) {
    free(e);
    free(path);
    free(file_id);
    s->oom = 1;
    return;
  }
  g.items = more;
  snprintf(path, n, "%s%c%s", s->dir, MC_PATH_SEP, name);
  e->file_id = file_id;
  e->path = mc_full_path(path);
  if (!e->path) e->path = path;
  else free(path);
  e->state = MC_PLUGIN_OFF;
  g.items[g.count++] = e;
}
#endif

/* dir/sub, malloc'd. */
static char *join(const char *dir, const char *sub) {
  if (!dir) return NULL;
  size_t n = strlen(dir) + strlen(sub) + 2;
  char *p = malloc(n);
  if (p) snprintf(p, n, "%s%c%s", dir, MC_PATH_SEP, sub);
  return p;
}

/* The folder holding path, malloc'd. */
static char *dir_of(const char *path) {
  if (!path) return NULL;
  const char *a = strrchr(path, '/'), *b = strrchr(path, MC_PATH_SEP);
  const char *sep = a > b ? a : b;
  if (!sep) return NULL;
  size_t len = (size_t)(sep - path);
  char *d = malloc(len + 1);
  if (!d) return NULL;
  memcpy(d, path, len);
  d[len] = '\0';
  return d;
}

/* The platform's plugin folders, malloc'd, into out (at most 4). */
static int default_dirs(char *out[4]) {
  int n = 0;
#if defined(_WIN32)
  char *exe = mc_exe_path(), *exe_dir = dir_of(exe);
  if (exe_dir && (out[n] = join(exe_dir, "plugins"))) n++;
  free(exe_dir);
  free(exe);
  const wchar_t *local = _wgetenv(L"LOCALAPPDATA");
  if (local && *local) {
    int len = WideCharToMultiByte(CP_UTF8, 0, local, -1, NULL, 0, NULL, NULL);
    char *utf8 = len > 0 ? malloc((size_t)len) : NULL;
    if (utf8 && WideCharToMultiByte(CP_UTF8, 0, local, -1, utf8, len, NULL, NULL) > 0 &&
        (out[n] = join(utf8, "Kithsong\\plugins")))
      n++;
    free(utf8);
  }
#elif LOADING
  char *exe = mc_exe_path(), *bin = dir_of(exe), *prefix = dir_of(bin);
  if (prefix && (out[n] = join(prefix, "lib/kithsong/plugins"))) n++;
  free(prefix);
  free(bin);
  free(exe);
  const char *data = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
  if (data && *data) {
    if ((out[n] = join(data, "kithsong/plugins"))) n++;
  } else if (home && *home) {
    if ((out[n] = join(home, ".local/share/kithsong/plugins"))) n++;
  }
  if (getenv("FLATPAK_ID") || mc_file_exists("/.flatpak-info"))
    if ((out[n] = mc_strdup("/app/plugins"))) n++;
#else
  (void)out;
  (void)join;
  (void)dir_of;
#endif
  return n;
}

/* --- Loading --- */

static void set_reason(entry *e, const char *reason) {
  free(e->reason);
  e->reason = reason ? mc_strdup(reason) : NULL;
}

static void take_value(void *user, const char *key, const char *value) {
  put_kv(user, key, value, 0);
}

/* Loads e, which is on. Under lock. */
static void load(entry *e) {
#if LOADING
  if (e->handle) { /* still loaded from before it was turned off */
    e->state = MC_PLUGIN_LOADED;
    return;
  }
  char sys[256] = "";
  void *handle = NULL;
#if defined(_WIN32)
  wchar_t *wide = mc_win_path(e->path);
  /* The plugin's own folder first, for libraries it brings. */
  if (wide)
    handle = LoadLibraryExW(wide, NULL,
                            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  if (!handle) snprintf(sys, sizeof sys, "error %lu", (unsigned long)GetLastError());
  free(wide);
#else
  handle = dlopen(e->path, RTLD_NOW | RTLD_LOCAL);
  if (!handle) {
    const char *why = dlerror();
    snprintf(sys, sizeof sys, "%s", why ? why : "dlopen failed");
  }
#endif
  if (!handle) {
    char reason[512];
    mc_format(reason, sizeof reason, mc_str(MC_STR_PLUGIN_REFUSED_LOAD), MC_S(sys));
    e->state = MC_PLUGIN_REFUSED;
    set_reason(e, reason);
    fprintf(stderr, "kithcore: plugin %s: %s\n", e->path, reason);
    return;
  }
  kith_plugin_init_fn init;
#if defined(_WIN32)
  FARPROC proc = GetProcAddress((HMODULE)handle, KITH_PLUGIN_INIT_SYMBOL);
  memcpy(&init, &proc, sizeof init);
#else
  void *sym = dlsym(handle, KITH_PLUGIN_INIT_SYMBOL);
  memcpy(&init, &sym, sizeof init);
#endif
  const char *refused = NULL;
  const kith_plugin *p = NULL;
  if (!init) {
    refused = mc_str(MC_STR_PLUGIN_REFUSED_ENTRY);
  } else {
    fill_host(e);
    p = init(&e->host);
    if (!p) refused = mc_str(MC_STR_PLUGIN_REFUSED_INIT);
    else if (p->abi != KITH_PLUGIN_ABI || p->size != (int)sizeof *p)
      refused = mc_str(MC_STR_PLUGIN_REFUSED_ABI);
    else if (!p->id || !*p->id || !p->name || !p->free || p->device_target_count < 0 ||
             p->encoder_count < 0 || (p->device_target_count && !p->device_targets) ||
             (p->encoder_count && !p->encoders) || p->option_count < 0 ||
             (p->option_count && !p->options))
      refused = mc_str(MC_STR_PLUGIN_REFUSED_INIT);
  }
  if (refused) {
    e->state = MC_PLUGIN_REFUSED;
    set_reason(e, refused);
    fprintf(stderr, "kithcore: plugin %s: %s\n", e->path, refused);
#if defined(_WIN32)
    FreeLibrary((HMODULE)handle);
#else
    dlclose(handle);
#endif
    return;
  }
  e->handle = handle;
  e->plugin = p;
  e->state = MC_PLUGIN_LOADED;
  set_reason(e, NULL);
  if (g.settings) {
    mc_settings_set_plugin_known(g.settings, e->file_id, p->id, p->name, p->version);
    mc_settings_plugin_values(g.settings, p->id, take_value, e);
  }
#else
  e->state = MC_PLUGIN_OFF;
#endif
}

static void unload(entry *e) {
#if LOADING
  if (!e->handle) return;
  if (e->plugin && e->plugin->shutdown) e->plugin->shutdown();
#if defined(_WIN32)
  FreeLibrary((HMODULE)e->handle);
#else
  dlclose(e->handle);
#endif
  e->handle = NULL;
  e->plugin = NULL;
#else
  (void)e;
#endif
}

static int by_file_id(const void *a, const void *b) {
  return strcmp((*(entry *const *)a)->file_id, (*(entry *const *)b)->file_id);
}

mc_status mc_plugins_load(mc_settings *settings, const char *const *dirs, int dir_count) {
  if (dir_count < 0 || (dir_count && !dirs)) return MC_ERR_INVALID_ARG;
  lock();
  if (g.loaded) {
    unlock();
    return MC_ERR_INVALID_ARG;
  }
  g.loaded = 1;
  g.settings = settings;
  mc_status st = MC_OK;
#if LOADING
  char *defaults[4] = {0};
  int ndefault = dirs ? 0 : default_dirs(defaults);
  int n = dirs ? dir_count : ndefault;
  for (int i = 0; i < n && st == MC_OK; i++) {
    scan s = {dirs ? dirs[i] : defaults[i], 0};
    if (s.dir && mc_file_exists(s.dir)) mc_list_dir(s.dir, on_file, &s);
    if (s.oom) st = MC_ERR_NOMEM;
  }
  for (int i = 0; i < ndefault; i++) free(defaults[i]);
  if (g.count > 1) qsort(g.items, (size_t)g.count, sizeof *g.items, by_file_id);
  for (int i = 0; i < g.count; i++) {
    entry *e = g.items[i];
    e->enabled = mc_settings_plugin_enabled(settings, e->file_id);
    if (e->enabled) load(e);
  }
#else
  (void)default_dirs;
#endif
  unlock();
  return st;
}

void mc_plugins_unload(void) {
  if (!g.lock_ready) return;
  lock();
  for (int i = 0; i < g.count; i++) {
    unload(g.items[i]);
    entry_free(g.items[i]);
  }
  free(g.items);
  g.items = NULL;
  g.count = g.cap = 0;
  g.loaded = 0;
  g.settings = NULL;
  unlock();
}

/* --- Listing --- */

void mc_plugin_list_free(mc_plugin_list *list) {
  if (!list) return;
  for (int i = 0; i < list->count; i++) {
    mc_plugin_info *it = &list->items[i];
    free(it->file_id);
    free(it->id);
    free(it->name);
    free(it->version);
    free(it->path);
    free(it->reason);
  }
  free(list->items);
  free(list);
}

static int dup_or_null(char **out, const char *s) {
  *out = s ? mc_strdup(s) : NULL;
  return !s || *out;
}

static int by_name(const void *a, const void *b) {
  const mc_plugin_info *x = a, *y = b;
  int c = strcmp(x->name, y->name);
  return c ? c : strcmp(x->file_id, y->file_id);
}

mc_status mc_plugins_list(mc_plugin_list **out) {
  if (!out) return MC_ERR_INVALID_ARG;
  *out = NULL;
  mc_plugin_list *list = calloc(1, sizeof *list);
  if (!list) return MC_ERR_NOMEM;
  lock();
  mc_status st = MC_OK;
  if (g.count && !(list->items = calloc((size_t)g.count, sizeof *list->items))) st = MC_ERR_NOMEM;
  for (int i = 0; st == MC_OK && i < g.count; i++) {
    entry *e = g.items[i];
    mc_plugin_info *it = &list->items[list->count++];
    const kith_plugin *p = e->plugin;
    const char *id = p ? p->id : mc_settings_plugin_known(g.settings, e->file_id, "id");
    const char *name = p ? p->name : mc_settings_plugin_known(g.settings, e->file_id, "name");
    const char *version = p ? p->version : mc_settings_plugin_known(g.settings, e->file_id, "version");
    it->state = e->state;
    it->enabled = e->enabled;
    if (p) {
      it->device_targets = p->device_target_count;
      it->encoders = p->encoder_count;
    }
    if (!dup_or_null(&it->file_id, e->file_id) || !dup_or_null(&it->id, id) ||
        !dup_or_null(&it->name, name && *name ? name : e->file_id) ||
        !dup_or_null(&it->version, version) || !dup_or_null(&it->path, e->path) ||
        !dup_or_null(&it->reason, e->state == MC_PLUGIN_REFUSED ? e->reason : NULL))
      st = MC_ERR_NOMEM;
  }
  unlock();
  if (st != MC_OK) {
    mc_plugin_list_free(list);
    return st;
  }
  if (list->count > 1) qsort(list->items, (size_t)list->count, sizeof *list->items, by_name);
  *out = list;
  return MC_OK;
}

mc_status mc_plugins_set_enabled(const char *file_id, int on) {
  if (!file_id) return MC_ERR_INVALID_ARG;
  lock();
  entry *e = find_entry(file_id);
  mc_status st = MC_ERR_NOT_FOUND;
  if (e) {
    st = g.settings ? mc_settings_set_plugin_enabled(g.settings, file_id, on) : MC_OK;
    if (st == MC_OK) {
      e->enabled = on != 0;
      if (on) load(e);
      else e->state = e->handle ? MC_PLUGIN_UNLOADING : MC_PLUGIN_OFF;
    }
  }
  unlock();
  return st;
}

/* --- Options --- */

static void option_free(mc_plugin_option *o) {
  free(o->key);
  free(o->label);
  free(o->description);
  free(o->value);
  for (int i = 0; i < o->choice_count; i++) {
    if (o->choice_ids) free(o->choice_ids[i]);
    if (o->choice_labels) free(o->choice_labels[i]);
  }
  free(o->choice_ids);
  free(o->choice_labels);
}

void mc_plugin_option_list_free(mc_plugin_option_list *list) {
  if (!list) return;
  for (int i = 0; i < list->count; i++) option_free(&list->items[i]);
  free(list->items);
  free(list);
}

/* The loaded and on entry for file_id. Under lock. */
static entry *running(const char *file_id) {
  entry *e = file_id ? find_entry(file_id) : NULL;
  return e && e->plugin && e->state == MC_PLUGIN_LOADED ? e : NULL;
}

mc_status mc_plugins_options(const char *file_id, mc_plugin_option_list **out) {
  if (!out) return MC_ERR_INVALID_ARG;
  *out = NULL;
  lock();
  entry *e = running(file_id);
  if (!e) {
    unlock();
    return MC_ERR_NOT_FOUND;
  }
  const kith_plugin *p = e->plugin;
  mc_plugin_option_list *list = calloc(1, sizeof *list);
  mc_status st = list ? MC_OK : MC_ERR_NOMEM;
  if (st == MC_OK && p->option_count &&
      !(list->items = calloc((size_t)p->option_count, sizeof *list->items)))
    st = MC_ERR_NOMEM;
  for (int i = 0; st == MC_OK && i < p->option_count; i++) {
    const kith_option *k = &p->options[i];
    mc_plugin_option *o = &list->items[list->count++];
    kv *v = k->key ? find_kv(e, k->key) : NULL;
    o->kind = (mc_plugin_option_kind)k->kind;
    o->min = k->min;
    o->max = k->max;
    if (!dup_or_null(&o->key, k->key) || !dup_or_null(&o->label, k->label) ||
        !dup_or_null(&o->description, k->description) ||
        !dup_or_null(&o->value, v && v->value ? v->value : k->def)) {
      st = MC_ERR_NOMEM;
      break;
    }
    if (k->kind == KITH_OPTION_CHOICE && k->choice_count > 0 && k->choices) {
      o->choice_ids = calloc((size_t)k->choice_count, sizeof *o->choice_ids);
      o->choice_labels = calloc((size_t)k->choice_count, sizeof *o->choice_labels);
      if (!o->choice_ids || !o->choice_labels) {
        st = MC_ERR_NOMEM;
        break;
      }
      o->choice_count = k->choice_count;
      for (int c = 0; c < k->choice_count; c++)
        if (!dup_or_null(&o->choice_ids[c], k->choices[c].id) ||
            !dup_or_null(&o->choice_labels[c], k->choices[c].label))
          st = MC_ERR_NOMEM;
    }
  }
  unlock();
  if (st != MC_OK) {
    mc_plugin_option_list_free(list);
    return st;
  }
  *out = list;
  return MC_OK;
}

/* Whether value suits option k. */
static int option_takes(const kith_option *k, const char *value) {
  if (!value) return 1; /* back to the default */
  switch (k->kind) {
    case KITH_OPTION_SWITCH: return !strcmp(value, "0") || !strcmp(value, "1");
    case KITH_OPTION_CHOICE:
      for (int c = 0; k->choices && c < k->choice_count; c++)
        if (k->choices[c].id && !strcmp(k->choices[c].id, value)) return 1;
      return 0;
    case KITH_OPTION_NUMBER: {
      char *end = NULL;
      long long n = strtoll(value, &end, 10);
      return *value && end && !*end && n >= k->min && n <= k->max;
    }
  }
  return 0;
}

mc_status mc_plugins_set_option(const char *file_id, const char *key, const char *value) {
  if (!key) return MC_ERR_INVALID_ARG;
  lock();
  entry *e = running(file_id);
  mc_status st = MC_ERR_NOT_FOUND;
  if (e) {
    const kith_option *k = NULL;
    for (int i = 0; i < e->plugin->option_count; i++)
      if (e->plugin->options[i].key && !strcmp(e->plugin->options[i].key, key))
        k = &e->plugin->options[i];
    st = k && option_takes(k, value) ? MC_OK : MC_ERR_INVALID_ARG;
    if (st == MC_OK) st = put_kv(e, key, value, 0);
    if (st == MC_OK && g.settings)
      st = mc_settings_set_plugin_value(g.settings, e->plugin->id, key, value);
  }
  unlock();
  return st;
}

int mc_plugins_flush_settings(void) {
  if (!g.lock_ready) return 0;
  int changed = 0;
  lock();
  for (int i = 0; g.settings && i < g.count; i++) {
    entry *e = g.items[i];
    for (int j = 0; e->plugin && j < e->nvalues; j++) {
      kv *v = &e->values[j];
      if (!v->dirty) continue;
      if (mc_settings_set_plugin_value(g.settings, e->plugin->id, v->key, v->value) == MC_OK) {
        v->dirty = 0;
        changed = 1;
      }
    }
  }
  unlock();
  return changed;
}

/* --- For core --- */

mc_status mc_plugins_find_encoder(const char *format_id, mc_plugin_encoder *out) {
  if (!format_id || !out) return MC_ERR_INVALID_ARG;
  memset(out, 0, sizeof *out);
  if (!g.lock_ready) return MC_ERR_NOT_FOUND;
  mc_status st = MC_ERR_NOT_FOUND;
  lock();
  for (int i = 0; st != MC_OK && i < g.count; i++) {
    entry *e = g.items[i];
    if (!e->plugin || e->state != MC_PLUGIN_LOADED) continue;
    for (int j = 0; st != MC_OK && j < e->plugin->encoder_count; j++) {
      const kith_encoder *enc = &e->plugin->encoders[j];
      for (int f = 0; enc->encode && f < enc->format_count; f++)
        if (enc->formats[f].id && !strcmp(enc->formats[f].id, format_id)) {
          out->plugin = e->plugin;
          out->encoder = enc;
          out->format = &enc->formats[f];
          st = MC_OK;
          break;
        }
    }
  }
  unlock();
  return st;
}

int mc_plugins_targets(mc_plugin_target *out, int max) {
  if (!g.lock_ready) return 0;
  int n = 0;
  lock();
  for (int i = 0; i < g.count; i++) {
    entry *e = g.items[i];
    if (!e->plugin || e->state != MC_PLUGIN_LOADED) continue;
    for (int j = 0; j < e->plugin->device_target_count; j++) {
      if (out && n < max) {
        out[n].plugin = e->plugin;
        out[n].target = &e->plugin->device_targets[j];
      }
      n++;
    }
  }
  unlock();
  return n;
}

mc_status mc_plugins_find_target(const char *plugin_id, const char *target_id,
                                 mc_plugin_target *out) {
  if (!plugin_id || !target_id || !out) return MC_ERR_INVALID_ARG;
  memset(out, 0, sizeof *out);
  if (!g.lock_ready) return MC_ERR_NOT_FOUND;
  mc_status st = MC_ERR_NOT_FOUND;
  lock();
  for (int i = 0; st != MC_OK && i < g.count; i++) {
    entry *e = g.items[i];
    if (!e->plugin || e->state != MC_PLUGIN_LOADED || strcmp(e->plugin->id, plugin_id)) continue;
    for (int j = 0; j < e->plugin->device_target_count; j++) {
      const kith_device_target *t = &e->plugin->device_targets[j];
      if (t->id && !strcmp(t->id, target_id)) {
        out->plugin = e->plugin;
        out->target = t;
        st = MC_OK;
        break;
      }
    }
  }
  unlock();
  return st;
}

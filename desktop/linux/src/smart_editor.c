#include "smart_editor.h"

#include <string.h>
#include <yyjson.h>

#if !ADW_CHECK_VERSION(1, 5, 0)
#error "smart_editor.c needs libadwaita 1.5 or later (AdwDialog)"
#endif

enum { PREVIEW_DELAY_MS = 250, PREVIEW_ROWS = 50, PROBLEM_ROWS = 4, MAX_SORTS = 8, MAX_NAME = 255 };

enum { MODE_ALL, MODE_ANY, MODE_NONE };
enum { UNIT_DAYS, UNIT_WEEKS, UNIT_MONTHS };
enum { PROBLEM_NONE, PROBLEM_VALUE, PROBLEM_NUMBER, PROBLEM_DURATION, PROBLEM_CHOICE };
enum { CHOICE_NONE = -1, CHOICE_EXTRA = -2 };

/* The formats a condition offers: the importer's file extensions (tagger.c), which is what
 * files.format holds. A rule naming another one keeps it as an extra entry. */
static const struct {
  const char *value, *label;
} k_formats[] = {
  {"flac", "FLAC"}, {"mp3", "MP3"}, {"opus", "Opus"}, {"wav", "WAV"}, {"aiff", "AIFF"}, {"aif", "AIF"},
};
#define FORMAT_COUNT ((int)G_N_ELEMENTS(k_formats))

/* --- The rule, as the editor holds it --- */

/* A group of conditions, or a condition. The rows and pages refer to one by its id, so a
 * widget outliving its node (being disposed of, say) finds nothing rather than freed memory. */
typedef struct node node;
struct node {
  guint id;
  node *parent; /* NULL for the list's own group */
  gboolean is_group;
  /* A group */
  int mode;
  char *name;     /* NULL or "" if it has none */
  GPtrArray *kids;
  char *desc;     /* mc_smart_describe() of it alone, as last worked out; NULL if none */
  gboolean bad;   /* something in it has a problem */
  /* A condition */
  int field, op;  /* into ed->fields, and that field's ops */
  char *value, *value2; /* a text, a number or a length as typed; value2 the high end of between */
  int unit;
  gint64 date;    /* before and after: local midnight, Unix ms */
  int choice;     /* Yes (1) or No (0); a playlist, group or format by index, or CHOICE_EXTRA */
  char *extra;    /* the choice's extra entry: a deleted playlist's or group's uid, or another format */
  int problem;
  /* What it matches on its own, as last counted, and the rule that was counted */
  int count;
  char *counted;
};

/* A row on a page, for a node. */
typedef struct row {
  guint id;
  GtkWidget *widget;
  GtkWidget *field, *op, *values, *hint; /* a condition's */
  GtkWidget *focus;                      /* the first thing to type in: its value, or its field */
  GtkWidget *warn;                       /* a group's */
  GtkWidget *count;
} row;

/* A page: a group's conditions. */
typedef struct page {
  guint group;
  AdwNavigationPage *nav;
  GtkWidget *name, *mode, *list, *placeholder;
  GtkWidget *add_condition;
  GtkWidget *preview_heading, *preview, *preview_note, *preview_more;
  GPtrArray *rows; /* of row, freed with the page */
  GtkWidget *focus; /* to have the focus once the page is shown */
  /* The list's page only */
  GtkWidget *sorts, *sort_add, *shuffled, *limited, *limit;
} page;

/* The thread's work: the rule without its limit, what the shown page previews, and the rule
 * for each row's count. */
typedef struct job {
  guint serial, gen;
  mc_smart_subject subject;
  char *list_rule;
  char *preview_rule; /* the list's rule on its own page, else the shown group's; may be NULL */
  gboolean preview_is_list;
  int n;
  guint *ids;
  char **rules;
} job;

typedef struct result {
  guint serial, gen;
  int total;       /* the list's, without its limit; -1 if it couldn't be worked out */
  gboolean preview_ok;
  mc_track_list *tracks;
  mc_album_list *albums;
  int n;
  guint *ids;
  char **rules;
  int *counts; /* -1 where a count wasn't made */
} result;

typedef struct worker {
  GMutex lock;
  GCond wake;
  job *job; /* the next one; a newer one replaces it */
  gboolean quit;
  char *dir;
} worker;

typedef struct editor {
  guint serial; /* tells this editor's results from an earlier one's */
  mc_library *lib;
  mc_smart_subject subject;
  gboolean creating, read_only;
  char uid[MC_UID_STRLEN];
  char *old_name, *old_rule;
  void (*done)(void *user, const char *uid);
  void *user;

  AdwDialog *dialog;
  AdwNavigationView *nav;
  GtkWidget *save, *cancel, *path;
  GtkWidget *problems, *problems_title, *problems_list, *problems_more;
  GtkWidget *status, *bottom;
  char *name; /* the list's, as typed */

  mc_smart_field fields[32];
  int nfields;
  int sortable[32]; /* the sortable fields, into fields */
  int nsortable;
  mc_playlist_list *playlists;
  mc_album_group_list *groups;

  node *root;
  GHashTable *nodes; /* id -> node */
  guint next_id;
  GArray *sorts; /* int: sortable index * 2 + desc */
  gboolean random, limited;
  gint64 seed;
  int limit;

  GPtrArray *bad; /* the conditions with problems, in the rule's order */
  worker *w;
  guint gen, timer;
  gboolean counted; /* a result has come back for the rule as it stands */
  int total;
  gboolean building; /* widgets being set up: their signals aren't edits */
} editor;

static editor *ed;
static guint editor_serial;

/* --- Little helpers --- */

static const mc_smart_field *field_of(const node *c) {
  return &ed->fields[c->field];
}

static const char *op_of(const node *c) {
  return field_of(c)->ops[c->op];
}

static gboolean type_is(const node *c, const char *type) {
  return !strcmp(field_of(c)->type, type);
}

static gboolean op_is(const node *c, const char *op) {
  return !strcmp(op_of(c), op);
}

static gboolean takes_no_value(const node *c) {
  return op_is(c, "empty") || op_is(c, "not_empty");
}

static gboolean is_choice(const node *c) {
  return type_is(c, "bool") || type_is(c, "playlist") || type_is(c, "group") || type_is(c, "format");
}

static int find_field(const char *name) {
  for (int i = 0; name && i < ed->nfields; i++)
    if (!strcmp(ed->fields[i].name, name)) return i;
  return -1;
}

static int find_op(int field, const char *op) {
  for (int i = 0; op && ed->fields[field].ops[i]; i++)
    if (!strcmp(ed->fields[field].ops[i], op)) return i;
  return -1;
}

/* A new condition's operator for its field's type: the one most rules want. */
static int default_op(int field) {
  const char *type = ed->fields[field].type;
  const char *op = !strcmp(type, "text")       ? "contains"
                   : !strcmp(type, "int")      ? "="
                   : !strcmp(type, "duration") ? ">"
                   : !strcmp(type, "date")     ? "within"
                                               : "is";
  int i = find_op(field, op);
  return i >= 0 ? i : 0;
}

/* An operator's label. A date's empty and not_empty read as never and any time: "Last played
 * is never". */
static const char *op_label(const char *type, const char *op) {
  if (!strcmp(type, "date") && !strcmp(op, "empty")) return mc_str(MC_STR_SMART_OP_NEVER);
  if (!strcmp(type, "date") && !strcmp(op, "not_empty")) return mc_str(MC_STR_SMART_OP_EVER);
  const char *label = mc_smart_op_label(type, op);
  return label ? label : op;
}

/* The field's name inside a sentence, for a negative text condition's hint. */
static enum mc_str_id in_text_id(const char *field) {
  static const struct {
    const char *name;
    enum mc_str_id id;
  } k[] = {
    {"title", MC_STR_SMART_FIELD_TITLE_IN_TEXT}, {"artist", MC_STR_SMART_FIELD_ARTIST_IN_TEXT},
    {"album_artist", MC_STR_SMART_FIELD_ALBUM_ARTIST_IN_TEXT}, {"album", MC_STR_SMART_FIELD_ALBUM_IN_TEXT},
    {"genre", MC_STR_SMART_FIELD_GENRE_IN_TEXT},
  };
  for (size_t i = 0; i < G_N_ELEMENTS(k); i++)
    if (!strcmp(k[i].name, field)) return k[i].id;
  return MC_STR_COUNT;
}

/* s without surrounding spaces, g_free'd by the caller. */
static char *trimmed(const char *s) {
  return g_strstrip(g_strdup(s ? s : ""));
}

/* A whole number of at most 12 digits, ignoring spaces around it. */
static gboolean parse_int(const char *text, gint64 *out) {
  char *s = trimmed(text);
  gboolean ok = *s && strlen(s) <= 12;
  gint64 n = 0;
  for (const char *p = s; ok && *p; p++) {
    ok = *p >= '0' && *p <= '9';
    n = n * 10 + (*p - '0');
  }
  g_free(s);
  if (ok) *out = n;
  return ok;
}

/* A length: m:ss or h:mm:ss, or a bare number of minutes. */
static gboolean parse_duration(const char *text, gint64 *out) {
  char *s = trimmed(text);
  gint64 parts[3] = {0};
  int n = 0, digits = 0;
  gboolean ok = *s;
  for (const char *p = s; ok && *p; p++) {
    if (*p == ':') {
      ok = digits && ++n < 3;
      digits = 0;
    } else {
      ok = *p >= '0' && *p <= '9' && digits < 6;
      if (ok) parts[n] = parts[n] * 10 + (*p - '0'), digits++;
    }
  }
  ok = ok && digits;
  g_free(s);
  if (!ok) return FALSE;
  if (n == 0) *out = parts[0] * 60;
  else if (n == 1 && parts[1] < 60) *out = parts[0] * 60 + parts[1];
  else if (n == 2 && parts[1] < 60 && parts[2] < 60) *out = parts[0] * 3600 + parts[1] * 60 + parts[2];
  else return FALSE;
  return TRUE;
}

static char *format_duration(gint64 s) {
  if (s >= 3600) return g_strdup_printf("%" G_GINT64_FORMAT ":%02d:%02d", s / 3600, (int)(s / 60 % 60), (int)(s % 60));
  return g_strdup_printf("%" G_GINT64_FORMAT ":%02d", s / 60, (int)(s % 60));
}

/* Today at local midnight, as Unix ms. */
static gint64 today(void) {
  GDateTime *now = g_date_time_new_now_local();
  GDateTime *day = g_date_time_new_local(g_date_time_get_year(now), g_date_time_get_month(now),
                                         g_date_time_get_day_of_month(now), 0, 0, 0);
  gint64 ms = g_date_time_to_unix(day) * 1000;
  g_date_time_unref(day);
  g_date_time_unref(now);
  return ms;
}

/* A date in the locale's own way of writing one. g_free it. */
static char *format_date(gint64 ms) {
  GDateTime *t = g_date_time_new_from_unix_local(ms / 1000);
  char *s = t ? g_date_time_format(t, "%x") : NULL;
  if (t) g_date_time_unref(t);
  return s ? s : g_strdup("");
}

static GtkWidget *flat_button(const char *icon, const char *tooltip) {
  GtkWidget *b = gtk_button_new_from_icon_name(icon);
  gtk_widget_set_tooltip_text(b, tooltip);
  gtk_widget_add_css_class(b, "flat");
  gtk_widget_add_css_class(b, "circular");
  gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
  return b;
}

/* A button with an icon and a label with a mnemonic. */
static GtkWidget *add_button(enum mc_str_id label) {
  GtkWidget *content = adw_button_content_new();
  adw_button_content_set_icon_name(ADW_BUTTON_CONTENT(content), "list-add-symbolic");
  adw_button_content_set_label(ADW_BUTTON_CONTENT(content), mc_str_mnemonic(label));
  adw_button_content_set_use_underline(ADW_BUTTON_CONTENT(content), TRUE);
  GtkWidget *b = gtk_button_new();
  gtk_button_set_child(GTK_BUTTON(b), content);
  return b;
}

static void set_label(GtkWidget *w, const char *text) {
  gtk_accessible_update_property(GTK_ACCESSIBLE(w), GTK_ACCESSIBLE_PROPERTY_LABEL, text, -1);
}

static GtkWidget *heading(const char *text) {
  GtkWidget *l = gtk_label_new(text);
  gtk_widget_add_css_class(l, "heading");
  gtk_label_set_xalign(GTK_LABEL(l), 0);
  gtk_widget_set_margin_top(l, 18);
  return l;
}

static GtkWidget *drop_down(const char *const *labels, const char *name) {
  GtkWidget *d = gtk_drop_down_new_from_strings(labels);
  gtk_widget_set_valign(d, GTK_ALIGN_CENTER);
  set_label(d, name);
  return d;
}

/* --- Nodes --- */

static node *node_new(node *parent, gboolean is_group) {
  node *n = g_new0(node, 1);
  n->id = ++ed->next_id;
  n->parent = parent;
  n->is_group = is_group;
  n->count = -1;
  n->choice = CHOICE_NONE;
  if (is_group) n->kids = g_ptr_array_new();
  g_hash_table_insert(ed->nodes, GUINT_TO_POINTER(n->id), n);
  if (parent) g_ptr_array_add(parent->kids, n);
  return n;
}

static void node_free(node *n) {
  if (!n) return;
  for (guint i = 0; n->kids && i < n->kids->len; i++) node_free(g_ptr_array_index(n->kids, i));
  if (n->kids) g_ptr_array_free(n->kids, TRUE);
  if (ed && ed->nodes) g_hash_table_remove(ed->nodes, GUINT_TO_POINTER(n->id));
  g_free(n->name);
  g_free(n->desc);
  g_free(n->value);
  g_free(n->value2);
  g_free(n->extra);
  g_free(n->counted);
  g_free(n);
}

static node *find(guint id) {
  return ed ? g_hash_table_lookup(ed->nodes, GUINT_TO_POINTER(id)) : NULL;
}

static node *kid(const node *g, guint i) {
  return g_ptr_array_index(g->kids, i);
}

static gboolean named(const node *g) {
  return g->name && *g->name;
}

/* The first choice for a condition's picker, or CHOICE_NONE if it has nothing to pick. */
static int first_choice(const node *c) {
  if (type_is(c, "bool")) return 1;
  if (type_is(c, "playlist")) return ed->playlists && ed->playlists->count ? 0 : CHOICE_NONE;
  if (type_is(c, "group")) return ed->groups && ed->groups->count ? 0 : CHOICE_NONE;
  return 0;
}

/* Values that suit c's field, after it's changed to another type. */
static void reset_values(node *c) {
  g_free(c->value);
  g_free(c->value2);
  g_free(c->extra);
  c->value = g_strdup(type_is(c, "date") ? "30" : "");
  c->value2 = g_strdup("");
  c->extra = NULL;
  c->unit = UNIT_DAYS;
  c->date = today();
  c->choice = first_choice(c);
}

/* A new condition: Genre for tracks, which most rules start from, else Artist. */
static node *cond_new(node *parent) {
  node *c = node_new(parent, FALSE);
  int f = find_field(ed->subject == MC_SMART_TRACKS ? "genre" : "artist");
  c->field = f >= 0 ? f : 0;
  c->op = default_op(c->field);
  reset_values(c);
  return c;
}

/* --- The rule as JSON --- */

/* c's value is missing or can't be read: why, or PROBLEM_NONE. */
static int cond_problem(const node *c) {
  if (takes_no_value(c)) return PROBLEM_NONE;
  gint64 n;
  if (type_is(c, "text")) {
    char *s = trimmed(c->value);
    int p = *s ? PROBLEM_NONE : PROBLEM_VALUE;
    g_free(s);
    return p;
  }
  if (type_is(c, "int") || type_is(c, "duration")) {
    gboolean dur = type_is(c, "duration");
    const char *vals[2] = {c->value, op_is(c, "between") ? c->value2 : NULL};
    for (int i = 0; i < 2; i++) {
      if (i && !vals[i]) break;
      char *s = trimmed(vals[i]);
      gboolean blank = !*s;
      g_free(s);
      if (blank) return PROBLEM_VALUE;
      if (!(dur ? parse_duration(vals[i], &n) : parse_int(vals[i], &n))) return dur ? PROBLEM_DURATION : PROBLEM_NUMBER;
    }
    return PROBLEM_NONE;
  }
  if (type_is(c, "date") && (op_is(c, "within") || op_is(c, "not_within"))) {
    char *s = trimmed(c->value);
    gboolean blank = !*s;
    g_free(s);
    if (blank) return PROBLEM_VALUE;
    static const int k_unit_days[] = {1, 7, 30};
    return parse_int(c->value, &n) && n * k_unit_days[c->unit] <= 1000000 ? PROBLEM_NONE : PROBLEM_NUMBER;
  }
  if (type_is(c, "date")) return PROBLEM_NONE;
  if (type_is(c, "bool")) return PROBLEM_NONE;
  return c->choice == CHOICE_NONE ? PROBLEM_CHOICE : PROBLEM_NONE;
}

/* c's condition, or NULL if it has a problem. */
static yyjson_mut_val *cond_json(yyjson_mut_doc *d, const node *c) {
  if (cond_problem(c)) return NULL;
  yyjson_mut_val *v = NULL;
  gint64 n, m;
  if (takes_no_value(c)) {
  } else if (type_is(c, "text")) {
    char *s = trimmed(c->value);
    v = yyjson_mut_strcpy(d, s);
    g_free(s);
  } else if (type_is(c, "int") || type_is(c, "duration")) {
    gboolean dur = type_is(c, "duration");
    if (dur) parse_duration(c->value, &n);
    else parse_int(c->value, &n);
    if (op_is(c, "between")) {
      if (dur) parse_duration(c->value2, &m);
      else parse_int(c->value2, &m);
      v = yyjson_mut_arr(d);
      yyjson_mut_arr_add_sint(d, v, n);
      yyjson_mut_arr_add_sint(d, v, m);
    } else {
      v = yyjson_mut_sint(d, n);
    }
  } else if (type_is(c, "date") && (op_is(c, "within") || op_is(c, "not_within"))) {
    static const int k_unit_days[] = {1, 7, 30};
    parse_int(c->value, &n);
    v = yyjson_mut_obj(d);
    yyjson_mut_obj_add_sint(d, v, "days", n * k_unit_days[c->unit]);
  } else if (type_is(c, "date")) {
    v = yyjson_mut_sint(d, c->date);
  } else if (type_is(c, "bool")) {
    v = yyjson_mut_bool(d, c->choice != 0);
  } else {
    int i = c->choice;
    const char *s = i == CHOICE_EXTRA          ? c->extra
                    : i < 0                    ? NULL
                    : type_is(c, "playlist")   ? (i < ed->playlists->count ? ed->playlists->items[i].uid : NULL)
                    : type_is(c, "group")      ? (i < ed->groups->count ? ed->groups->items[i].uid : NULL)
                    : i < FORMAT_COUNT         ? k_formats[i].value
                                               : NULL;
    if (!s) return NULL;
    v = yyjson_mut_strcpy(d, s);
  }
  yyjson_mut_val *obj = yyjson_mut_obj(d);
  yyjson_mut_obj_add_str(d, obj, "field", field_of(c)->name);
  yyjson_mut_obj_add_str(d, obj, "op", op_of(c));
  if (v) yyjson_mut_obj_add_val(d, obj, "value", v);
  return obj;
}

static yyjson_mut_val *node_json(yyjson_mut_doc *d, const node *n, gboolean lenient, gboolean *ok);

/* {"name", "all": [...]}, {"name", "any": [...]} or, for none, {"name", "not": {"any": [...]}}.
 * Lenient leaves out what has a problem, and empty groups, as for a sentence; otherwise a
 * problem anywhere clears *ok. */
static yyjson_mut_val *group_json(yyjson_mut_doc *d, const node *g, gboolean lenient, gboolean *ok) {
  yyjson_mut_val *list = yyjson_mut_arr(d);
  for (guint i = 0; i < g->kids->len; i++) {
    const node *k = kid(g, i);
    if (lenient && k->is_group && !k->kids->len) continue;
    yyjson_mut_val *v = node_json(d, k, lenient, ok);
    if (v) yyjson_mut_arr_append(list, v);
  }
  yyjson_mut_val *obj = yyjson_mut_obj(d);
  if (named(g) && g->parent) yyjson_mut_obj_add_strcpy(d, obj, "name", g->name);
  if (g->mode != MODE_NONE) {
    yyjson_mut_obj_add_val(d, obj, g->mode == MODE_ALL ? "all" : "any", list);
    return obj;
  }
  yyjson_mut_val *inner = yyjson_mut_obj(d);
  yyjson_mut_obj_add_val(d, inner, "any", list);
  yyjson_mut_obj_add_val(d, obj, "not", inner);
  return obj;
}

static yyjson_mut_val *node_json(yyjson_mut_doc *d, const node *n, gboolean lenient, gboolean *ok) {
  if (n->is_group) return group_json(d, n, lenient, ok);
  yyjson_mut_val *v = cond_json(d, n);
  if (!v && !lenient) *ok = FALSE;
  return v;
}

/* A document with {"v": 1} at its root, which *root is. */
static yyjson_mut_doc *rule_doc(yyjson_mut_val **root) {
  yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
  *root = yyjson_mut_obj(d);
  yyjson_mut_doc_set_root(d, *root);
  yyjson_mut_obj_add_int(d, *root, "v", 1);
  return d;
}

static char *write_doc(yyjson_mut_doc *d, gboolean ok) {
  char *json = ok ? yyjson_mut_write(d, 0, NULL) : NULL;
  char *out = g_strdup(json);
  free(json);
  yyjson_mut_doc_free(d);
  return out;
}

/* {"v":1,"match":<n>} as written (g_free it), or NULL if strict and n has a problem. */
static char *match_json(const node *n, gboolean lenient) {
  yyjson_mut_val *root;
  yyjson_mut_doc *d = rule_doc(&root);
  gboolean ok = TRUE;
  yyjson_mut_val *v = node_json(d, n, lenient, &ok);
  if (v) yyjson_mut_obj_add_val(d, root, "match", v);
  return write_doc(d, ok && v);
}

/* The whole rule as it stands (g_free it), or NULL if anything in it has a problem. */
static char *rule_json(gboolean with_limit) {
  yyjson_mut_val *root;
  yyjson_mut_doc *d = rule_doc(&root);
  gboolean ok = TRUE;
  if (ed->root->kids->len) yyjson_mut_obj_add_val(d, root, "match", group_json(d, ed->root, FALSE, &ok));
  yyjson_mut_val *order = yyjson_mut_arr(d);
  for (guint i = 0; i < ed->sorts->len; i++) {
    int k = g_array_index(ed->sorts, int, i);
    yyjson_mut_val *key = yyjson_mut_obj(d);
    yyjson_mut_obj_add_str(d, key, "field", ed->fields[ed->sortable[k / 2]].name);
    yyjson_mut_obj_add_str(d, key, "dir", k % 2 ? "desc" : "asc");
    yyjson_mut_arr_append(order, key);
  }
  if (ed->random) {
    yyjson_mut_val *key = yyjson_mut_obj(d);
    yyjson_mut_obj_add_str(d, key, "field", "random");
    yyjson_mut_obj_add_sint(d, key, "seed", ed->seed);
    yyjson_mut_arr_append(order, key);
  }
  if (yyjson_mut_arr_size(order)) yyjson_mut_obj_add_val(d, root, "order", order);
  if (with_limit && ed->limited && ed->limit > 0) yyjson_mut_obj_add_sint(d, root, "limit", ed->limit);
  return write_doc(d, ok);
}

/* What the core says rule is, or NULL. g_free it. */
static char *describe(const char *rule) {
  char *core = g_strdup(rule), *sentence = NULL, *out = NULL;
  if (core && mc_smart_describe(ed->lib, ed->subject, core, &sentence) == MC_OK) out = g_strdup(sentence);
  mc_free(sentence);
  g_free(core);
  return out;
}

/* --- Reading a rule into the editor --- */

static gint64 json_int(yyjson_val *v) {
  return yyjson_is_sint(v) ? yyjson_get_sint(v) : yyjson_is_uint(v) ? (gint64)yyjson_get_uint(v) : 0;
}

/* A "not" around a condition, as the negative operator that means the same: missing values
 * count as false either way, so only the ops the core writes as another's negation map. */
static const char *negated_op(const char *op) {
  static const char *const k[][2] = {{"is", "is_not"}, {"contains", "not_contains"}, {"empty", "not_empty"},
                                     {"=", "!="},      {"within", "not_within"}};
  for (size_t i = 0; op && i < G_N_ELEMENTS(k); i++) {
    if (!strcmp(op, k[i][0])) return k[i][1];
    if (!strcmp(op, k[i][1])) return k[i][0];
  }
  return NULL;
}

/* A condition (or a "not" around one, if negated) into a new node of parent's; FALSE if the
 * editor can't show it. The rule has been validated, so values have the right shapes. */
static gboolean load_cond(node *parent, yyjson_val *leaf, gboolean negated) {
  int field = find_field(yyjson_get_str(yyjson_obj_get(leaf, "field")));
  if (field < 0) return FALSE;
  const char *type = ed->fields[field].type, *op = yyjson_get_str(yyjson_obj_get(leaf, "op"));
  yyjson_val *value = yyjson_obj_get(leaf, "value");
  gboolean yes = yyjson_get_bool(value);
  if (negated && !strcmp(type, "bool")) yes = !yes;
  else if (negated) op = negated_op(op);
  int op_i = find_op(field, op);
  if (op_i < 0) return FALSE;
  node *c = node_new(parent, FALSE);
  c->field = field;
  c->op = op_i;
  reset_values(c);
  if (!strcmp(type, "text") && yyjson_is_str(value)) {
    g_free(c->value);
    c->value = g_strdup(yyjson_get_str(value));
  } else if (!strcmp(type, "int") || !strcmp(type, "duration")) {
    gboolean dur = !strcmp(type, "duration");
    yyjson_val *vals[2] = {value, NULL};
    if (yyjson_is_arr(value)) vals[0] = yyjson_arr_get(value, 0), vals[1] = yyjson_arr_get(value, 1);
    char **to[2] = {&c->value, &c->value2};
    for (int i = 0; i < 2; i++) {
      if (!vals[i]) continue;
      gint64 n = json_int(vals[i]);
      if (n < 0) return FALSE; /* the boxes take digits only */
      g_free(*to[i]);
      *to[i] = dur ? format_duration(n) : g_strdup_printf("%" G_GINT64_FORMAT, n);
    }
  } else if (!strcmp(type, "date") && yyjson_is_obj(value)) {
    gint64 days = json_int(yyjson_obj_get(value, "days"));
    gboolean weeks = days >= 14 && days % 7 == 0; /* as mc_smart_describe() says it */
    g_free(c->value);
    c->value = g_strdup_printf("%" G_GINT64_FORMAT, weeks ? days / 7 : days);
    c->unit = weeks ? UNIT_WEEKS : UNIT_DAYS;
  } else if (!strcmp(type, "date") && value) {
    c->date = json_int(value);
  } else if (!strcmp(type, "bool")) {
    c->choice = yes ? 1 : 0;
  } else if (yyjson_is_str(value)) {
    const char *s = yyjson_get_str(value);
    int found = -1;
    if (!strcmp(type, "playlist"))
      for (int i = 0; ed->playlists && i < ed->playlists->count && found < 0; i++)
        if (!strcmp(ed->playlists->items[i].uid, s)) found = i;
    if (!strcmp(type, "group"))
      for (int i = 0; ed->groups && i < ed->groups->count && found < 0; i++)
        if (!strcmp(ed->groups->items[i].uid, s)) found = i;
    if (!strcmp(type, "format"))
      for (int i = 0; i < FORMAT_COUNT && found < 0; i++)
        if (!g_ascii_strcasecmp(k_formats[i].value, s)) found = i;
    if (found < 0) c->extra = g_strdup(s);
    c->choice = found >= 0 ? found : CHOICE_EXTRA;
  }
  return TRUE;
}

/* {"all": [...]}, {"any": [...]} or {"not": {"any": [...]}}, each maybe with a "name": its
 * mode, list and name. FALSE for anything else. */
static gboolean group_of(yyjson_val *v, int *mode, yyjson_val **list, const char **name) {
  *name = yyjson_get_str(yyjson_obj_get(v, "name"));
  yyjson_val *not = yyjson_obj_get(v, "not");
  if (not) {
    *list = yyjson_obj_get(not, "any");
    *mode = MODE_NONE;
    return *list != NULL;
  }
  if ((*list = yyjson_obj_get(v, "all"))) *mode = MODE_ALL;
  else if ((*list = yyjson_obj_get(v, "any"))) *mode = MODE_ANY;
  return *list != NULL;
}

/* v into a new node of parent's. FALSE if the editor can't show it. */
static gboolean load_node(node *parent, yyjson_val *v) {
  int mode;
  yyjson_val *list;
  const char *name;
  if (group_of(v, &mode, &list, &name)) {
    node *g = node_new(parent, TRUE);
    g->mode = mode;
    g->name = g_strdup(name);
    size_t i, max;
    yyjson_val *k;
    yyjson_arr_foreach(list, i, max, k) if (!load_node(g, k)) return FALSE;
    return TRUE;
  }
  yyjson_val *not = yyjson_obj_get(v, "not");
  if (not && (yyjson_obj_get(not, "all") || yyjson_obj_get(not, "any") || yyjson_obj_get(not, "not"))) return FALSE;
  return load_cond(parent, not ? not : v, not != NULL);
}

/* rule into ed->root and the order. FALSE if the editor can't show it: it's read-only then. */
static gboolean load_rule(const char *rule) {
  char *core = g_strdup(rule);
  gboolean valid = core && mc_smart_validate(ed->subject, core) == MC_OK;
  g_free(core);
  if (!valid) return FALSE;
  yyjson_doc *doc = yyjson_read(rule, strlen(rule), 0);
  yyjson_val *root = yyjson_doc_get_root(doc), *match = yyjson_obj_get(root, "match");
  gboolean ok = doc != NULL && json_int(yyjson_obj_get(root, "v")) == 1;
  int mode;
  yyjson_val *list = NULL;
  const char *name;
  if (ok && match && group_of(match, &mode, &list, &name)) {
    ed->root->mode = mode;
    size_t i, max;
    yyjson_val *k;
    yyjson_arr_foreach(list, i, max, k) if (ok) ok = load_node(ed->root, k);
  } else if (ok && match) { /* one condition on its own */
    ok = load_node(ed->root, match);
  }
  size_t i, max;
  yyjson_val *key;
  yyjson_arr_foreach(yyjson_obj_get(root, "order"), i, max, key) {
    if (!ok) break;
    const char *field = yyjson_get_str(yyjson_obj_get(key, "field"));
    if (field && !strcmp(field, "random")) {
      ed->random = TRUE;
      ed->seed = json_int(yyjson_obj_get(key, "seed"));
      continue;
    }
    int f = find_field(field), s = -1;
    for (int j = 0; j < ed->nsortable && f >= 0; j++)
      if (ed->sortable[j] == f) s = j;
    yyjson_val *dir = yyjson_obj_get(key, "dir");
    ok = s >= 0 && ed->sorts->len < MAX_SORTS;
    int k = s * 2 + (dir && yyjson_equals_str(dir, "desc") ? 1 : 0);
    if (ok) g_array_append_val(ed->sorts, k);
  }
  yyjson_val *limit = yyjson_obj_get(root, "limit");
  if (ok && limit) {
    ed->limited = TRUE;
    ed->limit = (int)CLAMP(json_int(limit), 1, G_MAXINT);
  }
  yyjson_doc_free(doc);
  return ok;
}

/* --- Problems --- */

static const char *problem_text(int problem) {
  switch (problem) {
    case PROBLEM_NUMBER: return mc_str(MC_STR_SMART_PROBLEM_NUMBER);
    case PROBLEM_DURATION: return mc_str(MC_STR_SMART_PROBLEM_DURATION);
    case PROBLEM_CHOICE: return mc_str(MC_STR_SMART_PROBLEM_CHOICE);
    default: return mc_str(MC_STR_SMART_PROBLEM_VALUE);
  }
}

/* Every condition's problem and every group's bad, with the conditions that have one added to
 * ed->bad in order. TRUE if g holds any. */
static gboolean check_group(node *g) {
  g->bad = FALSE;
  for (guint i = 0; i < g->kids->len; i++) {
    node *k = kid(g, i);
    if (k->is_group) {
      g->bad |= check_group(k);
    } else if ((k->problem = cond_problem(k))) {
      g_ptr_array_add(ed->bad, k);
      g->bad = TRUE;
    }
  }
  return g->bad;
}

/* What a group is called in the path and the problems: its name, else its sentence, else
 * "Group". */
static const char *group_title(const node *g) {
  if (!g->parent) return ed->name && *ed->name ? ed->name : mc_str(MC_STR_UNTITLED);
  if (named(g)) return g->name;
  return g->desc ? g->desc : mc_str(MC_STR_SMART_GROUP_UNTITLED);
}

/* "Fresh stuff › Artist: needs a value". g_free it. */
static char *problem_line(const node *c) {
  GString *where = g_string_new(NULL);
  for (const node *g = c->parent; g && g->parent; g = g->parent) {
    if (where->len) g_string_prepend(where, mc_str(MC_STR_SMART_PATH_SEPARATOR));
    g_string_prepend(where, group_title(g));
  }
  if (where->len) g_string_append(where, mc_str(MC_STR_SMART_PATH_SEPARATOR));
  g_string_append(where, mc_smart_field_label(field_of(c)->name));
  char *line = mc_format_alloc(mc_str(MC_STR_SMART_PROBLEM), MC_S(where->str), MC_S(problem_text(c->problem)));
  g_string_free(where, TRUE);
  char *out = g_strdup(line ? line : "");
  mc_free(line);
  return out;
}

/* --- Pages --- */

static void changed(void);
static void structure_changed(page *p);
static void push_group(node *g);
static void on_shown(AdwNavigationPage *nav, gpointer data);
static void show_problem(guint id);

static page *page_of(AdwNavigationPage *nav) {
  return nav ? g_object_get_data(G_OBJECT(nav), "smart-page") : NULL;
}

static page *visible_page(void) {
  return ed && ed->nav ? page_of(adw_navigation_view_get_visible_page(ed->nav)) : NULL;
}

static node *page_group(const page *p) {
  return p ? find(p->group) : NULL;
}

static row *row_of(page *p, guint id) {
  for (guint i = 0; p && i < p->rows->len; i++) {
    row *r = g_ptr_array_index(p->rows, i);
    if (r->id == id) return r;
  }
  return NULL;
}

/* The count beside a row: what n matches on its own, if it's been counted as n now stands. */
static void set_count(GtkWidget *label, node *n) {
  char *now = NULL, *core = NULL;
  if (!(n->is_group && !n->kids->len)) now = match_json(n, FALSE);
  if (now) core = g_strdup(now);
  gboolean shown = core && n->counted && n->count >= 0 && !strcmp(core, n->counted);
  char text[32] = "", tip[128] = "";
  if (shown) {
    g_snprintf(text, sizeof text, "%d", n->count);
    mc_format(tip, sizeof tip,
              mc_str_plural(ed->subject == MC_SMART_TRACKS ? MC_STR_SMART_TRACKS_MATCH : MC_STR_SMART_ALBUMS_MATCH,
                            n->count),
              MC_D(n->count));
  }
  gtk_label_set_text(GTK_LABEL(label), text);
  gtk_widget_set_tooltip_text(label, *tip ? tip : NULL);
  set_label(label, tip);
  g_free(now);
  g_free(core);
}

/* A condition row's line beneath: its problem, else that it depends on this device, else that
 * a negative text condition takes in what has no value. */
static void update_cond_row(row *r, node *c) {
  const char *hint = NULL;
  char buf[256];
  if (c->problem) {
    hint = problem_text(c->problem);
  } else if (!strcmp(field_of(c)->name, "on_device") || !strcmp(field_of(c)->name, "format")) {
    hint = mc_str(MC_STR_SMART_HINT_DEVICE);
  } else if (in_text_id(field_of(c)->name) != MC_STR_COUNT && (op_is(c, "is_not") || op_is(c, "not_contains"))) {
    mc_format(buf, sizeof buf,
              mc_str(ed->subject == MC_SMART_TRACKS ? MC_STR_SMART_HINT_MISSING_TRACKS : MC_STR_SMART_HINT_MISSING_ALBUMS),
              MC_S(mc_str(in_text_id(field_of(c)->name))));
    hint = buf;
  }
  gtk_label_set_text(GTK_LABEL(r->hint), hint ? hint : "");
  gtk_widget_set_visible(r->hint, hint != NULL);
  if (c->problem) {
    gtk_widget_add_css_class(r->hint, "error");
    gtk_widget_remove_css_class(r->hint, "dim-label");
  } else {
    gtk_widget_remove_css_class(r->hint, "error");
    gtk_widget_add_css_class(r->hint, "dim-label");
  }
  set_count(r->count, c);
}

static void update_group_row(row *r, node *g) {
  AdwPreferencesRow *pr = ADW_PREFERENCES_ROW(r->widget);
  const char *sub = !g->kids->len ? mc_str(MC_STR_SMART_GROUP_EMPTY_HINT) : named(g) ? g->desc : NULL;
  adw_preferences_row_set_title(pr, group_title(g));
  adw_action_row_set_subtitle(ADW_ACTION_ROW(r->widget), sub ? sub : "");
  gtk_widget_set_visible(r->warn, g->bad);
  set_count(r->count, g);
}

/* Brings a page's rows, name placeholder and placeholder up to date with the rule. */
static void update_page(page *p) {
  node *g = page_group(p);
  if (!g) return;
  for (guint i = 0; i < p->rows->len; i++) {
    row *r = g_ptr_array_index(p->rows, i);
    node *n = find(r->id);
    if (n && n->is_group) update_group_row(r, n);
    else if (n) update_cond_row(r, n);
  }
  if (g->parent && p->name) {
    gtk_entry_set_placeholder_text(GTK_ENTRY(p->name), g->desc ? g->desc : "");
    adw_navigation_page_set_title(p->nav, group_title(g));
  }
}

/* --- Condition rows --- */

static void on_value_changed(GtkEditable *e, gpointer data) {
  node *c = find(GPOINTER_TO_UINT(data));
  if (!c || ed->building) return;
  char **to = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(e), "high")) ? &c->value2 : &c->value;
  g_free(*to);
  *to = g_strdup(gtk_editable_get_text(e));
  changed();
}

static void on_unit_changed(GObject *d, GParamSpec *pspec, gpointer data) {
  (void)pspec;
  node *c = find(GPOINTER_TO_UINT(data));
  if (!c || ed->building) return;
  c->unit = (int)gtk_drop_down_get_selected(GTK_DROP_DOWN(d));
  changed();
}

static void on_choice_changed(GObject *d, GParamSpec *pspec, gpointer data) {
  (void)pspec;
  node *c = find(GPOINTER_TO_UINT(data));
  if (!c || ed->building) return;
  guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(d));
  int n = type_is(c, "playlist") ? (ed->playlists ? ed->playlists->count : 0)
          : type_is(c, "group")  ? (ed->groups ? ed->groups->count : 0)
                                 : FORMAT_COUNT;
  if (type_is(c, "bool")) c->choice = i == 0 ? 1 : 0;
  else if (i == GTK_INVALID_LIST_POSITION) c->choice = CHOICE_NONE;
  else c->choice = (int)i < n ? (int)i : CHOICE_EXTRA;
  changed();
}

static void on_day_selected(GtkCalendar *cal, gpointer data) {
  node *c = find(GPOINTER_TO_UINT(data));
  if (!c || ed->building) return;
  GDateTime *t = gtk_calendar_get_date(cal);
  GDateTime *day = g_date_time_new_local(g_date_time_get_year(t), g_date_time_get_month(t),
                                         g_date_time_get_day_of_month(t), 0, 0, 0);
  c->date = g_date_time_to_unix(day) * 1000;
  g_date_time_unref(day);
  g_date_time_unref(t);
  GtkWidget *button = g_object_get_data(G_OBJECT(cal), "button");
  char *text = format_date(c->date);
  gtk_menu_button_set_label(GTK_MENU_BUTTON(button), text);
  g_free(text);
  gtk_menu_button_popdown(GTK_MENU_BUTTON(button));
  changed();
}

static GtkWidget *value_entry(node *c, gboolean high, const char *text, int width) {
  GtkWidget *e = gtk_entry_new();
  gtk_editable_set_text(GTK_EDITABLE(e), text ? text : "");
  gtk_entry_set_max_length(GTK_ENTRY(e), 256);
  gtk_widget_set_valign(e, GTK_ALIGN_CENTER);
  if (width > 0) gtk_editable_set_width_chars(GTK_EDITABLE(e), width);
  else gtk_widget_set_hexpand(e, TRUE);
  if (type_is(c, "int") || type_is(c, "date")) gtk_entry_set_input_purpose(GTK_ENTRY(e), GTK_INPUT_PURPOSE_DIGITS);
  if (type_is(c, "duration")) gtk_entry_set_placeholder_text(GTK_ENTRY(e), mc_str(MC_STR_SMART_DURATION_CUE));
  set_label(e, mc_str(high ? MC_STR_SMART_VALUE_HIGH_NAME : MC_STR_SMART_VALUE_NAME));
  g_object_set_data(G_OBJECT(e), "high", GINT_TO_POINTER(high));
  g_signal_connect(e, "changed", G_CALLBACK(on_value_changed), GUINT_TO_POINTER(c->id));
  return e;
}

/* The picker of Yes and No, the playlists, the album groups or the formats, with c->extra as
 * one more if it's set. */
static GtkWidget *choice_drop(node *c) {
  GPtrArray *labels = g_ptr_array_new_with_free_func(g_free);
  if (type_is(c, "bool")) {
    g_ptr_array_add(labels, g_strdup(mc_str(MC_STR_SMART_VALUE_YES)));
    g_ptr_array_add(labels, g_strdup(mc_str(MC_STR_SMART_VALUE_NO)));
  } else if (type_is(c, "playlist")) {
    for (int i = 0; ed->playlists && i < ed->playlists->count; i++)
      g_ptr_array_add(labels, g_strdup(ed->playlists->items[i].name ? ed->playlists->items[i].name : ""));
  } else if (type_is(c, "group")) {
    for (int i = 0; ed->groups && i < ed->groups->count; i++)
      g_ptr_array_add(labels, g_strdup(ed->groups->items[i].name ? ed->groups->items[i].name : ""));
  } else {
    for (int i = 0; i < FORMAT_COUNT; i++) g_ptr_array_add(labels, g_strdup(k_formats[i].label));
  }
  guint extra = labels->len;
  if (c->extra && type_is(c, "format")) g_ptr_array_add(labels, g_ascii_strup(c->extra, -1));
  else if (c->extra) g_ptr_array_add(labels, g_strdup(mc_str(MC_STR_SMART_DELETED_MEMBER)));
  g_ptr_array_add(labels, NULL);
  GtkWidget *d = drop_down((const char *const *)labels->pdata, mc_str(MC_STR_SMART_VALUE_NAME));
  g_ptr_array_free(labels, TRUE);
  gtk_widget_set_hexpand(d, TRUE);
  guint sel = type_is(c, "bool")          ? (c->choice ? 0 : 1)
              : c->choice == CHOICE_EXTRA ? extra
              : c->choice >= 0            ? (guint)c->choice
                                          : GTK_INVALID_LIST_POSITION;
  gtk_drop_down_set_selected(GTK_DROP_DOWN(d), sel);
  g_signal_connect(d, "notify::selected", G_CALLBACK(on_choice_changed), GUINT_TO_POINTER(c->id));
  return d;
}

/* The value's widgets for c's field and operator, into r->values. */
static void fill_values(row *r, node *c) {
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(r->values))) gtk_box_remove(GTK_BOX(r->values), child);
  r->focus = NULL;
  ed->building = TRUE;
  if (takes_no_value(c)) {
  } else if (is_choice(c)) {
    r->focus = choice_drop(c);
    gtk_box_append(GTK_BOX(r->values), r->focus);
  } else if ((type_is(c, "int") || type_is(c, "duration")) && op_is(c, "between")) {
    r->focus = value_entry(c, FALSE, c->value, 6);
    gtk_box_append(GTK_BOX(r->values), r->focus);
    gtk_box_append(GTK_BOX(r->values), gtk_label_new(mc_str(MC_STR_SMART_BETWEEN_AND)));
    gtk_box_append(GTK_BOX(r->values), value_entry(c, TRUE, c->value2, 6));
  } else if (type_is(c, "int") || type_is(c, "duration")) {
    r->focus = value_entry(c, FALSE, c->value, 8);
    gtk_box_append(GTK_BOX(r->values), r->focus);
  } else if (type_is(c, "date") && (op_is(c, "within") || op_is(c, "not_within"))) {
    r->focus = value_entry(c, FALSE, c->value, 5);
    gtk_box_append(GTK_BOX(r->values), r->focus);
    const char *units[] = {mc_str(MC_STR_SMART_UNIT_DAYS), mc_str(MC_STR_SMART_UNIT_WEEKS),
                           mc_str(MC_STR_SMART_UNIT_MONTHS), NULL};
    GtkWidget *unit = drop_down(units, mc_str(MC_STR_SMART_UNIT_NAME));
    gtk_drop_down_set_selected(GTK_DROP_DOWN(unit), (guint)c->unit);
    g_signal_connect(unit, "notify::selected", G_CALLBACK(on_unit_changed), GUINT_TO_POINTER(c->id));
    gtk_box_append(GTK_BOX(r->values), unit);
  } else if (type_is(c, "date")) {
    GtkWidget *cal = gtk_calendar_new();
    GDateTime *t = g_date_time_new_from_unix_local(c->date / 1000);
    if (t) {
#if GTK_CHECK_VERSION(4, 20, 0)
      gtk_calendar_set_date(GTK_CALENDAR(cal), t);
#else
      gtk_calendar_select_day(GTK_CALENDAR(cal), t);
#endif
      g_date_time_unref(t);
    }
    GtkWidget *pop = gtk_popover_new();
    gtk_popover_set_child(GTK_POPOVER(pop), cal);
    GtkWidget *button = gtk_menu_button_new();
    char *text = format_date(c->date);
    gtk_menu_button_set_label(GTK_MENU_BUTTON(button), text);
    g_free(text);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(button), pop);
    gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(button, mc_str(MC_STR_SMART_PICK_DATE));
    set_label(button, mc_str(MC_STR_SMART_VALUE_NAME));
    g_object_set_data(G_OBJECT(cal), "button", button);
    g_signal_connect(cal, "day-selected", G_CALLBACK(on_day_selected), GUINT_TO_POINTER(c->id));
    r->focus = button;
    gtk_box_append(GTK_BOX(r->values), button);
  } else {
    r->focus = value_entry(c, FALSE, c->value, 0);
    gtk_box_append(GTK_BOX(r->values), r->focus);
  }
  ed->building = FALSE;
  if (!r->focus) r->focus = r->field;
}

/* The op picker for c's field, with c->op selected. */
static void fill_ops(row *r, node *c) {
  GPtrArray *labels = g_ptr_array_new();
  const mc_smart_field *f = field_of(c);
  for (int i = 0; f->ops[i]; i++) g_ptr_array_add(labels, (gpointer)op_label(f->type, f->ops[i]));
  g_ptr_array_add(labels, NULL);
  ed->building = TRUE;
  GtkStringList *model = gtk_string_list_new((const char *const *)labels->pdata);
  gtk_drop_down_set_model(GTK_DROP_DOWN(r->op), G_LIST_MODEL(model));
  g_object_unref(model);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(r->op), (guint)c->op);
  ed->building = FALSE;
  g_ptr_array_free(labels, TRUE);
}

static void on_field_changed(GObject *d, GParamSpec *pspec, gpointer data) {
  (void)pspec;
  node *c = find(GPOINTER_TO_UINT(data));
  row *r = c ? row_of(visible_page(), c->id) : NULL;
  guint f = gtk_drop_down_get_selected(GTK_DROP_DOWN(d));
  if (!c || !r || ed->building || f == GTK_INVALID_LIST_POSITION || (int)f == c->field) return;
  const char *was_type = field_of(c)->type, *was_op = op_of(c);
  gboolean same_type = !strcmp(was_type, ed->fields[f].type);
  c->field = (int)f;
  int op = same_type ? find_op(c->field, was_op) : -1;
  c->op = op >= 0 ? op : default_op(c->field);
  if (!same_type) reset_values(c); /* "jazz" means nothing as a length */
  else if (is_choice(c)) {
    c->choice = first_choice(c);
    g_clear_pointer(&c->extra, g_free);
  }
  fill_ops(r, c);
  fill_values(r, c);
  changed();
}

static void on_op_changed(GObject *d, GParamSpec *pspec, gpointer data) {
  (void)pspec;
  node *c = find(GPOINTER_TO_UINT(data));
  row *r = c ? row_of(visible_page(), c->id) : NULL;
  guint op = gtk_drop_down_get_selected(GTK_DROP_DOWN(d));
  if (!c || !r || ed->building || op == GTK_INVALID_LIST_POSITION || (int)op == c->op) return;
  c->op = (int)op;
  fill_values(r, c);
  changed();
}

/* x on a row: the condition or group goes, and the focus to the row after it (or before, or
 * Add condition). */
static void on_remove(GtkButton *button, gpointer data) {
  (void)button;
  node *n = find(GPOINTER_TO_UINT(data));
  page *p = visible_page();
  node *g = page_group(p);
  if (!n || !g || n->parent != g) return;
  guint at = 0;
  g_ptr_array_find(g->kids, n, &at);
  g_ptr_array_remove_index(g->kids, at);
  node_free(n);
  structure_changed(p);
  guint next = at < g->kids->len ? at : at - 1;
  row *r = g->kids->len ? row_of(p, kid(g, next)->id) : NULL;
  gtk_widget_grab_focus(r ? (r->focus ? r->focus : r->widget) : p->add_condition);
}

static GtkWidget *count_label(void) {
  GtkWidget *l = gtk_label_new(NULL);
  gtk_widget_add_css_class(l, "dim-label");
  gtk_widget_add_css_class(l, "numeric");
  gtk_label_set_xalign(GTK_LABEL(l), 1);
  gtk_label_set_width_chars(GTK_LABEL(l), 5);
  gtk_widget_set_valign(l, GTK_ALIGN_CENTER);
  return l;
}

/*  [Field v] [op v] [value        ]  412  x
 *  Needs a value                               <- its problem, or a hint */
static row *cond_row(node *c) {
  row *r = g_new0(row, 1);
  r->id = c->id;
  GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  GPtrArray *labels = g_ptr_array_new();
  for (int i = 0; i < ed->nfields; i++) g_ptr_array_add(labels, (gpointer)mc_smart_field_label(ed->fields[i].name));
  g_ptr_array_add(labels, NULL);
  r->field = drop_down((const char *const *)labels->pdata, mc_str(MC_STR_SMART_FIELD_NAME));
  g_ptr_array_free(labels, TRUE);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(r->field), (guint)c->field);
  gtk_box_append(GTK_BOX(line), r->field);
  r->op = gtk_drop_down_new(NULL, NULL);
  gtk_widget_set_valign(r->op, GTK_ALIGN_CENTER);
  set_label(r->op, mc_str(MC_STR_SMART_OP_NAME));
  fill_ops(r, c);
  gtk_box_append(GTK_BOX(line), r->op);
  r->values = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_hexpand(r->values, TRUE);
  gtk_box_append(GTK_BOX(line), r->values);
  fill_values(r, c);
  r->count = count_label();
  gtk_box_append(GTK_BOX(line), r->count);
  GtkWidget *remove = flat_button("window-close-symbolic", mc_str(MC_STR_SMART_REMOVE_CONDITION));
  g_signal_connect(remove, "clicked", G_CALLBACK(on_remove), GUINT_TO_POINTER(c->id));
  gtk_box_append(GTK_BOX(line), remove);
  g_signal_connect(r->field, "notify::selected", G_CALLBACK(on_field_changed), GUINT_TO_POINTER(c->id));
  g_signal_connect(r->op, "notify::selected", G_CALLBACK(on_op_changed), GUINT_TO_POINTER(c->id));

  r->hint = gtk_label_new(NULL);
  gtk_widget_add_css_class(r->hint, "caption");
  gtk_label_set_xalign(GTK_LABEL(r->hint), 0);
  gtk_label_set_wrap(GTK_LABEL(r->hint), TRUE);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_margin_start(box, 12);
  gtk_widget_set_margin_end(box, 6);
  gtk_widget_set_margin_top(box, 6);
  gtk_widget_set_margin_bottom(box, 6);
  gtk_box_append(GTK_BOX(box), line);
  gtk_box_append(GTK_BOX(box), r->hint);
  r->widget = gtk_list_box_row_new();
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(r->widget), box);
  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(r->widget), FALSE);
  gtk_widget_set_focusable(r->widget, FALSE); /* Tab goes straight to its pickers */
  update_cond_row(r, c);
  return r;
}

/* --- Group rows --- */

static void on_group_activated(AdwActionRow *w, gpointer data) {
  (void)w;
  node *g = find(GPOINTER_TO_UINT(data));
  if (g) push_group(g);
}

/*  Fresh stuff                         40  !  x  >
 *  Tracks where date added is in the last 30 days */
static row *group_row(node *g) {
  row *r = g_new0(row, 1);
  r->id = g->id;
  r->widget = adw_action_row_new();
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(r->widget), FALSE);
  adw_action_row_set_title_lines(ADW_ACTION_ROW(r->widget), 1);
  adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(r->widget), 2);
  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(r->widget), TRUE);
  gtk_widget_set_tooltip_text(r->widget, mc_str(MC_STR_SMART_OPEN_GROUP));
  r->count = count_label();
  adw_action_row_add_suffix(ADW_ACTION_ROW(r->widget), r->count);
  r->warn = gtk_image_new_from_icon_name("dialog-warning-symbolic");
  gtk_widget_add_css_class(r->warn, "error");
  gtk_widget_set_tooltip_text(r->warn, mc_str(MC_STR_SMART_GROUP_HAS_PROBLEM));
  set_label(r->warn, mc_str(MC_STR_SMART_GROUP_HAS_PROBLEM));
  adw_action_row_add_suffix(ADW_ACTION_ROW(r->widget), r->warn);
  GtkWidget *remove = flat_button("window-close-symbolic", mc_str(MC_STR_SMART_REMOVE_GROUP));
  g_signal_connect(remove, "clicked", G_CALLBACK(on_remove), GUINT_TO_POINTER(g->id));
  adw_action_row_add_suffix(ADW_ACTION_ROW(r->widget), remove);
  adw_action_row_add_suffix(ADW_ACTION_ROW(r->widget), gtk_image_new_from_icon_name("go-next-symbolic"));
  g_signal_connect(r->widget, "activated", G_CALLBACK(on_group_activated), GUINT_TO_POINTER(g->id));
  update_group_row(r, g);
  return r;
}

/* --- A page's parts --- */

/* The page's rows, made again from its group. */
static void fill_rows(page *p) {
  node *g = page_group(p);
  for (guint i = 0; i < p->rows->len; i++) /* not remove_all(), which takes the placeholder too */
    gtk_list_box_remove(GTK_LIST_BOX(p->list), ((row *)g_ptr_array_index(p->rows, i))->widget);
  g_ptr_array_set_size(p->rows, 0);
  for (guint i = 0; g && i < g->kids->len; i++) {
    node *n = kid(g, i);
    row *r = n->is_group ? group_row(n) : cond_row(n);
    g_ptr_array_add(p->rows, r);
    gtk_list_box_append(GTK_LIST_BOX(p->list), r->widget);
  }
}

/* After a row has come or gone. */
static void structure_changed(page *p) {
  fill_rows(p);
  changed();
}

static void on_add_condition(GtkButton *button, gpointer data) {
  (void)button;
  page *p = data;
  node *g = page_group(p);
  if (!g) return;
  node *c = cond_new(g);
  structure_changed(p);
  row *r = row_of(p, c->id);
  if (r) gtk_widget_grab_focus(r->field);
}

static void on_add_group(GtkButton *button, gpointer data) {
  (void)button;
  page *p = data;
  node *g = page_group(p);
  if (!g) return;
  node *n = node_new(g, TRUE);
  n->mode = g->mode == MODE_ALL ? MODE_ANY : MODE_ALL; /* the other way round, else why a group */
  structure_changed(p);
  push_group(n);
}

static void on_mode_changed(GObject *d, GParamSpec *pspec, gpointer data) {
  (void)pspec;
  page *p = data;
  node *g = page_group(p);
  if (!g || ed->building) return;
  g->mode = (int)gtk_drop_down_get_selected(GTK_DROP_DOWN(d));
  changed();
}

static void on_group_name_changed(GtkEditable *e, gpointer data) {
  page *p = data;
  node *g = page_group(p);
  if (!g || ed->building) return;
  g_free(g->name);
  g->name = trimmed(gtk_editable_get_text(e));
  changed();
}

static void on_list_name_changed(GtkEditable *e, gpointer data) {
  (void)data;
  if (!ed || ed->building) return;
  g_free(ed->name);
  ed->name = trimmed(gtk_editable_get_text(e));
  changed();
}

/* The preview's rows: up to PREVIEW_ROWS of n tracks or albums (titles and artists), then "and
 * N more". */
static void fill_preview(page *p, const mc_track_list *tracks, const mc_album_list *albums, int n) {
  gtk_list_box_remove_all(GTK_LIST_BOX(p->preview));
  int shown = MIN(n, PREVIEW_ROWS);
  for (int i = 0; i < shown; i++) {
    const char *title = tracks ? tracks->items[i].title : albums->items[i].title;
    const char *artist = tracks ? tracks->items[i].artist : albums->items[i].artist;
    GtkWidget *r = adw_action_row_new();
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(r), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(r), title && *title ? title : mc_str(MC_STR_UNTITLED));
    adw_action_row_set_subtitle(ADW_ACTION_ROW(r), artist ? artist : "");
    adw_action_row_set_title_lines(ADW_ACTION_ROW(r), 1);
    adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(r), 1);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(r), FALSE);
    gtk_list_box_append(GTK_LIST_BOX(p->preview), r);
  }
  gtk_widget_set_visible(p->preview, shown > 0);
  char more[64] = "";
  if (n > shown) mc_format(more, sizeof more, mc_str_plural(MC_STR_SMART_MORE, n - shown), MC_D(n - shown));
  gtk_label_set_text(GTK_LABEL(p->preview_more), more);
  gtk_widget_set_visible(p->preview_more, n > shown);
}

/* --- The order, on the list's page --- */

static void fill_sorts(page *p);

static void on_sort_changed(GObject *combo, GParamSpec *pspec, gpointer data) {
  (void)pspec;
  guint i = GPOINTER_TO_UINT(data);
  if (!ed || ed->building || i >= ed->sorts->len) return;
  guint sel = adw_combo_row_get_selected(ADW_COMBO_ROW(combo));
  const int *keys = g_object_get_data(combo, "keys");
  if (sel == GTK_INVALID_LIST_POSITION || !keys) return;
  g_array_index(ed->sorts, int, i) = keys[sel];
  changed();
}

static void on_sort_remove(GtkButton *button, gpointer data) {
  (void)button;
  guint i = GPOINTER_TO_UINT(data);
  page *p = visible_page();
  if (!ed || i >= ed->sorts->len || !p || !p->sorts) return;
  g_array_remove_index(ed->sorts, i);
  fill_sorts(p);
  changed();
  gtk_widget_grab_focus(p->sort_add);
}

/* A row per sort order: every sortable field both ways, in mc_smart_order_label()'s words. */
static void fill_sorts(page *p) {
  gtk_list_box_remove_all(GTK_LIST_BOX(p->sorts));
  ed->building = TRUE;
  for (guint i = 0; i < ed->sorts->len; i++) {
    GtkStringList *labels = gtk_string_list_new(NULL);
    int *keys = g_new0(int, (gsize)ed->nsortable * 2 + 1), n = 0;
    guint sel = 0;
    for (int s = 0; s < ed->nsortable; s++)
      for (int dir = 0; dir < 2; dir++) {
        const char *label = mc_smart_order_label(ed->fields[ed->sortable[s]].name, dir);
        if (!label) continue;
        if (s * 2 + dir == g_array_index(ed->sorts, int, i)) sel = (guint)n;
        gtk_string_list_append(labels, label);
        keys[n++] = s * 2 + dir;
      }
    GtkWidget *combo = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(combo), mc_str(i ? MC_STR_SMART_SORT_THEN : MC_STR_SMART_SORT_BY));
    adw_combo_row_set_model(ADW_COMBO_ROW(combo), G_LIST_MODEL(labels));
    g_object_unref(labels);
    adw_combo_row_set_selected(ADW_COMBO_ROW(combo), sel);
    g_object_set_data_full(G_OBJECT(combo), "keys", keys, g_free);
    g_signal_connect(combo, "notify::selected", G_CALLBACK(on_sort_changed), GUINT_TO_POINTER(i));
    GtkWidget *remove = flat_button("window-close-symbolic", mc_str(MC_STR_SMART_REMOVE_SORT));
    g_signal_connect(remove, "clicked", G_CALLBACK(on_sort_remove), GUINT_TO_POINTER(i));
    adw_action_row_add_suffix(ADW_ACTION_ROW(combo), remove);
    gtk_list_box_append(GTK_LIST_BOX(p->sorts), combo);
  }
  ed->building = FALSE;
  gtk_widget_set_visible(p->sorts, ed->sorts->len > 0);
  gtk_widget_set_sensitive(p->sort_add, ed->sorts->len < MAX_SORTS && (int)ed->sorts->len < ed->nsortable);
}

/* The next sortable field not sorted on yet. */
static void on_sort_add(GtkButton *button, gpointer data) {
  (void)button;
  page *p = data;
  int next = 0;
  for (int s = 0; s < ed->nsortable; s++) {
    gboolean used = FALSE;
    for (guint i = 0; i < ed->sorts->len; i++) used |= g_array_index(ed->sorts, int, i) / 2 == s;
    if (!used) {
      next = s;
      break;
    }
  }
  int k = next * 2;
  g_array_append_val(ed->sorts, k);
  fill_sorts(p);
  changed();
  GtkListBoxRow *last = gtk_list_box_get_row_at_index(GTK_LIST_BOX(p->sorts), (int)ed->sorts->len - 1);
  if (last) gtk_widget_grab_focus(GTK_WIDGET(last));
}

static void on_shuffled(GObject *w, GParamSpec *pspec, gpointer data) {
  (void)pspec;
  (void)data;
  if (!ed || ed->building) return;
  ed->random = adw_switch_row_get_active(ADW_SWITCH_ROW(w));
  changed();
}

static void on_limited(GtkCheckButton *check, gpointer data) {
  page *p = data;
  if (!ed || ed->building) return;
  ed->limited = gtk_check_button_get_active(check);
  gtk_widget_set_sensitive(p->limit, ed->limited);
  changed();
}

static void on_limit(GtkSpinButton *spin, gpointer data) {
  (void)data;
  if (!ed || ed->building) return;
  ed->limit = gtk_spin_button_get_value_as_int(spin);
  changed();
}

/*  Order
 *  | Sort by        [least played first v]  x |
 *  [+ Add sort order]
 *  | Shuffled                          [   ]  |
 *  | [x] Only the first           [10] tracks |  */
static GtkWidget *build_order(page *p) {
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_box_append(GTK_BOX(box), heading(mc_str(MC_STR_SMART_ORDER_HEADING)));
  p->sorts = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(p->sorts), GTK_SELECTION_NONE);
  gtk_widget_add_css_class(p->sorts, "boxed-list");
  set_label(p->sorts, mc_str(MC_STR_SMART_SORT_NAME));
  gtk_box_append(GTK_BOX(box), p->sorts);
  p->sort_add = add_button(MC_STR_SMART_ADD_SORT_BUTTON);
  gtk_widget_set_halign(p->sort_add, GTK_ALIGN_START);
  g_signal_connect(p->sort_add, "clicked", G_CALLBACK(on_sort_add), p);
  gtk_box_append(GTK_BOX(box), p->sort_add);
  fill_sorts(p);

  GtkWidget *list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class(list, "boxed-list");
  ed->building = TRUE;
  p->shuffled = adw_switch_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(p->shuffled), mc_str(MC_STR_SMART_SHUFFLED));
  adw_switch_row_set_active(ADW_SWITCH_ROW(p->shuffled), ed->random);
  g_signal_connect(p->shuffled, "notify::active", G_CALLBACK(on_shuffled), p);
  gtk_list_box_append(GTK_LIST_BOX(list), p->shuffled);

  GtkWidget *limit_row = adw_action_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(limit_row), mc_str(MC_STR_SMART_LIMIT));
  p->limited = gtk_check_button_new();
  gtk_check_button_set_active(GTK_CHECK_BUTTON(p->limited), ed->limited);
  gtk_widget_set_valign(p->limited, GTK_ALIGN_CENTER);
  g_signal_connect(p->limited, "toggled", G_CALLBACK(on_limited), p);
  adw_action_row_add_prefix(ADW_ACTION_ROW(limit_row), p->limited);
  adw_action_row_set_activatable_widget(ADW_ACTION_ROW(limit_row), p->limited);
  p->limit = gtk_spin_button_new_with_range(1, 100000, 1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(p->limit), ed->limit);
  gtk_widget_set_valign(p->limit, GTK_ALIGN_CENTER);
  gtk_widget_set_sensitive(p->limit, ed->limited);
  set_label(p->limit, mc_str(MC_STR_SMART_LIMIT_COUNT_NAME));
  g_signal_connect(p->limit, "value-changed", G_CALLBACK(on_limit), p);
  adw_action_row_add_suffix(ADW_ACTION_ROW(limit_row), p->limit);
  GtkWidget *unit = gtk_label_new(mc_str(ed->subject == MC_SMART_TRACKS ? MC_STR_SMART_LIMIT_TRACKS : MC_STR_SMART_LIMIT_ALBUMS));
  adw_action_row_add_suffix(ADW_ACTION_ROW(limit_row), unit);
  gtk_list_box_append(GTK_LIST_BOX(list), limit_row);
  ed->building = FALSE;
  gtk_box_append(GTK_BOX(box), list);
  return box;
}

/* --- A page --- */

static void page_free(gpointer data) {
  page *p = data;
  g_ptr_array_free(p->rows, TRUE);
  g_free(p);
}

static GtkWidget *boxed_list(void) {
  GtkWidget *l = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(l), GTK_SELECTION_NONE);
  gtk_widget_add_css_class(l, "boxed-list");
  return l;
}

/* g's page: on the list's own, its name and order too. */
static AdwNavigationPage *page_new(node *g) {
  page *p = g_new0(page, 1);
  p->group = g->id;
  p->rows = g_ptr_array_new_with_free_func(g_free);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(box, 12);
  gtk_widget_set_margin_end(box, 12);
  gtk_widget_set_margin_top(box, 12);
  gtk_widget_set_margin_bottom(box, 18);

  /* The name: the list's, or the group's with its sentence as placeholder. */
  GtkWidget *name_label = gtk_label_new(mc_str(g->parent ? MC_STR_SMART_GROUP_NAME : MC_STR_NAME));
  gtk_widget_add_css_class(name_label, "heading");
  gtk_label_set_xalign(GTK_LABEL(name_label), 0);
  gtk_box_append(GTK_BOX(box), name_label);
  p->name = gtk_entry_new();
  gtk_entry_set_max_length(GTK_ENTRY(p->name), MAX_NAME);
  ed->building = TRUE;
  gtk_editable_set_text(GTK_EDITABLE(p->name), g->parent ? (g->name ? g->name : "") : (ed->name ? ed->name : ""));
  ed->building = FALSE;
  gtk_entry_set_activates_default(GTK_ENTRY(p->name), TRUE);
  gtk_accessible_update_relation(GTK_ACCESSIBLE(p->name), GTK_ACCESSIBLE_RELATION_LABELLED_BY, name_label, NULL, -1);
  if (g->parent) g_signal_connect(p->name, "changed", G_CALLBACK(on_group_name_changed), p);
  else g_signal_connect(p->name, "changed", G_CALLBACK(on_list_name_changed), p);
  gtk_widget_set_sensitive(p->name, !ed->read_only);
  gtk_box_append(GTK_BOX(box), p->name);

  if (ed->read_only) {
    /* The rule as its sentence, and that it stays as it is. */
    GtkWidget *note = gtk_label_new(mc_str(MC_STR_SMART_READ_ONLY));
    gtk_label_set_wrap(GTK_LABEL(note), TRUE);
    gtk_label_set_xalign(GTK_LABEL(note), 0);
    gtk_widget_set_margin_top(note, 12);
    gtk_box_append(GTK_BOX(box), note);
    char *core = g_strdup(ed->old_rule), *sentence = NULL;
    gboolean described = core && mc_smart_describe(ed->lib, ed->subject, core, &sentence) == MC_OK;
    GtkWidget *text = gtk_label_new(described ? sentence : ed->old_rule);
    mc_free(sentence);
    g_free(core);
    gtk_label_set_wrap(GTK_LABEL(text), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(text), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_selectable(GTK_LABEL(text), TRUE);
    gtk_label_set_xalign(GTK_LABEL(text), 0);
    gtk_widget_set_margin_start(text, 12);
    gtk_widget_set_margin_end(text, 12);
    gtk_widget_set_margin_top(text, 12);
    gtk_widget_set_margin_bottom(text, 12);
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(card, "card");
    gtk_widget_set_margin_top(card, 6);
    gtk_box_append(GTK_BOX(card), text);
    gtk_box_append(GTK_BOX(box), card);
    p->list = boxed_list(); /* unused, so the rows have somewhere to be */
  } else {
    /* All, any or none of the following, and a row each. */
    const char *modes[] = {mc_str(MC_STR_SMART_MODE_ALL), mc_str(MC_STR_SMART_MODE_ANY),
                           mc_str(MC_STR_SMART_MODE_NONE), NULL};
    p->mode = drop_down(modes, mc_str(MC_STR_SMART_MODE_NAME));
    gtk_widget_set_halign(p->mode, GTK_ALIGN_START);
    gtk_widget_set_margin_top(p->mode, 12);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(p->mode), (guint)g->mode);
    g_signal_connect(p->mode, "notify::selected", G_CALLBACK(on_mode_changed), p);
    gtk_box_append(GTK_BOX(box), p->mode);
    p->list = boxed_list();
    p->placeholder = gtk_label_new(mc_str(g->parent ? MC_STR_SMART_GROUP_EMPTY_HINT : MC_STR_SMART_NO_CONDITIONS));
    gtk_label_set_wrap(GTK_LABEL(p->placeholder), TRUE);
    gtk_widget_add_css_class(p->placeholder, "dim-label");
    gtk_widget_set_margin_top(p->placeholder, 18);
    gtk_widget_set_margin_bottom(p->placeholder, 18);
    gtk_widget_set_margin_start(p->placeholder, 12);
    gtk_widget_set_margin_end(p->placeholder, 12);
    gtk_list_box_set_placeholder(GTK_LIST_BOX(p->list), p->placeholder);
    gtk_box_append(GTK_BOX(box), p->list);
    fill_rows(p);

    GtkWidget *adds = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    p->add_condition = add_button(MC_STR_SMART_ADD_CONDITION_BUTTON);
    g_signal_connect(p->add_condition, "clicked", G_CALLBACK(on_add_condition), p);
    gtk_box_append(GTK_BOX(adds), p->add_condition);
    GtkWidget *add_group = add_button(MC_STR_SMART_ADD_GROUP_BUTTON);
    g_signal_connect(add_group, "clicked", G_CALLBACK(on_add_group), p);
    gtk_box_append(GTK_BOX(adds), add_group);
    gtk_box_append(GTK_BOX(box), adds);

    if (!g->parent) gtk_box_append(GTK_BOX(box), build_order(p));

    /* What the list holds, or what the group matches on its own. */
    p->preview_heading = heading(mc_str(g->parent ? MC_STR_SMART_GROUP_PREVIEW : MC_STR_SMART_PREVIEW));
    gtk_box_append(GTK_BOX(box), p->preview_heading);
    gtk_widget_set_visible(p->preview_heading, !g->parent || g->kids->len);
    p->preview_note = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(p->preview_note), 0);
    gtk_widget_add_css_class(p->preview_note, "dim-label");
    gtk_widget_set_visible(p->preview_note, FALSE);
    gtk_box_append(GTK_BOX(box), p->preview_note);
    p->preview = boxed_list();
    set_label(p->preview, mc_str(g->parent ? MC_STR_SMART_GROUP_PREVIEW : MC_STR_SMART_PREVIEW));
    gtk_widget_set_visible(p->preview, FALSE);
    gtk_box_append(GTK_BOX(box), p->preview);
    p->preview_more = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(p->preview_more), 0);
    gtk_widget_add_css_class(p->preview_more, "dim-label");
    gtk_widget_set_visible(p->preview_more, FALSE);
    gtk_box_append(GTK_BOX(box), p->preview_more);
  }

  GtkWidget *clamp = adw_clamp_new();
  adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 720);
  adw_clamp_set_child(ADW_CLAMP(clamp), box);
  GtkWidget *scroller = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), clamp);
  gtk_widget_set_vexpand(scroller, TRUE);

  p->nav = adw_navigation_page_new(scroller, group_title(g));
  g_object_set_data_full(G_OBJECT(p->nav), "smart-page", p, page_free);
  g_signal_connect(p->nav, "shown", G_CALLBACK(on_shown), NULL);
  update_page(p);
  return p->nav;
}

/* Once a page has slid in, the focus goes where it was asked to. */
static void on_shown(AdwNavigationPage *nav, gpointer data) {
  (void)data;
  page *p = page_of(nav);
  if (p && p->focus) gtk_widget_grab_focus(p->focus);
  if (p) p->focus = NULL;
}

/* Opens g's page over the shown one, with the focus on its first condition (or Add
 * condition). */
static void push_group(node *g) {
  AdwNavigationPage *nav = page_new(g);
  page *p = page_of(nav);
  row *r = p->rows->len ? g_ptr_array_index(p->rows, 0) : NULL;
  p->focus = r ? (r->focus ? r->focus : r->widget) : p->add_condition;
  adw_navigation_view_push(ed->nav, nav);
}

/* --- The path at the top --- */

static void on_crumb(GtkButton *button, gpointer data) {
  (void)button;
  AdwNavigationPage *nav = data;
  adw_navigation_view_pop_to_page(ed->nav, nav);
}

/* List name › Group › Subgroup, each but the last a button back to it, marked where something
 * in it has a problem. Shown once a group is open. */
static void update_path(void) {
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(ed->path))) gtk_box_remove(GTK_BOX(ed->path), child);
  GListModel *stack = adw_navigation_view_get_navigation_stack(ed->nav);
  guint n = g_list_model_get_n_items(stack);
  for (guint i = 0; i < n; i++) {
    AdwNavigationPage *nav = g_list_model_get_item(stack, i);
    page *p = page_of(nav);
    node *g = page_group(p);
    if (g) {
      if (gtk_widget_get_first_child(ed->path)) {
        GtkWidget *sep = gtk_label_new("›");
        gtk_widget_add_css_class(sep, "dim-label");
        gtk_box_append(GTK_BOX(ed->path), sep);
      }
      GtkWidget *label = gtk_label_new(group_title(g));
      gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
      gtk_label_set_max_width_chars(GTK_LABEL(label), 24);
      GtkWidget *content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
      if (g->bad) {
        GtkWidget *warn = gtk_image_new_from_icon_name("dialog-warning-symbolic");
        gtk_widget_set_tooltip_text(warn, mc_str(MC_STR_SMART_GROUP_HAS_PROBLEM));
        gtk_box_append(GTK_BOX(content), warn);
        gtk_widget_add_css_class(content, "error");
      }
      gtk_box_append(GTK_BOX(content), label);
      if (i + 1 < n) {
        GtkWidget *b = gtk_button_new();
        gtk_button_set_child(GTK_BUTTON(b), content);
        gtk_widget_add_css_class(b, "flat");
        gtk_widget_set_tooltip_text(b, group_title(g));
        g_signal_connect(b, "clicked", G_CALLBACK(on_crumb), nav);
        gtk_box_append(GTK_BOX(ed->path), b);
      } else {
        gtk_widget_add_css_class(label, "heading");
        gtk_widget_set_margin_start(content, 10);
        gtk_box_append(GTK_BOX(ed->path), content);
      }
    }
    g_object_unref(nav);
  }
  g_object_unref(stack);
  gtk_widget_set_visible(ed->path, n > 1);
}

/* --- The problems' card and the counts --- */

static void on_show_problem(GtkButton *button, gpointer data) {
  (void)button;
  show_problem(GPOINTER_TO_UINT(data));
}

static void update_problems(void) {
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(ed->problems_list))) gtk_box_remove(GTK_BOX(ed->problems_list), child);
  guint n = ed->bad->len;
  gtk_widget_set_visible(ed->problems, n > 0);
  if (!n) return;
  char title[128];
  mc_format(title, sizeof title, mc_str_plural(MC_STR_SMART_PROBLEMS_TITLE, (long)n), MC_D(n));
  gtk_label_set_text(GTK_LABEL(ed->problems_title), title);
  for (guint i = 0; i < n && i < PROBLEM_ROWS; i++) {
    node *c = g_ptr_array_index(ed->bad, i);
    char *line = problem_line(c);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *label = gtk_label_new(line);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_widget_set_tooltip_text(label, line);
    gtk_box_append(GTK_BOX(box), label);
    GtkWidget *show = gtk_button_new_with_label(mc_str(MC_STR_SMART_PROBLEM_SHOW));
    gtk_widget_add_css_class(show, "flat");
    gtk_widget_set_valign(show, GTK_ALIGN_CENTER);
    gtk_accessible_update_relation(GTK_ACCESSIBLE(show), GTK_ACCESSIBLE_RELATION_DESCRIBED_BY, label, NULL, -1);
    g_signal_connect(show, "clicked", G_CALLBACK(on_show_problem), GUINT_TO_POINTER(c->id));
    gtk_box_append(GTK_BOX(box), show);
    gtk_box_append(GTK_BOX(ed->problems_list), box);
    g_free(line);
  }
  char more[64] = "";
  if (n > PROBLEM_ROWS) mc_format(more, sizeof more, mc_str_plural(MC_STR_SMART_MORE, (long)(n - PROBLEM_ROWS)), MC_D(n - PROBLEM_ROWS));
  gtk_label_set_text(GTK_LABEL(ed->problems_more), more);
  gtk_widget_set_visible(ed->problems_more, n > PROBLEM_ROWS);
}

/* "40 tracks match · 10 in the list", or Counting… while it's being worked out. */
static void update_status(void) {
  char text[256] = "";
  if (ed->read_only || ed->bad->len) {
  } else if (!ed->counted) {
    g_strlcpy(text, mc_str(MC_STR_SMART_COUNTING), sizeof text);
  } else if (ed->total >= 0) {
    enum mc_str_id id = ed->subject == MC_SMART_TRACKS ? MC_STR_SMART_TRACKS_MATCH : MC_STR_SMART_ALBUMS_MATCH;
    char part[128];
    mc_format(part, sizeof part, mc_str_plural(id, ed->total), MC_D(ed->total));
    if (ed->limited) {
      char in_list[128];
      mc_format(in_list, sizeof in_list, mc_str(MC_STR_SMART_IN_LIST), MC_D(MIN(ed->limit, ed->total)));
      g_snprintf(text, sizeof text, "%s%s%s", part, mc_str(MC_STR_SEPARATOR_DOT), in_list);
    } else {
      g_strlcpy(text, part, sizeof text);
    }
  }
  gtk_label_set_text(GTK_LABEL(ed->status), text);
  gtk_widget_set_visible(ed->status, *text);
  gtk_widget_set_visible(ed->bottom, *text || ed->bad->len);
}

static void update_save(void) {
  gtk_widget_set_sensitive(ed->save, !ed->read_only && ed->name && *ed->name && !ed->bad->len);
}

/* --- The preview thread --- */

static void job_free(job *j) {
  if (!j) return;
  g_free(j->list_rule);
  g_free(j->preview_rule);
  for (int i = 0; i < j->n; i++) g_free(j->rules[i]);
  g_free(j->rules);
  g_free(j->ids);
  g_free(j);
}

static void result_free(result *r) {
  mc_track_list_free(r->tracks);
  mc_album_list_free(r->albums);
  for (int i = 0; i < r->n; i++) g_free(r->rules[i]);
  g_free(r->rules);
  g_free(r->ids);
  g_free(r->counts);
  g_free(r);
}

static gboolean on_result(gpointer data);

static int count_of(mc_library *lib, mc_smart_subject subject, const char *rule) {
  int n;
  return rule && mc_smart_count(lib, subject, rule, &n) == MC_OK ? n : -1;
}

static gpointer worker_main(gpointer data) {
  worker *w = data;
  mc_library *lib = NULL;
  for (;;) {
    g_mutex_lock(&w->lock);
    while (!w->job && !w->quit) g_cond_wait(&w->wake, &w->lock);
    job *j = w->quit ? NULL : w->job;
    w->job = NULL;
    g_mutex_unlock(&w->lock);
    if (!j) break;
    if (!lib && mc_library_open(w->dir, &lib) != MC_OK) lib = NULL;
    result *r = g_new0(result, 1);
    r->serial = j->serial;
    r->gen = j->gen;
    r->total = -1;
    if (lib) {
      if (j->preview_rule)
        r->preview_ok = j->subject == MC_SMART_TRACKS ? mc_smart_preview_tracks(lib, j->preview_rule, &r->tracks) == MC_OK
                                                      : mc_smart_preview_albums(lib, j->preview_rule, &r->albums) == MC_OK;
      if (j->preview_is_list && r->preview_ok) r->total = r->tracks ? r->tracks->count : r->albums ? r->albums->count : 0;
      else r->total = count_of(lib, j->subject, j->list_rule);
      r->n = j->n;
      r->ids = j->ids;
      r->rules = j->rules;
      j->ids = NULL;
      j->rules = NULL;
      j->n = 0;
      r->counts = g_new(int, r->n > 0 ? r->n : 1);
      for (int i = 0; i < r->n; i++) {
        g_mutex_lock(&w->lock);
        gboolean stale = w->job || w->quit; /* a newer rule is waiting: these counts are wasted */
        g_mutex_unlock(&w->lock);
        r->counts[i] = stale ? -1 : count_of(lib, j->subject, r->rules[i]);
      }
    }
    job_free(j);
    g_idle_add(on_result, r);
  }
  if (lib) mc_library_close(lib);
  g_mutex_lock(&w->lock);
  job_free(w->job);
  g_mutex_unlock(&w->lock);
  g_mutex_clear(&w->lock);
  g_cond_clear(&w->wake);
  g_free(w->dir);
  g_free(w);
  return NULL;
}

static void start_worker(void) {
  worker *w = g_new0(worker, 1);
  g_mutex_init(&w->lock);
  g_cond_init(&w->wake);
  w->dir = g_strdup(mc_library_dir(ed->lib));
  GThread *t = g_thread_try_new("smart-preview", worker_main, w, NULL);
  if (!t) {
    g_mutex_clear(&w->lock);
    g_cond_clear(&w->wake);
    g_free(w->dir);
    g_free(w);
    return;
  }
  g_thread_unref(t); /* it frees w when told to quit */
  ed->w = w;
}

/* Tells the thread to stop after what it's doing; it frees itself. */
static void stop_worker(void) {
  worker *w = ed->w;
  if (!w) return;
  ed->w = NULL;
  g_mutex_lock(&w->lock);
  w->quit = TRUE;
  g_cond_signal(&w->wake);
  g_mutex_unlock(&w->lock);
}

/* The rule as it stands, and the shown page's rows, to the thread. */
static void submit(void) {
  if (!ed->w || ed->read_only) return;
  page *p = visible_page();
  node *g = page_group(p);
  if (!g) return;
  job *j = g_new0(job, 1);
  j->serial = ed->serial;
  j->gen = ++ed->gen;
  j->subject = ed->subject;
  char *rule = rule_json(FALSE);
  j->list_rule = g_strdup(rule);
  g_free(rule);
  j->preview_is_list = !g->parent;
  if (g->parent && g->kids->len) {
    char *m = match_json(g, FALSE);
    j->preview_rule = g_strdup(m);
    g_free(m);
  } else if (!g->parent) {
    j->preview_rule = g_strdup(j->list_rule);
  }
  j->ids = g_new0(guint, p->rows->len + 1);
  j->rules = g_new0(char *, p->rows->len + 1);
  for (guint i = 0; i < p->rows->len; i++) {
    row *r = g_ptr_array_index(p->rows, i);
    node *n = find(r->id);
    if (!n || (n->is_group && !n->kids->len)) continue;
    char *m = match_json(n, FALSE);
    if (!m) continue;
    j->ids[j->n] = n->id;
    j->rules[j->n++] = g_strdup(m);
    g_free(m);
  }
  ed->counted = FALSE;
  update_status();
  g_mutex_lock(&ed->w->lock);
  job_free(ed->w->job);
  ed->w->job = j;
  g_cond_signal(&ed->w->wake);
  g_mutex_unlock(&ed->w->lock);
}

static gboolean on_result(gpointer data) {
  result *r = data;
  if (!ed || r->serial != ed->serial) {
    result_free(r);
    return G_SOURCE_REMOVE;
  }
  /* Counts keep, whenever they come: each is for the rule it was made from. */
  for (int i = 0; i < r->n; i++) {
    node *n = find(r->ids[i]);
    if (!n || r->counts[i] < 0) continue;
    g_free(n->counted);
    n->counted = r->rules[i];
    r->rules[i] = NULL;
    n->count = r->counts[i];
  }
  page *p = visible_page();
  if (r->gen == ed->gen) {
    ed->counted = TRUE;
    ed->total = r->total;
    node *g = page_group(p);
    if (p && g && p->preview) {
      int n = r->preview_ok ? (r->tracks ? r->tracks->count : r->albums ? r->albums->count : 0) : 0;
      if (!g->parent && ed->limited) n = MIN(n, ed->limit);
      fill_preview(p, r->tracks, r->albums, n);
      char note[128] = "";
      if (g->parent && r->preview_ok)
        mc_format(note, sizeof note,
                  mc_str_plural(ed->subject == MC_SMART_TRACKS ? MC_STR_SMART_GROUP_TRACKS : MC_STR_SMART_GROUP_ALBUMS, n),
                  MC_D(n));
      gtk_label_set_text(GTK_LABEL(p->preview_note), note);
      gtk_widget_set_visible(p->preview_note, *note);
      gtk_widget_set_visible(p->preview_heading, !g->parent || g->kids->len);
    }
    update_status();
  }
  if (p) update_page(p);
  result_free(r);
  return G_SOURCE_REMOVE;
}

/* --- Changes --- */

/* Whether g holds a condition without a problem: else its sentence would be "All tracks". */
static gboolean says_something(const node *g) {
  for (guint i = 0; i < g->kids->len; i++) {
    const node *k = kid(g, i);
    if (k->is_group ? says_something(k) : !cond_problem(k)) return TRUE;
  }
  return FALSE;
}

/* Every group's sentence, for its row, its page's placeholder, the path and the problems. */
static void describe_groups(node *g) {
  for (guint i = 0; i < g->kids->len; i++) {
    node *k = kid(g, i);
    if (!k->is_group) continue;
    describe_groups(k);
    g_free(k->desc);
    char *m = says_something(k) ? match_json(k, TRUE) : NULL;
    k->desc = m ? describe(m) : NULL;
    g_free(m);
  }
}

/* A quarter of a second after the last change: the sentences, then the counts and preview. */
static gboolean on_timer(gpointer data) {
  (void)data;
  ed->timer = 0;
  describe_groups(ed->root);
  page *p = visible_page();
  if (p) update_page(p);
  update_path();
  update_problems();
  submit();
  return G_SOURCE_REMOVE;
}

static void schedule(void) {
  if (ed->timer) g_source_remove(ed->timer);
  ed->timer = g_timeout_add(PREVIEW_DELAY_MS, on_timer, NULL);
}

/* After any edit: the problems, Save, the shown page's marks, and soon the rest. */
static void changed(void) {
  if (!ed || ed->read_only) return;
  g_ptr_array_set_size(ed->bad, 0);
  check_group(ed->root);
  page *p = visible_page();
  if (p) update_page(p);
  update_problems();
  update_path();
  update_save();
  ed->counted = FALSE;
  update_status();
  schedule();
}

/* The navigation moved: the path, and the new page's counts. */
static void on_visible_page(GObject *nav, GParamSpec *pspec, gpointer data) {
  (void)nav;
  (void)pspec;
  (void)data;
  if (!ed || ed->read_only) return;
  page *p = visible_page();
  if (p) update_page(p);
  update_path();
  if (ed->timer) g_source_remove(ed->timer);
  ed->timer = 0;
  submit();
}

/* Show on a problem: its group's page (and those above it), with the focus on its value. */
static void show_problem(guint id) {
  node *c = find(id);
  if (!c) return;
  GPtrArray *groups = g_ptr_array_new();
  for (node *g = c->parent; g; g = g->parent) g_ptr_array_insert(groups, 0, g);
  AdwNavigationPage **pages = g_new0(AdwNavigationPage *, groups->len);
  for (guint i = 0; i < groups->len; i++) pages[i] = page_new(g_ptr_array_index(groups, i));
  adw_navigation_view_replace(ed->nav, pages, (int)groups->len);
  page *p = page_of(pages[groups->len - 1]);
  g_free(pages);
  g_ptr_array_free(groups, TRUE);
  row *r = row_of(p, id);
  if (r) gtk_widget_grab_focus(r->focus ? r->focus : r->field);
  if (r) p->focus = r->focus ? r->focus : r->field; /* again, should the page only now be shown */
}

/* --- Saving --- */

static void fail(const char *heading) {
  AdwDialog *alert = adw_alert_dialog_new(heading, NULL);
  adw_alert_dialog_add_response(ADW_ALERT_DIALOG(alert), "close", mc_str_mnemonic(MC_STR_CLOSE));
  adw_dialog_present(alert, GTK_WIDGET(ed->dialog));
}

static void on_save(GtkButton *button, gpointer data) {
  (void)button;
  (void)data;
  if (!ed || !gtk_widget_get_sensitive(ed->save)) return;
  char *rule = rule_json(TRUE), *core = g_strdup(rule);
  gboolean ok = ed->name && *ed->name && core && mc_smart_validate(ed->subject, core) == MC_OK;
  gboolean library_changed = FALSE;
  enum mc_str_id failed = ed->creating ? MC_STR_SMART_CREATE_FAILED : MC_STR_SMART_SAVE_FAILED;
  if (ok && ed->creating) {
    ok = mc_smart_create(ed->lib, ed->name, ed->subject, core, ed->uid) == MC_OK;
    library_changed = ok;
  } else if (ok) {
    if (!ed->old_name || strcmp(ed->name, ed->old_name)) {
      ok = mc_smart_rename(ed->lib, ed->uid, ed->name) == MC_OK;
      if (!ok) failed = MC_STR_SMART_RENAME_FAILED;
      library_changed |= ok;
    }
    if (ok && (!ed->old_rule || strcmp(rule, ed->old_rule))) {
      ok = mc_smart_set_rule(ed->lib, ed->uid, core) == MC_OK;
      library_changed |= ok;
    }
  }
  g_free(rule);
  g_free(core);
  if (!ok) {
    fail(mc_str(failed));
    return;
  }
  void (*done)(void *, const char *) = ed->done;
  void *user = ed->user;
  char uid[MC_UID_STRLEN];
  g_strlcpy(uid, ed->uid, sizeof uid);
  adw_dialog_force_close(ed->dialog); /* frees ed */
  if (library_changed && done) done(user, uid);
}

/* --- The dialog --- */

/* Escape, and Cancel: back a group, or on the list's page, the dialog closes. */
static void on_close_attempt(AdwDialog *dialog, gpointer data) {
  (void)data;
  if (ed && adw_navigation_view_get_previous_page(ed->nav, adw_navigation_view_get_visible_page(ed->nav)))
    adw_navigation_view_pop(ed->nav);
  else
    adw_dialog_force_close(dialog);
}

static void on_cancel(GtkButton *button, gpointer data) {
  (void)button;
  adw_dialog_force_close(data);
}

static void on_closed(AdwDialog *dialog, gpointer data) {
  (void)dialog;
  (void)data;
  if (!ed) return;
  if (ed->timer) g_source_remove(ed->timer);
  stop_worker();
  editor *e = ed;
  node_free(e->root); /* while ed is set, which takes them out of the table */
  g_hash_table_destroy(e->nodes);
  g_array_free(e->sorts, TRUE);
  g_ptr_array_free(e->bad, TRUE);
  mc_playlist_list_free(e->playlists);
  mc_album_group_list_free(e->groups);
  g_free(e->old_name);
  g_free(e->old_rule);
  g_free(e->name);
  g_free(e);
  ed = NULL;
}

/* The dialog around the pages: Cancel and Save, the path, and below, the problems and counts. */
static void build_dialog(const char *title) {
  ed->dialog = adw_dialog_new();
  adw_dialog_set_title(ed->dialog, title);
  adw_dialog_set_content_width(ed->dialog, 720);
  adw_dialog_set_content_height(ed->dialog, 760);
  adw_dialog_set_can_close(ed->dialog, FALSE); /* Escape goes back a group first */
  g_signal_connect(ed->dialog, "close-attempt", G_CALLBACK(on_close_attempt), NULL);
  g_signal_connect(ed->dialog, "closed", G_CALLBACK(on_closed), NULL);

  GtkWidget *header = adw_header_bar_new();
  adw_header_bar_set_show_start_title_buttons(ADW_HEADER_BAR(header), FALSE);
  adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(header), FALSE);
  ed->cancel = gtk_button_new_with_mnemonic(mc_str_mnemonic(ed->read_only ? MC_STR_CLOSE : MC_STR_CANCEL));
  g_signal_connect(ed->cancel, "clicked", G_CALLBACK(on_cancel), ed->dialog);
  adw_header_bar_pack_start(ADW_HEADER_BAR(header), ed->cancel);
  ed->save = gtk_button_new_with_mnemonic(mc_str_mnemonic(ed->creating ? MC_STR_CREATE : MC_STR_SAVE));
  gtk_widget_add_css_class(ed->save, "suggested-action");
  gtk_widget_set_sensitive(ed->save, FALSE);
  gtk_widget_set_visible(ed->save, !ed->read_only);
  g_signal_connect(ed->save, "clicked", G_CALLBACK(on_save), NULL);
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), ed->save);
  adw_dialog_set_default_widget(ed->dialog, ed->save);

  ed->path = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
  set_label(ed->path, mc_str(MC_STR_SMART_PATH_NAME));
  gtk_widget_set_margin_start(ed->path, 6);
  gtk_widget_set_margin_end(ed->path, 6);
  gtk_widget_set_margin_bottom(ed->path, 6);
  gtk_widget_set_visible(ed->path, FALSE);

  ed->nav = ADW_NAVIGATION_VIEW(adw_navigation_view_new());
  g_signal_connect(ed->nav, "notify::visible-page", G_CALLBACK(on_visible_page), NULL);

  ed->problems_title = gtk_label_new(NULL);
  gtk_widget_add_css_class(ed->problems_title, "heading");
  gtk_widget_add_css_class(ed->problems_title, "error");
  gtk_label_set_xalign(GTK_LABEL(ed->problems_title), 0);
  ed->problems_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_accessible_update_relation(GTK_ACCESSIBLE(ed->problems_list), GTK_ACCESSIBLE_RELATION_LABELLED_BY,
                                 ed->problems_title, NULL, -1);
  ed->problems_more = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(ed->problems_more), 0);
  gtk_widget_add_css_class(ed->problems_more, "dim-label");
  GtkWidget *inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_start(inner, 12);
  gtk_widget_set_margin_end(inner, 6);
  gtk_widget_set_margin_top(inner, 10);
  gtk_widget_set_margin_bottom(inner, 10);
  gtk_box_append(GTK_BOX(inner), ed->problems_title);
  gtk_box_append(GTK_BOX(inner), ed->problems_list);
  gtk_box_append(GTK_BOX(inner), ed->problems_more);
  GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(card, "card");
  gtk_box_append(GTK_BOX(card), inner);
  ed->problems = card;
  gtk_widget_set_visible(card, FALSE);
  ed->status = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(ed->status), 0);
  gtk_label_set_ellipsize(GTK_LABEL(ed->status), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class(ed->status, "dim-label");
  GtkWidget *bottom = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_start(bottom, 12);
  gtk_widget_set_margin_end(bottom, 12);
  gtk_widget_set_margin_top(bottom, 8);
  gtk_widget_set_margin_bottom(bottom, 10);
  gtk_box_append(GTK_BOX(bottom), card);
  gtk_box_append(GTK_BOX(bottom), ed->status);
  ed->bottom = bottom;
  gtk_widget_set_visible(bottom, FALSE);

  GtkWidget *view = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), ed->path);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), GTK_WIDGET(ed->nav));
  adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(view), bottom);
  adw_toolbar_view_set_bottom_bar_style(ADW_TOOLBAR_VIEW(view), ADW_TOOLBAR_RAISED);
  adw_dialog_set_child(ed->dialog, view);
}

/* A new editor for subject, with the fields and the pickers' playlists and groups. */
static gboolean editor_new(mc_library *lib, mc_smart_subject subject) {
  if (ed || !lib) return FALSE; /* one at a time */
  ed = g_new0(editor, 1);
  ed->serial = ++editor_serial;
  ed->lib = lib;
  ed->subject = subject;
  ed->nodes = g_hash_table_new(NULL, NULL);
  ed->sorts = g_array_new(FALSE, FALSE, sizeof(int));
  ed->bad = g_ptr_array_new();
  ed->limit = subject == MC_SMART_TRACKS ? 25 : 10;
  ed->seed = g_random_int_range(0, G_MAXINT32); /* should Shuffled be turned on */
  ed->total = -1;
  for (int i = 0; i < mc_smart_field_count(subject) && ed->nfields < (int)G_N_ELEMENTS(ed->fields); i++) {
    if (!mc_smart_field_get(subject, i, &ed->fields[ed->nfields])) continue;
    if (ed->fields[ed->nfields].sortable && mc_smart_order_label(ed->fields[ed->nfields].name, 0))
      ed->sortable[ed->nsortable++] = ed->nfields;
    ed->nfields++;
  }
  if (mc_library_list_playlists(lib, 0, &ed->playlists) != MC_OK) ed->playlists = NULL;
  if (mc_library_list_album_groups(lib, &ed->groups) != MC_OK) ed->groups = NULL;
  ed->root = node_new(NULL, TRUE);
  return TRUE;
}

/* Fills the editor from rule (or makes it read-only) and shows the list's page. */
static void start(const char *name, const char *rule) {
  g_free(ed->name);
  ed->name = g_strdup(name ? name : "");
  if (!load_rule(rule)) {
    ed->read_only = TRUE;
    gtk_widget_set_visible(ed->save, FALSE);
    gtk_button_set_label(GTK_BUTTON(ed->cancel), mc_str_mnemonic(MC_STR_CLOSE));
  }
  g_free(ed->old_rule);
  ed->old_rule = ed->creating ? NULL : ed->read_only ? g_strdup(rule) : rule_json(TRUE); /* as written here */
  if (ed->creating && !ed->root->kids->len) cond_new(ed->root); /* a blank rule: one condition to fill in */
  if (!ed->read_only) {
    describe_groups(ed->root);
    start_worker();
  }
  AdwNavigationPage *root = page_new(ed->root);
  AdwNavigationPage *pages[] = {root};
  adw_navigation_view_replace(ed->nav, pages, 1);
  changed();
  page *p = page_of(root);
  gtk_widget_grab_focus(p->name);
}

/* --- Ready-made rules --- */

typedef struct preset {
  enum mc_str_id name;
  const char *rule;
} preset;

static const preset k_track_presets[] = {
  {MC_STR_SMART_PRESET_RECENT,
   "{\"v\":1,\"match\":{\"all\":[{\"field\":\"added\",\"op\":\"within\",\"value\":{\"days\":30}}]},"
   "\"order\":[{\"field\":\"added\",\"dir\":\"desc\"}]}"},
  {MC_STR_SMART_PRESET_LEAST_PLAYED,
   "{\"v\":1,\"match\":{\"all\":[{\"field\":\"on_device\",\"op\":\"is\",\"value\":true}]},"
   "\"order\":[{\"field\":\"plays\",\"dir\":\"asc\"},{\"field\":\"last_played\",\"dir\":\"asc\"}],\"limit\":100}"},
  {MC_STR_SMART_PRESET_NOT_PLAYED,
   "{\"v\":1,\"match\":{\"all\":[{\"field\":\"last_played\",\"op\":\"not_within\",\"value\":{\"days\":180}}]},"
   "\"order\":[{\"field\":\"last_played\",\"dir\":\"asc\"}]}"},
  {MC_STR_SMART_PRESET_FLAC,
   "{\"v\":1,\"match\":{\"all\":[{\"field\":\"on_device\",\"op\":\"is\",\"value\":true},"
   "{\"field\":\"format\",\"op\":\"is\",\"value\":\"flac\"}]}}"},
  {MC_STR_SMART_PRESET_BLANK, "{\"v\":1}"},
};

static const preset k_album_presets[] = {
  {MC_STR_SMART_PRESET_RECENT_ALBUMS,
   "{\"v\":1,\"match\":{\"all\":[{\"field\":\"added\",\"op\":\"within\",\"value\":{\"days\":30}}]},"
   "\"order\":[{\"field\":\"added\",\"dir\":\"desc\"}]}"},
  {MC_STR_SMART_PRESET_LEAST_PLAYED_ALBUMS,
   "{\"v\":1,\"match\":{\"all\":[{\"field\":\"on_device\",\"op\":\"is\",\"value\":true}]},"
   "\"order\":[{\"field\":\"plays\",\"dir\":\"asc\"},{\"field\":\"last_played\",\"dir\":\"asc\"}],\"limit\":20}"},
  {MC_STR_SMART_PRESET_NOT_PLAYED_ALBUMS,
   "{\"v\":1,\"match\":{\"all\":[{\"field\":\"last_played\",\"op\":\"not_within\",\"value\":{\"days\":180}}]},"
   "\"order\":[{\"field\":\"last_played\",\"dir\":\"asc\"}]}"},
  {MC_STR_SMART_PRESET_FLAC_ALBUMS,
   "{\"v\":1,\"match\":{\"all\":[{\"field\":\"on_device\",\"op\":\"is\",\"value\":true},"
   "{\"field\":\"format\",\"op\":\"is\",\"value\":\"flac\"}]}}"},
  {MC_STR_SMART_PRESET_BLANK, "{\"v\":1}"},
};

static void on_preset(AdwActionRow *w, gpointer data) {
  (void)w;
  const preset *p = data;
  if (!ed) return;
  start(p->name == MC_STR_SMART_PRESET_BLANK ? "" : mc_str(p->name), p->rule);
}

/*  Start from
 *  | Recently added                             > |
 *  |   Tracks where date added is in the last ... |
 *  | ...                                          |
 *  | Blank                                      > |  */
static AdwNavigationPage *presets_page(void) {
  const preset *presets = ed->subject == MC_SMART_TRACKS ? k_track_presets : k_album_presets;
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(box, 12);
  gtk_widget_set_margin_end(box, 12);
  gtk_widget_set_margin_top(box, 12);
  gtk_widget_set_margin_bottom(box, 18);
  GtkWidget *title = gtk_label_new(mc_str(MC_STR_SMART_PRESET_TITLE));
  gtk_widget_add_css_class(title, "heading");
  gtk_label_set_xalign(GTK_LABEL(title), 0);
  gtk_box_append(GTK_BOX(box), title);
  GtkWidget *list = boxed_list();
  gtk_accessible_update_relation(GTK_ACCESSIBLE(list), GTK_ACCESSIBLE_RELATION_LABELLED_BY, title, NULL, -1);
  for (size_t i = 0; i < G_N_ELEMENTS(k_track_presets); i++) {
    GtkWidget *r = adw_action_row_new();
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(r), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(r), mc_str(presets[i].name));
    char *sentence = presets[i].name == MC_STR_SMART_PRESET_BLANK ? NULL : describe(presets[i].rule);
    if (sentence) adw_action_row_set_subtitle(ADW_ACTION_ROW(r), sentence);
    g_free(sentence);
    adw_action_row_add_suffix(ADW_ACTION_ROW(r), gtk_image_new_from_icon_name("go-next-symbolic"));
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(r), TRUE);
    g_signal_connect(r, "activated", G_CALLBACK(on_preset), (gpointer)&presets[i]);
    gtk_list_box_append(GTK_LIST_BOX(list), r);
  }
  gtk_box_append(GTK_BOX(box), list);
  GtkWidget *clamp = adw_clamp_new();
  adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 720);
  adw_clamp_set_child(ADW_CLAMP(clamp), box);
  GtkWidget *scroller = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), clamp);
  AdwNavigationPage *nav = adw_navigation_page_new(scroller, mc_str(MC_STR_SMART_PRESET_TITLE));
  g_object_set_data(G_OBJECT(nav), "first-row", gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), 0));
  return nav;
}

void smart_editor_new(GtkWidget *parent, mc_library *lib, mc_smart_subject subject,
                      void (*done)(void *user, const char *uid), void *user) {
  if (!editor_new(lib, subject)) return;
  ed->creating = TRUE;
  ed->done = done;
  ed->user = user;
  build_dialog(mc_str(subject == MC_SMART_TRACKS ? MC_STR_SMART_NEW_PLAYLIST_TITLE : MC_STR_SMART_NEW_GROUP_TITLE));
  AdwNavigationPage *nav = presets_page();
  adw_navigation_view_add(ed->nav, nav);
  adw_dialog_present(ed->dialog, parent);
  GtkWidget *first = g_object_get_data(G_OBJECT(nav), "first-row");
  if (first) gtk_widget_grab_focus(first);
}

void smart_editor_edit(GtkWidget *parent, mc_library *lib, const char *uid,
                       void (*done)(void *user, const char *uid), void *user) {
  mc_smart_list *list = NULL;
  if (!lib || !uid || mc_library_list_smart(lib, 0, &list) != MC_OK) return;
  const mc_smart_info *info = NULL;
  for (int i = 0; i < list->count && !info; i++)
    if (!strcmp(list->items[i].uid, uid)) info = &list->items[i];
  if (info && editor_new(lib, info->subject)) {
    g_strlcpy(ed->uid, uid, sizeof ed->uid);
    ed->old_name = g_strdup(info->name ? info->name : "");
    ed->done = done;
    ed->user = user;
    build_dialog(mc_str(info->subject == MC_SMART_TRACKS ? MC_STR_SMART_EDIT_PLAYLIST_TITLE : MC_STR_SMART_EDIT_GROUP_TITLE));
    start(info->name, info->rule ? info->rule : "{\"v\":1}");
    adw_dialog_present(ed->dialog, parent);
    page *p = visible_page();
    if (p) gtk_widget_grab_focus(ed->read_only ? ed->cancel : p->name);
  }
  mc_smart_list_free(list);
}

#include "pair_dialog.h"

#include <string.h>

#if !ADW_CHECK_VERSION(1, 5, 0)
#error "pair_dialog.c needs libadwaita 1.5 or later (AdwDialog)"
#endif

/* Wrong codes before the responder draws a new one (MC_PAIR_MAX_FAILURES in core's private
 * pair.h). A wrong-code result with this many tries left means a new code is showing. */
#define MAX_FAILURES 3

/* settings.json's record that this computer showed which device is the archive. */
#define ARCHIVE_EXPLAINED_KEY "archive_explained"

/* What the ended page's button above OK does. */
typedef enum ended_action {
  ACT_NONE,
  ACT_CHOOSE,      /* Choose What to Sync: the page of the device that joined */
  ACT_NEW_LIBRARY, /* Create a New Library, then offer to pair again */
  ACT_PAIR_AGAIN,  /* Pair Again, after that */
} ended_action;

/* One open dialog. Freed when it closes. */
typedef struct pair_dlg {
  AdwDialog *dialog;
  mc_sync *sync;
  gboolean initiator;
  gboolean checking; /* the code went to the other device; waiting to hear */
  gboolean ended;    /* shows how the pairing ended */
  gboolean quiet;    /* closed by pair_dialog_close_all() or on the end: don't cancel */
  char peer[MC_SYNC_DEVICE_NAME_MAX + 1];
  GtkWidget *parent;            /* what it was presented over: the main window */
  AdwHeaderBar *header;
  GtkWidget *cancel, *pair;     /* the header bar's buttons */
  GtkWidget *stack;             /* "ask" | "ended" */
  GtkWidget *text, *code, *status;
  GtkWidget *ended_icon, *ended_title, *ended_text, *action, *ok;
  ended_action next;
  char target[MC_UID_STRLEN];   /* ACT_CHOOSE's device; the other device for the rest */
  char host[MC_SYNC_ADDR_STRLEN]; /* ACT_PAIR_AGAIN: where it was, "" if not known */
  guint16 port;
} pair_dlg;

static struct {
  pair_dlg *enter; /* the enter-code dialog (initiator), or NULL */
  pair_dlg *show;  /* the show-code dialog (responder), or NULL */
  gboolean enter_cancelled, show_cancelled; /* Cancel was pressed: the CANCELLED to come is ours */
  const pair_dialog_host *host;
} s;

/* --- Texts --- */

/* The other device's name, or a stand-in when it's not known. */
static const char *peer_name(const char *name, gboolean sentence_start) {
  if (name && *name) return name;
  return mc_str(sentence_start ? MC_STR_OTHER_DEVICE_START : MC_STR_SYNC_OTHER_DEVICE);
}

/* The string id with the other device's name (or a stand-in) as its {0}; g_free it. */
static char *peer_text(enum mc_str_id id, const char *name, gboolean sentence_start) {
  char *text = mc_format_alloc(mc_str(id), MC_S(peer_name(name, sentence_start)));
  char *out = g_strdup(text ? text : "");
  mc_free(text);
  return out;
}

/* prefix_id ("Wrong code entered. {0}") with how many tries are left as its {0}; g_free it. */
static char *tries_left(enum mc_str_id prefix_id, int left) {
  char tries[64];
  mc_format(tries, sizeof tries, mc_str_plural(MC_STR_TRIES_LEFT, left), MC_D(left));
  char *text = mc_format_alloc(mc_str(prefix_id), MC_S(tries));
  char *out = g_strdup(text ? text : "");
  mc_free(text);
  return out;
}

/* Whether this computer could have joined: it's in no group, so a new, empty library lets it. */
static gboolean could_join(void) {
  mc_sync_lib_state state;
  mc_sync *sync = sync_bridge_get();
  return sync && s.host && s.host->new_library && mc_sync_library_state(sync, &state) == MC_OK &&
         state == MC_SYNC_LIBRARY_NO_GROUP;
}

/* text as Pango markup, its "**archive**" in bold; g_free it. */
static char *bold_marks(const char *text) {
  char *escaped = g_markup_escape_text(text, -1);
  GString *out = g_string_new(NULL);
  gboolean bold = FALSE;
  for (const char *p = escaped; *p; p++) {
    if (p[0] == '*' && p[1] == '*') {
      g_string_append(out, bold ? "</b>" : "<b>");
      bold = !bold;
      p++;
    } else {
      g_string_append_c(out, *p);
    }
  }
  if (bold) g_string_append(out, "</b>");
  g_free(escaped);
  return g_string_free(out, FALSE);
}

/* Once, on this computer after it joined a group: which device keeps the originals, as
 * markup; NULL if the archive isn't known or it was shown before. g_free it. */
static char *archive_explainer(const sync_event *ev) {
  if (!ev->joined || !ev->archive_name[0] || !s.host || !s.host->first_time) return NULL;
  if (!s.host->first_time(ARCHIVE_EXPLAINED_KEY)) return NULL;
  char *text = ev->archives > 1 ? mc_format_alloc(mc_str_plural(MC_STR_ARCHIVE_EXPLAINER_SEVERAL, ev->archives - 1),
                                                  MC_S(ev->archive_name), MC_D(ev->archives - 1))
                                : mc_format_alloc(mc_str(MC_STR_ARCHIVE_EXPLAINER), MC_S(ev->archive_name));
  char *markup = bold_marks(text ? text : "");
  mc_free(text);
  return markup;
}

/* How a pairing ended, as on Windows and Android; g_free it. */
static char *ended_text(const sync_event *ev, gboolean initiator, const char *name) {
  switch (ev->reason) {
    case MC_PAIR_PAIRED:
      /* Nothing syncs yet: the device that joined has its sync settings to choose. */
      return ev->joined ? peer_text(MC_STR_LINUX_COMPUTER_PAIRED_TEXT, name, FALSE)
                        : peer_text(MC_STR_PAIRED_PEER_TEXT, name, TRUE);
    case MC_PAIR_WRONG_CODE: return g_strdup(mc_str(MC_STR_PAIR_WRONG_CODE_TOO_OFTEN));
    /* Both are in groups, different ones (a device in no group just joins). */
    case MC_PAIR_IN_GROUP: return peer_text(MC_STR_LINUX_COMPUTER_PAIR_IN_GROUP, name, FALSE);
    case MC_PAIR_LIBRARY_MISMATCH: {
      char *text = initiator ? peer_text(MC_STR_LINUX_COMPUTER_PAIR_LIBRARY_MISMATCH, name, FALSE)
                             : peer_text(MC_STR_LINUX_COMPUTER_PAIR_LIBRARY_MISMATCH_INCOMING, name, TRUE);
      if (!could_join()) return text;
      char *joined = g_strdup_printf("%s\n\n%s", text, mc_str(MC_STR_PAIR_NEW_LIBRARY_HINT));
      g_free(text);
      return joined;
    }
    case MC_PAIR_ALREADY_PAIRED: return peer_text(MC_STR_LINUX_COMPUTER_PAIR_ALREADY_PAIRED, name, FALSE);
    case MC_PAIR_BUSY: return peer_text(MC_STR_PAIR_BUSY, name, TRUE);
    case MC_PAIR_CANCELLED: return g_strdup(mc_str(MC_STR_PAIR_CANCELLED));
    case MC_PAIR_FAILED:
    default: return peer_text(MC_STR_PAIR_UNREACHABLE, name, FALSE);
  }
}

/* "123456" as "123 456"; g_free it. */
static char *spaced_code(const char *code) {
  GString *out = g_string_new(NULL);
  for (int i = 0; code[i] && i < MC_PAIR_CODE_LEN; i++) {
    if (i == MC_PAIR_CODE_LEN / 2) g_string_append_c(out, ' ');
    g_string_append_c(out, code[i]);
  }
  return g_string_free(out, FALSE);
}

/* --- The dialog --- */

static void set_status(pair_dlg *d, const char *text) {
  gtk_label_set_text(GTK_LABEL(d->status), text ? text : "");
  gtk_widget_set_visible(d->status, text && *text);
}

/* The digits typed, if there are all six. */
static gboolean code_text(pair_dlg *d, char *code) {
  const char *text = gtk_editable_get_text(GTK_EDITABLE(d->code));
  if (strlen(text) != MC_PAIR_CODE_LEN) return FALSE;
  for (int i = 0; i < MC_PAIR_CODE_LEN; i++) {
    if (!g_ascii_isdigit(text[i])) return FALSE;
    code[i] = text[i];
  }
  code[MC_PAIR_CODE_LEN] = '\0';
  return TRUE;
}

static void update_pair(pair_dlg *d) {
  char code[MC_PAIR_CODE_LEN + 1];
  if (d->pair) gtk_widget_set_sensitive(d->pair, !d->ended && !d->checking && code_text(d, code));
}

/* The enter-code dialog, ready for (another) code. */
static void ready_for_code(pair_dlg *d) {
  d->checking = FALSE;
  gtk_widget_set_sensitive(d->code, TRUE);
  gtk_editable_set_text(GTK_EDITABLE(d->code), "");
  update_pair(d);
  gtk_widget_grab_focus(d->code);
}

static void close_quietly(pair_dlg *d) {
  if (!d) return;
  d->quiet = TRUE;
  adw_dialog_force_close(d->dialog);
}

/* Turns the dialog into a status page: icon, title, markup, and OK (labelled ok_id), with
 * the action button above it unless next is ACT_NONE. */
static void show_message(pair_dlg *d, const char *icon, gboolean good, const char *title, const char *markup,
                         ended_action next, enum mc_str_id action_id, enum mc_str_id ok_id) {
  d->ended = TRUE;
  d->checking = FALSE;
  d->next = next;
  adw_dialog_set_title(d->dialog, title);
  gtk_image_set_from_icon_name(GTK_IMAGE(d->ended_icon), icon);
  gtk_widget_remove_css_class(d->ended_icon, good ? "warning" : "success");
  gtk_widget_add_css_class(d->ended_icon, good ? "success" : "warning");
  gtk_label_set_text(GTK_LABEL(d->ended_title), title);
  gtk_label_set_markup(GTK_LABEL(d->ended_text), markup);
  gtk_widget_set_visible(d->cancel, FALSE);
  if (d->pair) gtk_widget_set_visible(d->pair, FALSE);
  adw_header_bar_set_show_end_title_buttons(d->header, TRUE);
  gtk_button_set_label(GTK_BUTTON(d->ok), mc_str_mnemonic(ok_id));
  gtk_widget_set_visible(d->action, next != ACT_NONE);
  if (next != ACT_NONE) gtk_button_set_label(GTK_BUTTON(d->action), mc_str_mnemonic(action_id));
  /* The next step is the suggested one, except Create a New Library, which is a bigger one. */
  gboolean suggest = next != ACT_NONE && next != ACT_NEW_LIBRARY;
  GtkWidget *def = suggest ? d->action : d->ok;
  if (suggest) {
    gtk_widget_add_css_class(d->action, "suggested-action");
    gtk_widget_remove_css_class(d->ok, "suggested-action");
  } else {
    gtk_widget_remove_css_class(d->action, "suggested-action");
    gtk_widget_add_css_class(d->ok, "suggested-action");
  }
  gtk_stack_set_visible_child_name(GTK_STACK(d->stack), "ended");
  adw_dialog_set_default_widget(d->dialog, def);
  adw_dialog_set_focus(d->dialog, def);
}

/* Shows how the pairing ended, with OK and a button for the next step: Choose What to Sync
 * after pairing, Create a New Library when this computer has music of its own but could
 * otherwise join. */
static void show_ended(pair_dlg *d, const sync_event *ev) {
  const char *name = ev->name[0] ? ev->name : d->peer;
  if (ev->name[0]) g_strlcpy(d->peer, ev->name, sizeof d->peer);
  char *text = ended_text(ev, d->initiator, name);
  char *markup = g_markup_escape_text(text, -1);
  g_free(text);
  gboolean paired = ev->reason == MC_PAIR_PAIRED;
  if (paired) {
    char *explainer = archive_explainer(ev);
    if (explainer) {
      char *both = g_strdup_printf("%s\n\n%s", markup, explainer);
      g_free(markup);
      g_free(explainer);
      markup = both;
    }
  }
  ended_action next = ACT_NONE;
  enum mc_str_id action_id = MC_STR_OK;
  if (paired && s.host && s.host->show_device) {
    mc_sync *sync = sync_bridge_get();
    const char *uid = ev->joined ? (sync ? mc_sync_device_uid(sync) : "") : ev->uid;
    if (uid && *uid) {
      g_strlcpy(d->target, uid, sizeof d->target);
      next = ACT_CHOOSE;
      action_id = MC_STR_CHOOSE_WHAT_TO_SYNC;
    }
  } else if (ev->reason == MC_PAIR_LIBRARY_MISMATCH && could_join()) {
    if (ev->uid[0]) g_strlcpy(d->target, ev->uid, sizeof d->target); /* else as set_target said */
    next = ACT_NEW_LIBRARY;
    action_id = MC_STR_PAIR_CREATE_LIBRARY;
  }
  show_message(d, paired ? "object-select-symbolic" : "dialog-warning-symbolic", paired,
               mc_str(paired ? MC_STR_PAIRED : MC_STR_PAIR_FAILED), markup, next, action_id, MC_STR_OK);
  g_free(markup);
}

static void on_cancel_clicked(GtkButton *button, gpointer data) {
  (void)button;
  adw_dialog_close(((pair_dlg *)data)->dialog);
}

static void on_ok_clicked(GtkButton *button, gpointer data) {
  (void)button;
  close_quietly(data);
}

static pair_dlg *open_dlg(pair_dlg **slot, GtkWidget *parent, mc_sync *sync, gboolean initiator, const char *peer);

/* Create a New Library's wait: what to pair with again once it's open. */
typedef struct pending {
  GtkWidget *parent;
  char uid[MC_UID_STRLEN];
  char name[MC_SYNC_DEVICE_NAME_MAX + 1];
  char host[MC_SYNC_ADDR_STRLEN];
  guint16 port;
} pending;

/* After Create a New Library: offers to pair with the same device again. */
static void offer_pair_again(gpointer data, gboolean switched) {
  pending *p = data;
  /* Refused before it said its name: its address stands in. */
  if (!p->name[0]) g_strlcpy(p->name, p->host, sizeof p->name);
  if (switched && (p->uid[0] || p->host[0])) {
    pair_dlg *d = open_dlg(&s.enter, p->parent, NULL, TRUE, p->name);
    g_strlcpy(d->target, p->uid, sizeof d->target);
    g_strlcpy(d->host, p->host, sizeof d->host);
    d->port = p->port;
    char *title = peer_text(MC_STR_PAIR_WITH, p->name, FALSE);
    char *text = peer_text(MC_STR_PAIR_AGAIN_TEXT, p->name, FALSE);
    char *markup = g_markup_escape_text(text, -1);
    show_message(d, "folder-new-symbolic", TRUE, title, markup, ACT_PAIR_AGAIN, MC_STR_PAIR_AGAIN, MC_STR_CANCEL);
    g_free(markup);
    g_free(text);
    g_free(title);
  }
  g_free(p);
}

/* The ended page's button above OK. The dialog closes first, so what follows (a page, a
 * folder picker, another pairing) has the window to itself. */
static void on_action_clicked(GtkButton *button, gpointer data) {
  (void)button;
  pair_dlg *d = data;
  if (!d->ended || d->next == ACT_NONE) return;
  ended_action next = d->next;
  pending *p = g_new0(pending, 1);
  p->parent = d->parent;
  g_strlcpy(p->uid, d->target, sizeof p->uid);
  g_strlcpy(p->name, d->peer, sizeof p->name);
  g_strlcpy(p->host, d->host, sizeof p->host);
  p->port = d->port;
  if (next == ACT_NEW_LIBRARY && p->uid[0]) {
    /* Where it is now, in case discovery hasn't seen it again by the time the new library's
     * sync asks. */
    mc_sync_nearby_list *nearby = NULL;
    mc_sync *sync = sync_bridge_get();
    if (sync && mc_sync_list_nearby(sync, &nearby) == MC_OK) {
      for (int i = 0; i < nearby->count; i++)
        if (!strcmp(nearby->items[i].uid, p->uid) && nearby->items[i].address) {
          g_strlcpy(p->host, nearby->items[i].address, sizeof p->host);
          p->port = nearby->items[i].port;
        }
      mc_sync_nearby_list_free(nearby);
    }
  }
  close_quietly(d); /* d may be freed from here on */
  const pair_dialog_host *h = s.host;
  switch (next) {
    case ACT_CHOOSE:
      if (h && h->show_device) h->show_device(p->uid);
      break;
    case ACT_NEW_LIBRARY:
      if (h && h->new_library) {
        h->new_library(p->parent, offer_pair_again, p);
        return; /* p goes with offer_pair_again */
      }
      break;
    case ACT_PAIR_AGAIN:
      if (h && h->pair_again) h->pair_again(p->uid, p->name, p->host[0] ? p->host : NULL, p->port);
      break;
    case ACT_NONE:
      break;
  }
  g_free(p);
}

static void send_code(pair_dlg *d) {
  char code[MC_PAIR_CODE_LEN + 1];
  if (!d->initiator || d->ended || d->checking || !d->sync || !code_text(d, code)) return;
  mc_status st = mc_sync_pair_enter(d->sync, code);
  if (st == MC_OK) {
    d->checking = TRUE;
    gtk_widget_set_sensitive(d->code, FALSE);
    update_pair(d);
    set_status(d, mc_str(MC_STR_PAIRING_PROGRESS));
  } else {
    char *text = mc_format_alloc(mc_str(MC_STR_PAIR_SEND_FAILED), MC_S(mc_status_str(st)));
    set_status(d, text);
    mc_free(text);
  }
}

static void on_pair_clicked(GtkButton *button, gpointer data) {
  (void)button;
  send_code(data);
}

static void on_code_activate(GtkEntry *entry, gpointer data) {
  (void)entry;
  send_code(data);
}

/* Keeps the code to digits, whatever is typed or pasted. */
static void on_code_changed(GtkEditable *editable, gpointer data) {
  pair_dlg *d = data;
  const char *text = gtk_editable_get_text(editable);
  GString *digits = g_string_new(NULL);
  for (const char *p = text; *p && digits->len < MC_PAIR_CODE_LEN; p++)
    if (g_ascii_isdigit(*p)) g_string_append_c(digits, *p);
  if (strcmp(digits->str, text)) {
    g_signal_handlers_block_by_func(editable, on_code_changed, data);
    gtk_editable_set_text(editable, digits->str);
    gtk_editable_set_position(editable, -1);
    g_signal_handlers_unblock_by_func(editable, on_code_changed, data);
  }
  g_string_free(digits, TRUE);
  update_pair(d);
}

/* Closing before the end (Cancel, Escape, the close button) gives up the pairing. */
static void on_closed(AdwDialog *dialog, gpointer data) {
  (void)dialog;
  pair_dlg *d = data;
  if (!d->ended && !d->quiet) {
    /* The CANCELLED result this brings needs no telling. */
    if (d->initiator) s.enter_cancelled = TRUE;
    else s.show_cancelled = TRUE;
    if (d->sync) mc_sync_pair_cancel(d->sync);
  }
  if (s.enter == d) s.enter = NULL;
  if (s.show == d) s.show = NULL;
  g_object_unref(d->dialog);
  g_free(d);
}

static GtkWidget *wrapped_label(const char *css) {
  GtkWidget *label = gtk_label_new(NULL);
  gtk_label_set_wrap(GTK_LABEL(label), TRUE);
  gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
  gtk_label_set_max_width_chars(GTK_LABEL(label), 40);
  if (css) gtk_widget_add_css_class(label, css);
  return label;
}

/* Creates and presents a dialog in place of *slot (closing the old one quietly). */
static pair_dlg *open_dlg(pair_dlg **slot, GtkWidget *parent, mc_sync *sync, gboolean initiator,
                          const char *peer) {
  pair_dlg *d = g_new0(pair_dlg, 1);
  d->sync = sync;
  d->initiator = initiator;
  d->parent = parent;
  g_strlcpy(d->peer, peer ? peer : "", sizeof d->peer);
  d->dialog = g_object_ref_sink(adw_dialog_new()); /* ours until it closes */
  adw_dialog_set_content_width(d->dialog, 400);

  GtkWidget *header = adw_header_bar_new();
  d->header = ADW_HEADER_BAR(header);
  adw_header_bar_set_show_start_title_buttons(d->header, FALSE);
  adw_header_bar_set_show_end_title_buttons(d->header, FALSE);
  d->cancel = gtk_button_new_with_mnemonic(mc_str_mnemonic(MC_STR_CANCEL));
  g_signal_connect(d->cancel, "clicked", G_CALLBACK(on_cancel_clicked), d);
  adw_header_bar_pack_start(d->header, d->cancel);
  if (initiator) {
    d->pair = gtk_button_new_with_mnemonic(mc_str_mnemonic(MC_STR_PAIR));
    gtk_widget_add_css_class(d->pair, "suggested-action");
    gtk_widget_set_sensitive(d->pair, FALSE);
    g_signal_connect(d->pair, "clicked", G_CALLBACK(on_pair_clicked), d);
    adw_header_bar_pack_end(d->header, d->pair);
  }

  GtkWidget *ask = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
  gtk_widget_set_margin_start(ask, 24);
  gtk_widget_set_margin_end(ask, 24);
  gtk_widget_set_margin_top(ask, 12);
  gtk_widget_set_margin_bottom(ask, 24);
  d->text = wrapped_label(NULL);
  gtk_box_append(GTK_BOX(ask), d->text);
  if (initiator) {
    d->code = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(d->code), MC_PAIR_CODE_LEN);
    gtk_entry_set_input_purpose(GTK_ENTRY(d->code), GTK_INPUT_PURPOSE_DIGITS);
    gtk_entry_set_alignment(GTK_ENTRY(d->code), 0.5f);
    gtk_editable_set_width_chars(GTK_EDITABLE(d->code), MC_PAIR_CODE_LEN + 2);
    gtk_widget_set_halign(d->code, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(d->code, "pair-code");
    g_signal_connect(d->code, "changed", G_CALLBACK(on_code_changed), d);
    g_signal_connect(d->code, "activate", G_CALLBACK(on_code_activate), d);
  } else {
    d->code = gtk_label_new(NULL);
    gtk_label_set_selectable(GTK_LABEL(d->code), TRUE);
    gtk_widget_add_css_class(d->code, "pair-code-shown");
  }
  gtk_accessible_update_property(GTK_ACCESSIBLE(d->code), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 mc_str(MC_STR_LINUX_PAIR_CODE_LABEL), -1);
  gtk_box_append(GTK_BOX(ask), d->code);
  d->status = wrapped_label("dim-label");
  gtk_widget_set_visible(d->status, FALSE);
  gtk_box_append(GTK_BOX(ask), d->status);

  /* How it ended, laid out as a status page (whose own scrolling would clip it here). */
  GtkWidget *ended = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(ended, 24);
  gtk_widget_set_margin_end(ended, 24);
  gtk_widget_set_margin_bottom(ended, 24);
  d->ended_icon = gtk_image_new();
  gtk_image_set_pixel_size(GTK_IMAGE(d->ended_icon), 64);
  gtk_box_append(GTK_BOX(ended), d->ended_icon);
  d->ended_title = wrapped_label("title-2");
  gtk_box_append(GTK_BOX(ended), d->ended_title);
  d->ended_text = wrapped_label(NULL);
  gtk_box_append(GTK_BOX(ended), d->ended_text);
  d->action = gtk_button_new_with_mnemonic("");
  gtk_widget_add_css_class(d->action, "pill");
  gtk_widget_set_halign(d->action, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top(d->action, 12);
  gtk_widget_set_visible(d->action, FALSE);
  g_signal_connect(d->action, "clicked", G_CALLBACK(on_action_clicked), d);
  gtk_box_append(GTK_BOX(ended), d->action);
  d->ok = gtk_button_new_with_mnemonic(mc_str_mnemonic(MC_STR_OK));
  gtk_widget_add_css_class(d->ok, "pill");
  gtk_widget_add_css_class(d->ok, "suggested-action");
  gtk_widget_set_halign(d->ok, GTK_ALIGN_CENTER);
  g_signal_connect(d->ok, "clicked", G_CALLBACK(on_ok_clicked), d);
  gtk_box_append(GTK_BOX(ended), d->ok);

  d->stack = gtk_stack_new();
  gtk_stack_set_vhomogeneous(GTK_STACK(d->stack), FALSE);
  gtk_stack_set_transition_type(GTK_STACK(d->stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  gtk_stack_add_named(GTK_STACK(d->stack), ask, "ask");
  gtk_stack_add_named(GTK_STACK(d->stack), ended, "ended");

  GtkWidget *toolbar = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), d->stack);
  adw_dialog_set_child(d->dialog, toolbar);
  g_signal_connect(d->dialog, "closed", G_CALLBACK(on_closed), d);

  pair_dlg *old = *slot;
  *slot = d;
  close_quietly(old); /* its closed handler leaves *slot alone: it's no longer old */
  if (initiator) adw_dialog_set_focus(d->dialog, d->code);
  adw_dialog_present(d->dialog, parent);
  return d;
}

/* --- API --- */

void pair_dialog_init(const pair_dialog_host *host) {
  s.host = host;
}

void pair_dialog_set_target(const char *uid, const char *host, guint16 port) {
  pair_dlg *d = s.enter;
  if (!d || d->ended) return;
  g_strlcpy(d->target, uid ? uid : "", sizeof d->target);
  g_strlcpy(d->host, host ? host : "", sizeof d->host);
  d->port = port;
}

void pair_dialog_begin_enter(GtkWidget *parent, mc_sync *sync, const char *peer, const char *address) {
  pair_dlg *d = open_dlg(&s.enter, parent, sync, TRUE, peer);
  s.enter_cancelled = FALSE;
  char *text;
  if (peer && *peer) {
    text = peer_text(MC_STR_PAIR_WITH, peer, FALSE);
    adw_dialog_set_title(d->dialog, text);
    g_free(text);
  } else {
    adw_dialog_set_title(d->dialog, mc_str(MC_STR_PAIR_BY_ADDRESS));
  }
  if (!(peer && *peer) && address && *address) { /* the address on a line of its own */
    char *t = mc_format_alloc(mc_str(MC_STR_PAIR_ENTER_CODE_AT), MC_S(address));
    text = g_strdup(t ? t : "");
    mc_free(t);
  } else {
    text = peer_text(MC_STR_PAIR_ENTER_CODE, peer, FALSE);
  }
  gtk_label_set_text(GTK_LABEL(d->text), text);
  g_free(text);
}

void pair_dialog_show_code(GtkWidget *parent, mc_sync *sync, const sync_event *ev) {
  char *code = spaced_code(ev->code);
  pair_dlg *d = s.show;
  if (d && !d->ended) {
    /* A new code after too many wrong ones. */
    gtk_label_set_text(GTK_LABEL(d->code), code);
    set_status(d, mc_str(MC_STR_PAIR_NEW_CODE));
  } else {
    d = open_dlg(&s.show, parent, sync, FALSE, ev->name);
    s.show_cancelled = FALSE;
    adw_dialog_set_title(d->dialog, mc_str(MC_STR_PAIR_REQUEST));
    char *text = peer_text(MC_STR_PAIR_SHOW_CODE, ev->name, FALSE);
    gtk_label_set_text(GTK_LABEL(d->text), text);
    g_free(text);
    gtk_label_set_text(GTK_LABEL(d->code), code);
  }
  g_free(code);
}

static void initiator_result(GtkWidget *parent, const sync_event *ev) {
  pair_dlg *d = s.enter;
  if (!d) {
    /* Closed by Cancel: only a pairing that made it anyway is worth telling. */
    if (ev->reason == MC_PAIR_WRONG_CODE || (s.enter_cancelled && ev->reason != MC_PAIR_PAIRED)) {
      if (ev->reason != MC_PAIR_WRONG_CODE) s.enter_cancelled = FALSE;
      return;
    }
    d = open_dlg(&s.enter, parent, NULL, TRUE, ev->name);
    s.enter_cancelled = FALSE;
    show_ended(d, ev);
    return;
  }
  if (d->ended) return;
  if (ev->reason == MC_PAIR_WRONG_CODE) {
    /* The pairing waits for another code. */
    char *text = ev->attempts_left >= MAX_FAILURES
                   ? peer_text(MC_STR_PAIR_PEER_NEW_CODE, ev->name[0] ? ev->name : d->peer, TRUE)
                   : tries_left(MC_STR_PAIR_WRONG_CODE, ev->attempts_left);
    set_status(d, text);
    g_free(text);
    ready_for_code(d);
    return;
  }
  show_ended(d, ev);
}

static void responder_result(GtkWidget *parent, const sync_event *ev) {
  pair_dlg *d = s.show;
  if (ev->reason == MC_PAIR_CANCELLED && (s.show_cancelled || !d || d->ended)) {
    s.show_cancelled = FALSE; /* ours, or nothing showing to cancel */
    return;
  }
  if (ev->reason == MC_PAIR_WRONG_CODE) {
    if (!d || d->ended) return;
    if (ev->attempts_left >= MAX_FAILURES) {
      set_status(d, mc_str(MC_STR_PAIR_NEW_CODE)); /* the new code is already showing */
    } else {
      char *text = tries_left(MC_STR_PAIR_WRONG_CODE, ev->attempts_left);
      set_status(d, text);
      g_free(text);
    }
    return;
  }
  if (!d || d->ended) {
    /* Refused before a code was shown: say why on a dialog of its own. */
    if (s.show_cancelled && ev->reason != MC_PAIR_PAIRED) {
      s.show_cancelled = FALSE;
      return;
    }
    d = open_dlg(&s.show, parent, NULL, FALSE, ev->name);
  }
  show_ended(d, ev);
  s.show_cancelled = FALSE;
}

gboolean pair_dialog_on_result(GtkWidget *parent, const sync_event *ev) {
  if (!ev) return FALSE;
  if (ev->initiator) initiator_result(parent, ev);
  else responder_result(parent, ev);
  return ev->reason == MC_PAIR_PAIRED;
}

void pair_dialog_close_all(void) {
  close_quietly(s.enter);
  close_quietly(s.show);
  s.enter = s.show = NULL;
  s.enter_cancelled = s.show_cancelled = FALSE;
}

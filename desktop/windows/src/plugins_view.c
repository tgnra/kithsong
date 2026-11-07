#include "plugins_view.h"

#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "canvas.h"
#include "kithcore/plugins.h"
#include "prompt_dialog.h"
#include "theme.h"
#include "wstr.h"

/* Each plugin's controls have ids from its row's base: the box, its name, the details line, the
 * state line, then three for each option (its control, label and description). */
enum {
  ID_PLUG_NONE = 600,
  ID_PLUG_FIRST = 608,
  PER_PLUGIN = 16,
  MAX_PLUGINS = 12, /* 608 + 12 * 16 = 800 */
  MAX_OPTIONS = 4,
  SLOT_BOX = 0,
  SLOT_NAME,
  SLOT_DETAIL,
  SLOT_STATE,
  SLOT_OPTIONS,
};

typedef struct opt_row {
  HWND ctl, label, desc; /* a switch's ctl is its box; desc is NULL without a description */
} opt_row;

typedef struct plug_row {
  char *file_id;
  wchar_t *name;
  int enabled;
  HWND box, name_label, detail, state;
  mc_plugin_option_list *options; /* while it's running; NULL otherwise */
  opt_row opts[MAX_OPTIONS];
  int nopts;
} plug_row;

static struct {
  HWND parent;
  HWND anchor, after; /* the controls go in the tab order after anchor: after is the last made */
  HWND none; /* "No plugins found" */
  plug_row rows[MAX_PLUGINS];
  int nrows;
  HFONT font;
  UINT dpi;
  BOOL shown;
  BOOL filling; /* ignore notifications while the controls are being made */
  void (*relayout)(void);
  mc_settings *settings;
  void (*changed)(void);
} pv;

static void rebuild(void);

/* Puts wnd next in the tab order. */
static void in_order(HWND wnd) {
  if (!wnd || !pv.after) return;
  SetWindowPos(wnd, pv.after, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  pv.after = wnd;
}

enum { REBUILD_TIMER = 0x7E01 };

static void CALLBACK rebuild_timer(HWND wnd, UINT msg, UINT_PTR id, DWORD time) {
  (void)msg, (void)time;
  KillTimer(wnd, id);
  rebuild();
}

/* Makes the controls again once the notification under way is over: never destroy a control in
 * its own notification. */
static void rebuild_later(void) {
  SetTimer(pv.parent, REBUILD_TIMER, 0, rebuild_timer);
}

static HWND child(const wchar_t *cls, const wchar_t *text, DWORD style, int id) {
  HWND wnd = CreateWindowExW(0, cls, text, WS_CHILD | style | (pv.shown ? WS_VISIBLE : 0), 0, 0, 0, 0, pv.parent,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
  if (wnd && pv.font) SendMessageW(wnd, WM_SETFONT, (WPARAM)pv.font, FALSE);
  if (wnd) theme_apply_control(wnd);
  in_order(wnd);
  return wnd;
}

static void destroy(HWND *wnd) {
  if (*wnd) DestroyWindow(*wnd);
  *wnd = NULL;
}

static void free_rows(void) {
  for (int i = 0; i < pv.nrows; i++) {
    plug_row *r = &pv.rows[i];
    destroy(&r->box);
    destroy(&r->name_label);
    destroy(&r->detail);
    destroy(&r->state);
    for (int o = 0; o < r->nopts; o++) {
      destroy(&r->opts[o].ctl);
      destroy(&r->opts[o].label);
      destroy(&r->opts[o].desc);
    }
    mc_plugin_option_list_free(r->options);
    free(r->file_id);
    free(r->name);
    memset(r, 0, sizeof *r);
  }
  pv.nrows = 0;
}

/* Where plugins go for this user: %LOCALAPPDATA%\Kithsong\plugins. */
static void user_folder(wchar_t *out, DWORD cap) {
  wchar_t base[MAX_PATH];
  DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
  if (!n || n >= MAX_PATH) {
    out[0] = L'\0';
    return;
  }
  swprintf(out, cap, L"%ls\\Kithsong\\plugins", base);
}

/* One option's controls, under the plugin. */
static void make_option(plug_row *r, int base, int o) {
  const mc_plugin_option *opt = &r->options->items[o];
  opt_row *row = &r->opts[o];
  wchar_t label[256], value[64];
  wstr_from_utf8(label, 256, opt->label ? opt->label : opt->key);
  int id = base + SLOT_OPTIONS + 3 * o;
  switch (opt->kind) {
    case MC_PLUGIN_OPTION_SWITCH:
      /* As elsewhere in Settings: a box with no text, and a label beside it that toggles it. */
      row->ctl = child(WC_BUTTONW, NULL, BS_AUTOCHECKBOX | WS_TABSTOP, id);
      SendMessageW(row->ctl, BM_SETCHECK, opt->value && !strcmp(opt->value, "1") ? BST_CHECKED : BST_UNCHECKED, 0);
      row->label = child(WC_STATICW, label, SS_LEFT | SS_NOPREFIX | SS_NOTIFY, id + 1);
      break;
    case MC_PLUGIN_OPTION_CHOICE: {
      row->label = child(WC_STATICW, label, SS_LEFT | SS_NOPREFIX, id + 1);
      row->ctl = child(WC_COMBOBOXW, NULL, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, id);
      int sel = -1;
      for (int c = 0; c < opt->choice_count; c++) {
        wchar_t text[256];
        wstr_from_utf8(text, 256, opt->choice_labels && opt->choice_labels[c] ? opt->choice_labels[c] : opt->choice_ids[c]);
        SendMessageW(row->ctl, CB_ADDSTRING, 0, (LPARAM)text);
        if (opt->value && !strcmp(opt->value, opt->choice_ids[c])) sel = c;
      }
      SendMessageW(row->ctl, CB_SETCURSEL, (WPARAM)sel, 0);
      break;
    }
    case MC_PLUGIN_OPTION_NUMBER:
      row->label = child(WC_STATICW, label, SS_LEFT | SS_NOPREFIX, id + 1);
      row->ctl = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, NULL,
                                 WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL | (pv.shown ? WS_VISIBLE : 0), 0, 0, 0, 0,
                                 pv.parent, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
      if (row->ctl) {
        in_order(row->ctl);
        SendMessageW(row->ctl, WM_SETFONT, (WPARAM)pv.font, FALSE);
        theme_center_edit(row->ctl);
        wstr_from_utf8(value, 64, opt->value ? opt->value : "");
        SetWindowTextW(row->ctl, value);
      }
      break;
  }
  if (opt->description) {
    wchar_t desc[512];
    wstr_from_utf8(desc, 512, opt->description);
    row->desc = child(WC_STATICW, desc, SS_LEFT | SS_NOPREFIX, id + 2);
  }
}

/* The controls again, from the list as it is now. */
static void rebuild(void) {
  /* The focus comes back to the same plugin's box. */
  HWND focus = GetFocus();
  int focused = -1;
  if (focus && GetParent(focus) == pv.parent) {
    LONG_PTR id = GetWindowLongPtrW(focus, GWLP_ID);
    if (id >= ID_PLUG_FIRST && id < ID_PLUG_FIRST + MAX_PLUGINS * PER_PLUGIN) focused = (int)(id - ID_PLUG_FIRST) / PER_PLUGIN;
  }
  pv.filling = TRUE;
  free_rows();
  destroy(&pv.none);
  pv.after = pv.anchor;

  mc_plugin_list *list = NULL;
  if (mc_plugins_list(&list) != MC_OK) list = NULL;
  int n = list ? list->count : 0;
  if (n > MAX_PLUGINS) n = MAX_PLUGINS;
  for (int i = 0; i < n; i++) {
    const mc_plugin_info *info = &list->items[i];
    plug_row *r = &pv.rows[pv.nrows++];
    int base = ID_PLUG_FIRST + i * PER_PLUGIN;
    r->file_id = _strdup(info->file_id);
    r->name = wstr_dup_from_utf8(info->name ? info->name : info->file_id);
    r->enabled = info->enabled;
    /* Not an auto checkbox: it shows what's stored, which changes once confirmed. */
    r->box = child(WC_BUTTONW, NULL, BS_CHECKBOX | WS_TABSTOP, base + SLOT_BOX);
    SendMessageW(r->box, BM_SETCHECK, info->enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    r->name_label = child(WC_STATICW, r->name ? r->name : L"", SS_LEFT | SS_NOPREFIX | SS_NOTIFY, base + SLOT_NAME);

    wchar_t detail[1024] = L"", path[MAX_PATH * 2];
    wstr_from_utf8(path, MAX_PATH * 2, info->path);
    if (info->version) {
      mc_wformat(detail, 1024, mc_str(MC_STR_PLUGIN_VERSION), MC_S(info->version));
      size_t used = wcslen(detail);
      swprintf(detail + used, 1024 - used, L"%ls%ls", mc_wstr(MC_STR_SEPARATOR_DOT), path);
    } else {
      swprintf(detail, 1024, L"%ls", path);
    }
    r->detail = child(WC_STATICW, detail, SS_LEFT | SS_NOPREFIX | SS_PATHELLIPSIS, base + SLOT_DETAIL);

    wchar_t state[512];
    switch (info->state) {
      case MC_PLUGIN_LOADED: swprintf(state, 512, L"%ls", mc_wstr(MC_STR_PLUGIN_RUNNING)); break;
      case MC_PLUGIN_REFUSED:
        mc_wformat(state, 512, mc_str(MC_STR_PLUGIN_REFUSED), MC_S(info->reason ? info->reason : mc_str(MC_STR_STATUS_UNKNOWN)));
        break;
      case MC_PLUGIN_UNLOADING: swprintf(state, 512, L"%ls", mc_wstr(MC_STR_PLUGIN_UNLOADING)); break;
      default: swprintf(state, 512, L"%ls", mc_wstr(MC_STR_PLUGIN_OFF)); break;
    }
    r->state = child(WC_STATICW, state, SS_LEFT | SS_NOPREFIX, base + SLOT_STATE);

    if (info->state == MC_PLUGIN_LOADED && mc_plugins_options(info->file_id, &r->options) == MC_OK && r->options) {
      r->nopts = r->options->count < MAX_OPTIONS ? r->options->count : MAX_OPTIONS;
      for (int o = 0; o < r->nopts; o++) make_option(r, base, o);
    } else {
      r->options = NULL;
    }
  }
  if (!pv.nrows) {
    wchar_t folder[MAX_PATH + 32], text[MAX_PATH + 256];
    user_folder(folder, MAX_PATH + 32);
    char *f = wstr_to_utf8(folder);
    mc_wformat(text, MAX_PATH + 256, mc_str(MC_STR_PLUGINS_NONE), MC_S(f ? f : ""));
    free(f);
    pv.none = child(WC_STATICW, text, SS_LEFT | SS_NOPREFIX, ID_PLUG_NONE);
  }
  mc_plugin_list_free(list);
  pv.filling = FALSE;
  if (focused >= 0 && focused < pv.nrows) SetFocus(pv.rows[focused].box);
  if (pv.relayout) pv.relayout();
  /* Controls the section's new height moved past their old place aren't all repainted. */
  if (pv.shown) RedrawWindow(pv.parent, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

void plugins_view_create(HWND parent, HWND after, void (*relayout)(void)) {
  pv.anchor = after;
  pv.parent = parent;
  pv.relayout = relayout;
  rebuild();
}

void plugins_view_destroy(void) {
  free_rows();
  destroy(&pv.none);
}

void plugins_view_init(mc_settings *settings, void (*changed)(void)) {
  pv.settings = settings;
  pv.changed = changed;
  rebuild(); /* the plugins are loaded by now */
}

void plugins_view_set_font(HFONT font, UINT dpi) {
  pv.font = font;
  pv.dpi = dpi;
  rebuild();
}

void plugins_view_show(BOOL shown) {
  pv.shown = shown;
  int cmd = shown ? SW_SHOW : SW_HIDE;
  if (pv.none) ShowWindow(pv.none, cmd);
  for (int i = 0; i < pv.nrows; i++) {
    plug_row *r = &pv.rows[i];
    HWND all[] = {r->box, r->name_label, r->detail, r->state};
    for (int k = 0; k < 4; k++) ShowWindow(all[k], cmd);
    for (int o = 0; o < r->nopts; o++) {
      ShowWindow(r->opts[o].ctl, cmd);
      ShowWindow(r->opts[o].label, cmd);
      if (r->opts[o].desc) ShowWindow(r->opts[o].desc, cmd);
    }
  }
}

static int text_w(HWND wnd) {
  wchar_t buf[256];
  int n = GetWindowTextW(wnd, buf, 256);
  HDC dc = GetDC(wnd);
  HGDIOBJ old = SelectObject(dc, pv.font);
  SIZE size = {0, 0};
  GetTextExtentPoint32W(dc, buf, n, &size);
  SelectObject(dc, old);
  ReleaseDC(wnd, dc);
  return size.cx;
}

int plugins_view_layout(int left, int top, int width, UINT dpi) {
#define S(px) MulDiv((px), (int)dpi, USER_DEFAULT_SCREEN_DPI)
  if (!pv.font) return top;
  if (pv.none) {
    int h = canvas_text_height(pv.none, pv.font, width, 0);
    MoveWindow(pv.none, left, top, width, h, TRUE);
    return top + h;
  }
  int box = GetSystemMetricsForDpi(SM_CXMENUCHECK, dpi), indent = box + S(8);
  for (int i = 0; i < pv.nrows; i++) {
    plug_row *r = &pv.rows[i];
    if (i) top += S(16);
    int lh = canvas_text_height(r->name_label, pv.font, width - indent, DT_SINGLELINE);
    int row_h = max(box, lh);
    MoveWindow(r->box, left, top + (row_h - box) / 2, box, box, TRUE);
    MoveWindow(r->name_label, left + indent, top + (row_h - lh) / 2, min(text_w(r->name_label) + S(8), width - indent),
               lh, TRUE);
    top += row_h + S(2);
    int dh = canvas_text_height(r->detail, pv.font, width - indent, DT_SINGLELINE);
    MoveWindow(r->detail, left + indent, top, width - indent, dh, TRUE);
    top += dh + S(2);
    int sh = canvas_text_height(r->state, pv.font, width - indent, 0);
    MoveWindow(r->state, left + indent, top, width - indent, sh, TRUE);
    top += sh;
    /* Its settings, indented under it: a label and its control on a line, a description under. */
    int label_w = 0;
    for (int o = 0; o < r->nopts; o++)
      if (r->options->items[o].kind != MC_PLUGIN_OPTION_SWITCH) label_w = max(label_w, text_w(r->opts[o].label));
    label_w += S(16);
    for (int o = 0; o < r->nopts; o++) {
      opt_row *row = &r->opts[o];
      int x = left + indent, field_h = S(30), oh = canvas_text_height(row->label, pv.font, width - indent, 0);
      top += S(8);
      switch (r->options->items[o].kind) {
        case MC_PLUGIN_OPTION_SWITCH: {
          int rh = max(box, oh);
          MoveWindow(row->ctl, x, top + (rh - box) / 2, box, box, TRUE);
          MoveWindow(row->label, x + indent, top + (rh - oh) / 2, width - 2 * indent, oh, TRUE);
          top += rh;
          break;
        }
        case MC_PLUGIN_OPTION_CHOICE: {
          MoveWindow(row->label, x, top + (field_h - oh) / 2, label_w, oh, TRUE);
          RECT rc;
          GetWindowRect(row->ctl, &rc);
          int ch = rc.bottom - rc.top;
          if (!SendMessageW(row->ctl, CB_GETDROPPEDSTATE, 0, 0))
            MoveWindow(row->ctl, x + label_w, top + (ch > 0 && ch < field_h ? (field_h - ch) / 2 : 0),
                       min(S(240), width - indent - label_w), S(300), TRUE);
          top += field_h;
          break;
        }
        default:
          MoveWindow(row->label, x, top + (field_h - oh) / 2, label_w, oh, TRUE);
          MoveWindow(row->ctl, x + label_w, top + (field_h - S(28)) / 2, S(96), S(28), TRUE);
          top += field_h;
          break;
      }
      if (row->desc) {
        int dh2 = canvas_text_height(row->desc, pv.font, width - indent, 0);
        MoveWindow(row->desc, x, top + S(2), width - indent, dh2, TRUE);
        top += dh2 + S(2);
      }
    }
  }
  return top;
#undef S
}

/* Saves the settings, and tells the main window. */
static void changed(void) {
  if (pv.settings) {
    mc_plugins_flush_settings();
    mc_settings_save(pv.settings); /* failing, it's saved with the next setting */
  }
  if (pv.changed) pv.changed();
}

static void failed(mc_status st) {
  wchar_t text[256];
  mc_wformat(text, 256, mc_str(MC_STR_PLUGIN_CHANGE_FAILED), MC_S(mc_status_str(st)));
  message_dialog(GetAncestor(pv.parent, GA_ROOT), mc_wstr(MC_STR_SETTINGS_PLUGINS), text, MB_ICONERROR);
}

/* The box: turning on asks first, as it runs code with the app's rights. */
static void toggle(int i) {
  plug_row *r = &pv.rows[i];
  BOOL on = !r->enabled;
  if (on) {
    mc_plugin_list *list = NULL;
    wchar_t title[256], text[2048], path[MAX_PATH * 2] = L"";
    if (mc_plugins_list(&list) == MC_OK && list)
      for (int k = 0; k < list->count; k++)
        if (!strcmp(list->items[k].file_id, r->file_id)) wstr_from_utf8(path, MAX_PATH * 2, list->items[k].path);
    mc_plugin_list_free(list);
    char *name = wstr_to_utf8(r->name ? r->name : L"");
    const wchar_t *file = wcsrchr(path, L'\\');
    char *p = wstr_to_utf8(file ? file + 1 : path); /* the row above has the whole path */
    mc_wformat(title, 256, mc_str(MC_STR_PLUGIN_TURN_ON_TITLE), MC_S(name ? name : ""));
    mc_wformat(text, 2048, mc_str(MC_STR_PLUGIN_TURN_ON_TEXT), MC_S(p ? p : ""));
    free(name);
    free(p);
    prompt_dialog ask = {.caption = mc_wstr(MC_STR_APP_NAME), .icon = MB_ICONWARNING, .heading = title,
                         .text = text, .ok = mc_wstr_mnemonic(MC_STR_PLUGIN_TURN_ON),
                         .cancel = mc_wstr_mnemonic(MC_STR_CANCEL)};
    if (prompt_dialog_show(GetAncestor(pv.parent, GA_ROOT), &ask) != IDOK) return;
  }
  char *file_id = _strdup(r->file_id); /* the rows are made again */
  mc_status st = file_id ? mc_plugins_set_enabled(file_id, on) : MC_ERR_NOMEM;
  free(file_id);
  if (st != MC_OK) failed(st);
  rebuild_later();
  if (st == MC_OK) changed();
}

/* An option's control changed: stores its value, or puts it back if it's refused. */
static void option_changed(int i, int o) {
  plug_row *r = &pv.rows[i];
  if (!r->options || o >= r->nopts) return;
  const mc_plugin_option *opt = &r->options->items[o];
  HWND ctl = r->opts[o].ctl;
  char value[64];
  switch (opt->kind) {
    case MC_PLUGIN_OPTION_SWITCH:
      snprintf(value, sizeof value, "%d", SendMessageW(ctl, BM_GETCHECK, 0, 0) == BST_CHECKED);
      break;
    case MC_PLUGIN_OPTION_CHOICE: {
      int c = (int)SendMessageW(ctl, CB_GETCURSEL, 0, 0);
      if (c < 0 || c >= opt->choice_count) return;
      snprintf(value, sizeof value, "%s", opt->choice_ids[c]);
      break;
    }
    default: {
      wchar_t buf[32];
      GetWindowTextW(ctl, buf, 32);
      long long v = wcstoll(buf, NULL, 10);
      if (v < opt->min) v = opt->min;
      if (v > opt->max) v = opt->max;
      snprintf(value, sizeof value, "%lld", v);
      if (opt->value && !strcmp(value, opt->value)) { /* unchanged, but maybe clamped: shown as stored */
        wstr_from_utf8(buf, 32, value);
        SetWindowTextW(ctl, buf);
        return;
      }
      break;
    }
  }
  char *file_id = _strdup(r->file_id), *key = _strdup(opt->key);
  mc_status st = file_id && key ? mc_plugins_set_option(file_id, key, value) : MC_ERR_NOMEM;
  free(file_id);
  free(key);
  if (st != MC_OK) failed(st);
  rebuild_later(); /* what's stored, and any options that depend on it */
  if (st == MC_OK) changed();
}

BOOL plugins_view_on_command(int id, int code) {
  if (id == ID_PLUG_NONE) return TRUE;
  if (id < ID_PLUG_FIRST || id >= ID_PLUG_FIRST + MAX_PLUGINS * PER_PLUGIN) return FALSE;
  if (pv.filling) return TRUE;
  int i = (id - ID_PLUG_FIRST) / PER_PLUGIN, slot = (id - ID_PLUG_FIRST) % PER_PLUGIN;
  if (i >= pv.nrows) return TRUE;
  if ((slot == SLOT_BOX && code == BN_CLICKED) || (slot == SLOT_NAME && code == STN_CLICKED)) {
    toggle(i);
    return TRUE;
  }
  if (slot < SLOT_OPTIONS) return TRUE;
  int o = (slot - SLOT_OPTIONS) / 3, part = (slot - SLOT_OPTIONS) % 3;
  plug_row *r = &pv.rows[i];
  if (o >= r->nopts) return TRUE;
  mc_plugin_option_kind kind = r->options->items[o].kind;
  if (part == 1 && kind == MC_PLUGIN_OPTION_SWITCH && code == STN_CLICKED) { /* its label */
    HWND box = r->opts[o].ctl;
    SendMessageW(box, BM_SETCHECK, SendMessageW(box, BM_GETCHECK, 0, 0) == BST_CHECKED ? BST_UNCHECKED : BST_CHECKED, 0);
    option_changed(i, o);
  } else if (part == 0 && ((kind == MC_PLUGIN_OPTION_SWITCH && code == BN_CLICKED) ||
                           (kind == MC_PLUGIN_OPTION_CHOICE && code == CBN_SELCHANGE) ||
                           (kind == MC_PLUGIN_OPTION_NUMBER && code == EN_KILLFOCUS))) {
    option_changed(i, o);
  }
  return TRUE;
}

HBRUSH plugins_view_ctlcolor(HWND wnd, HDC dc) {
  if (!wnd) return NULL;
  COLORREF color = 0;
  BOOL found = FALSE, plain = FALSE;
  if (wnd == pv.none) {
    color = theme.text2;
    found = TRUE;
  }
  for (int i = 0; i < pv.nrows && !found; i++) {
    plug_row *r = &pv.rows[i];
    if (wnd == r->box) plain = found = TRUE;
    else if (wnd == r->name_label) color = theme.text, found = TRUE;
    else if (wnd == r->detail || wnd == r->state) color = theme.text2, found = TRUE;
    for (int o = 0; o < r->nopts && !found; o++) {
      if (r->options->items[o].kind == MC_PLUGIN_OPTION_SWITCH && wnd == r->opts[o].ctl) plain = found = TRUE;
      else if (wnd == r->opts[o].label) color = theme.text, found = TRUE;
      else if (wnd == r->opts[o].desc) color = theme.text2, found = TRUE;
    }
  }
  if (!found) return NULL;
  if (!plain) SetTextColor(dc, color);
  SetBkColor(dc, theme.bg);
  return theme.bg_brush;
}

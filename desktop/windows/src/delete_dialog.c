#include "delete_dialog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "playlist_dialogs.h"
#include "wstr.h"

static struct {
  char *config_dir;
  void (*before_delete)(const mc_delete_plan *plan);
} d;

void delete_dialog_init(const char *config_dir, void (*before_delete)(const mc_delete_plan *plan)) {
  free(d.config_dir);
  d.config_dir = config_dir ? _strdup(config_dir) : NULL;
  d.before_delete = before_delete;
}

/* How many of the tracks the confirmation names. */
#define LOST_NAMED 6

/* The confirmation's note: what removing means, then the tracks. */
static void lost_note(const mc_sync_problems *p, wchar_t *note, size_t cap) {
  lstrcpynW(note, mc_wstr(MC_STR_SYNC_LOST_CONFIRM_BODY), (int)cap);
  wcsncat(note, L"\n", cap - wcslen(note) - 1);
  for (int i = 0; i < p->lost && i < LOST_NAMED; i++) {
    const mc_sync_lost_track *t = &p->tracks[i];
    wchar_t line[256];
    if (t->artist && *t->artist) mc_wformat(line, 256, mc_str(MC_STR_SYNC_LOST_TRACK), MC_S(t->title), MC_S(t->artist));
    else mc_wformat(line, 256, "{0}", MC_S(t->title));
    wcsncat(note, L"\n", cap - wcslen(note) - 1);
    wcsncat(note, line, cap - wcslen(note) - 1);
  }
  if (p->lost > LOST_NAMED) {
    wchar_t more[64];
    mc_wformat(more, 64, mc_str_plural(MC_STR_SYNC_LOST_CONFIRM_MORE, p->lost - LOST_NAMED), MC_D(p->lost - LOST_NAMED));
    wcsncat(note, L"\n", cap - wcslen(note) - 1);
    wcsncat(note, more, cap - wcslen(note) - 1);
  }
}

BOOL delete_dialog_lost(HWND owner, mc_library *lib, mc_sync *sync) {
  mc_sync_problems *p = NULL;
  if (!sync || !lib || mc_sync_get_problems(sync, &p) != MC_OK || !p->lost) {
    mc_sync_problems_free(p);
    return FALSE;
  }
  const wchar_t *title = mc_wstr(MC_STR_SYNC_LOST_CAPTION);
  wchar_t heading[128], note[2048];
  mc_wformat(heading, 128, mc_str_plural(MC_STR_SYNC_LOST_CONFIRM_TITLE, p->lost), MC_D(p->lost));
  lost_note(p, note, sizeof note / sizeof *note);
  BOOL changed = FALSE;
  const char **uids = malloc((size_t)p->lost * sizeof *uids);
  if (uids && confirm_lost_dialog(owner, title, heading, note, mc_wstr(MC_STR_SYNC_LOST_CONFIRM_BUTTON))) {
    for (int i = 0; i < p->lost; i++) uids[i] = p->tracks[i].uid;
    mc_delete_plan *plan = NULL;
    if (d.before_delete && mc_delete_plan_new(lib, MC_DELETE_TRACKS, uids, p->lost, &plan) == MC_OK)
      d.before_delete(plan);
    mc_delete_plan_free(plan);
    mc_delete_result res = {0};
    mc_status st = mc_sync_remove_lost(sync, uids, p->lost, &res);
    changed = res.tracks_deleted > 0;
    if (st == MC_ERR_LOCKED) {
      MessageBoxW(owner, mc_wstr(MC_STR_SYNC_LOST_BUSY), title, MB_OK | MB_ICONINFORMATION);
    } else if (st != MC_OK) {
      MessageBoxW(owner, mc_wstr(MC_STR_SYNC_LOST_FAILED), title, MB_OK | MB_ICONERROR);
    } else if (res.tracks_deleted < p->lost) {
      wchar_t text[256];
      int kept = p->lost - res.tracks_deleted;
      mc_wformat(text, 256, mc_str_plural(MC_STR_SYNC_LOST_KEPT, kept), MC_D(kept));
      MessageBoxW(owner, text, title, MB_OK | MB_ICONINFORMATION);
    }
  }
  free(uids);
  mc_sync_problems_free(p);
  return changed;
}

BOOL delete_dialog(HWND owner, mc_library *lib, mc_delete_kind kind, const char *uid, const wchar_t *name) {
  if (!lib || !uid || !d.config_dir) return FALSE;
  const wchar_t *caption = mc_wstr(MC_STR_WINDOWS_DELETE_CAPTION);
  /* Copied: the dialog's message loop repaints the views, which may reload what uid and name
   * point into. */
  char key[MC_UID_STRLEN];
  snprintf(key, sizeof key, "%s", uid);
  wchar_t *title = _wcsdup(name && *name ? name : mc_wstr(MC_STR_UNKNOWN_VALUE));
  const char *uids[] = {key};
  mc_delete_plan *plan = NULL;
  if (!title || mc_delete_plan_new(lib, kind, uids, 1, &plan) != MC_OK) {
    free(title);
    MessageBoxW(owner, mc_wstr(MC_STR_DELETE_FAILED), caption, MB_OK | MB_ICONERROR);
    return FALSE;
  }
  int files = mc_delete_plan_files(plan);
  wchar_t note[512];
  if (files) mc_wformat(note, 512, mc_str_plural(MC_STR_WINDOWS_DELETE_TO_RECYCLE_BIN_TEXT, files), MC_D(files));
  else wcscpy(note, mc_wstr(MC_STR_DELETE_NO_FILES_TEXT));
  BOOL changed = FALSE;
  if (mc_delete_plan_tracks(plan) && confirm_delete_dialog(owner, caption, title, note)) {
    if (d.before_delete) d.before_delete(plan);
    mc_delete_result res;
    mc_status st = mc_delete_run(lib, d.config_dir, plan, &res);
    changed = res.tracks_deleted > 0 || res.files_trashed > 0;
    if (st != MC_OK) {
      MessageBoxW(owner, mc_wstr(MC_STR_DELETE_FAILED), caption, MB_OK | MB_ICONERROR);
    } else if (res.tracks_kept) {
      wchar_t text[512];
      mc_wformat(text, 512, mc_str_plural(MC_STR_WINDOWS_DELETE_KEPT_RECYCLE_BIN, res.tracks_kept), MC_D(res.tracks_kept));
      MessageBoxW(owner, text, caption, MB_OK | MB_ICONWARNING);
    }
  }
  mc_delete_plan_free(plan);
  free(title);
  return changed;
}

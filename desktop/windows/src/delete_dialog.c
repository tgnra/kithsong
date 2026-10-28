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

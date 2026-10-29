#include "remove_here.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "playlist_dialogs.h"
#include "sync_bridge.h"
#include "wstr.h"

static void (*g_before_remove)(const mc_here_plan *plan);

void remove_here_init(void (*before_remove)(const mc_here_plan *plan)) {
  g_before_remove = before_remove;
}

void remove_here_append_menu(HMENU menu, mc_profile_item_kind kind, const char *uid, UINT cmd_remove,
                             UINT cmd_sync) {
  mc_sync *sync = sync_bridge_get();
  mc_here_state state = MC_HERE_UNAVAILABLE;
  if (!sync || !uid || mc_sync_here_state(sync, kind, uid, &state) != MC_OK) return;
  if (state == MC_HERE_HELD)
    AppendMenuW(menu, MF_STRING, cmd_remove, mc_wstr_mnemonic(MC_STR_WINDOWS_REMOVE_FROM_THIS_PC));
  else if (state == MC_HERE_REMOVED)
    AppendMenuW(menu, MF_STRING, cmd_sync, mc_wstr_mnemonic(MC_STR_WINDOWS_SYNC_TO_THIS_PC));
}

/* Appends text to note (cap wide chars), after a space if note isn't empty. */
static void append(wchar_t *note, size_t cap, const wchar_t *text) {
  size_t n = wcslen(note);
  if (n && n + 1 < cap) note[n++] = L' ', note[n] = L'\0';
  wcsncat(note, text, cap - n - 1);
}

static BOOL remove_item(HWND owner, mc_sync *sync, mc_profile_item_kind kind, const char *uid,
                        const wchar_t *name) {
  const wchar_t *caption = mc_wstr(MC_STR_WINDOWS_REMOVE_FROM_THIS_PC_CAPTION);
  mc_here_plan *plan = NULL;
  if (mc_sync_here_plan_new(sync, kind, uid, &plan) != MC_OK) {
    MessageBoxW(owner, mc_wstr(MC_STR_REMOVE_HERE_FAILED), caption, MB_OK | MB_ICONERROR);
    return FALSE;
  }
  int files = mc_here_plan_files(plan), kept = mc_here_plan_kept(plan);
  wchar_t note[1024] = L"", part[512];
  /* What goes (or that nothing's here), what stays for now, how to get it back. */
  if (files) {
    mc_wformat(part, 512, mc_str_plural(MC_STR_REMOVE_HERE_FILES, files), MC_D(files));
    append(note, 1024, part);
  } else if (!kept) {
    append(note, 1024, mc_wstr(MC_STR_REMOVE_HERE_NO_FILES));
  }
  if (kept) {
    mc_wformat(part, 512, mc_str_plural(MC_STR_REMOVE_HERE_KEPT, kept), MC_D(kept));
    append(note, 1024, part);
  }
  append(note, 1024, mc_wstr(MC_STR_WINDOWS_REMOVE_FROM_THIS_PC_BACK));
  BOOL changed = FALSE;
  if (confirm_remove_dialog(owner, caption, MC_STR_WINDOWS_REMOVE_FROM_THIS_PC_TITLE, name, note,
                            mc_wstr_mnemonic(MC_STR_REMOVE))) {
    if (g_before_remove) g_before_remove(plan);
    mc_here_result res;
    mc_status st = mc_sync_remove_here(sync, plan, &res);
    changed = st == MC_OK;
    if (st != MC_OK) {
      MessageBoxW(owner, mc_wstr(MC_STR_REMOVE_HERE_FAILED), caption, MB_OK | MB_ICONERROR);
    } else if (res.files_kept > kept) {
      int n = res.files_kept - kept;
      wchar_t text[512];
      mc_wformat(text, 512, mc_str_plural(MC_STR_REMOVE_HERE_IN_USE, n), MC_D(n));
      MessageBoxW(owner, text, caption, MB_OK | MB_ICONWARNING);
    }
  }
  mc_here_plan_free(plan);
  return changed;
}

BOOL remove_here_command(HWND owner, UINT cmd, UINT cmd_remove, UINT cmd_sync, mc_profile_item_kind kind,
                         const char *uid, const wchar_t *name) {
  mc_sync *sync = sync_bridge_get();
  if (!sync || !uid || (cmd != cmd_remove && cmd != cmd_sync)) return FALSE;
  /* Copied: the dialog's message loop repaints the views, which may reload what they point
   * into. */
  char key[MC_UID_STRLEN];
  snprintf(key, sizeof key, "%s", uid);
  wchar_t *title = _wcsdup(name && *name ? name : mc_wstr(MC_STR_UNKNOWN_VALUE));
  if (!title) return FALSE;
  BOOL changed = FALSE;
  if (cmd == cmd_remove) {
    changed = remove_item(owner, sync, kind, key, title);
  } else if (mc_sync_restore_here(sync, kind, key) == MC_OK) {
    changed = TRUE;
    mc_sync_now(sync, NULL); /* bring it back soon, rather than at the next scheduled sync */
  } else {
    MessageBoxW(owner, mc_wstr(MC_STR_SYNC_HERE_FAILED), mc_wstr(MC_STR_WINDOWS_SYNC_TO_THIS_PC),
                MB_OK | MB_ICONERROR);
  }
  free(title);
  return changed;
}

#include "issue_dialog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "delete_dialog.h"
#include "prompt_dialog.h"
#include "wstr.h"

enum {
  MAX_SHOWN = 5,     /* issues in the dialog; the rest are counted */
  TEXT_CAP = 12288,  /* wide characters */
};

static BOOL g_open;                   /* one of these dialogs is open */
static char g_checked[MC_UID_STRLEN]; /* the lost archive whose offer was seen, this run */

static BOOL is_offer(const mc_sync_issue *is) {
  return is->kind == MC_ISSUE_ARCHIVE_REINSTALL || (is->kind == MC_ISSUE_ARCHIVE_LOST && is->count > 0);
}

static BOOL is_lost_archive(const mc_sync_issue *is) {
  return is->kind == MC_ISSUE_ARCHIVE_REINSTALL || is->kind == MC_ISSUE_ARCHIVE_LOST;
}

/* Adds a paragraph, a blank line after what's there. */
static void append(wchar_t *text, size_t *used, const wchar_t *para) {
  if (!para[0] || *used + 3 >= TEXT_CAP) return;
  int n = swprintf(text + *used, TEXT_CAP - *used, L"%ls%ls", *used ? L"\n\n" : L"", para);
  if (n > 0) *used += (size_t)n;
  else text[*used] = L'\0'; /* cut short: swprintf leaves it undefined */
}

static void append_utf8(wchar_t *text, size_t *used, const char *para) {
  wchar_t *wide = wstr_dup_from_utf8(para);
  if (wide) append(text, used, wide);
  free(wide);
}

BOOL issue_dialog_has_issues(mc_sync *sync, const char *device_uid, BOOL *offer) {
  if (offer) *offer = FALSE;
  mc_sync_problems *p = NULL;
  if (!sync || mc_sync_get_problems(sync, &p) != MC_OK) return FALSE;
  BOOL any = FALSE;
  for (int i = 0; i < p->issue_count; i++) {
    const mc_sync_issue *is = &p->issues[i];
    if (device_uid && strcmp(is->device_uid, device_uid)) continue;
    any = TRUE;
    if (offer && is_offer(is)) *offer = TRUE;
  }
  mc_sync_problems_free(p);
  return any;
}

BOOL issue_dialog_show(HWND owner, mc_library *lib, mc_sync *sync, const char *device_uid) {
  if (!sync || g_open) return FALSE;
  wchar_t *text = calloc(TEXT_CAP, sizeof *text);
  if (!text) return FALSE;
  mc_sync_problems *p = NULL;
  if (mc_sync_get_problems(sync, &p) != MC_OK) p = NULL;
  size_t used = 0;
  int shown = 0, more = 0;
  BOOL notices = FALSE, lost = FALSE;
  char offer_uid[MC_UID_STRLEN] = "";
  for (int i = 0; p && i < p->issue_count; i++) {
    const mc_sync_issue *is = &p->issues[i];
    if (device_uid && strcmp(is->device_uid, device_uid)) continue;
    if (shown == MAX_SHOWN) {
      more++;
      continue;
    }
    notices |= is->notice != 0;
    if (is->kind == MC_ISSUE_LOST) lost = TRUE;
    if (is_offer(is) && !offer_uid[0]) snprintf(offer_uid, sizeof offer_uid, "%s", is->device_uid);
    mc_sync_issue_text t;
    mc_sync_issue_describe(is, 0, &t);
    append_utf8(text, &used, t.title);
    /* The title's paragraph goes on with what happened and the fix, a line each. */
    wchar_t line[1024];
    if (t.body[0] && used + 2 < TEXT_CAP) {
      wstr_from_utf8(line, 1024, t.body);
      int n = swprintf(text + used, TEXT_CAP - used, L"\n%ls", line);
      if (n > 0) used += (size_t)n;
    }
    if (t.fix[0] && used + 2 < TEXT_CAP) {
      mc_wformat(line, 1024, mc_str(MC_STR_SYNC_ISSUE_FIX), MC_S(t.fix));
      int n = swprintf(text + used, TEXT_CAP - used, L"\n%ls", line);
      if (n > 0) used += (size_t)n;
    }
    text[used] = L'\0';
    shown++;
  }
  mc_sync_problems_free(p);
  if (more) { /* "(2 more problems)": the banner's sentence without its first title */
    wchar_t line[128];
    mc_wformat(line, 128, mc_str_plural(MC_STR_SYNC_ISSUES_MORE, more), MC_S(""), MC_D(more));
    const wchar_t *s = line;
    while (*s == L' ') s++;
    append(text, &used, s);
  }
  if (!shown) append(text, &used, mc_wstr(MC_STR_SYNC_ISSUES_NONE));

  /* One thing to do from here: lost tracks' removal, else a lost archive's offer. */
  const wchar_t *action = lost ? mc_wstr(MC_STR_SYNC_LOST_REMOVE)
                          : offer_uid[0] ? mc_wstr(MC_STR_WINDOWS_SYNC_OFFER_OPEN)
                                         : NULL;
  prompt_dialog d = {.caption = mc_wstr(MC_STR_SYNC_ISSUES_TITLE),
                     .icon = shown && !notices ? MB_ICONWARNING : MB_ICONINFORMATION,
                     .text = text,
                     .ok = action ? action : mc_wstr_mnemonic(MC_STR_CLOSE),
                     .cancel = action ? mc_wstr_mnemonic(MC_STR_CLOSE) : NULL};
  g_open = TRUE;
  INT_PTR r = prompt_dialog_show(owner, &d);
  g_open = FALSE;
  free(text);
  if (notices) mc_sync_dismiss_notices(sync);
  if (r != IDOK || !action) return FALSE;
  if (lost) return delete_dialog_lost(owner, lib, sync);
  issue_dialog_offer(owner, sync, offer_uid);
  return FALSE;
}

static void show_offer(HWND owner, mc_sync *sync, const mc_sync_archive_offer *o) {
  mc_sync_offer_text t;
  mc_sync_archive_offer_describe(o, MC_DEVICE_DESKTOP, &t);
  wchar_t *heading = wstr_dup_from_utf8(t.title), *body = wstr_dup_from_utf8(t.body);
  wchar_t *button = t.button[0] ? wstr_dup_from_utf8(t.button) : NULL;
  prompt_dialog d = {.caption = mc_wstr(MC_STR_SYNC_ISSUES_TITLE),
                     .icon = MB_ICONWARNING,
                     .heading = heading,
                     .text = body ? body : L"",
                     .ok = button ? button : mc_wstr_mnemonic(MC_STR_OK),
                     .cancel = button ? mc_wstr(MC_STR_SYNC_OFFER_LATER) : NULL};
  MessageBeep(MB_ICONWARNING);
  g_open = TRUE;
  INT_PTR r = prompt_dialog_show(owner, &d);
  g_open = FALSE;
  mc_sync_archive_offer_seen(sync, o->device_uid);
  snprintf(g_checked, sizeof g_checked, "%s", o->device_uid);
  if (button && r == IDOK) {
    mc_status st = mc_sync_take_over_archive(sync, o->device_uid);
    if (st != MC_OK) {
      wchar_t msg[256];
      mc_wformat(msg, 256, mc_str(MC_STR_WINDOWS_SYNC_TAKEOVER_START_FAILED), MC_S(mc_status_str(st)));
      message_dialog(owner, mc_wstr(MC_STR_SYNC_ISSUES_TITLE), msg, MB_ICONERROR);
    }
  }
  free(heading);
  free(body);
  free(button);
}

void issue_dialog_offer(HWND owner, mc_sync *sync, const char *device_uid) {
  if (!sync || g_open) return;
  mc_sync_archive_offer *o = NULL;
  if (mc_sync_get_archive_offer(sync, &o) != MC_OK) o = NULL;
  if (o && (!device_uid || !strcmp(o->device_uid, device_uid))) show_offer(owner, sync, o);
  else message_dialog(owner, mc_wstr(MC_STR_SYNC_ISSUES_TITLE), mc_wstr(MC_STR_SYNC_ISSUES_NONE), MB_ICONINFORMATION);
  mc_sync_archive_offer_free(o);
}

void issue_dialog_check_offer(HWND owner, mc_sync *sync) {
  /* Not over another dialog: the next change tries again. */
  if (!sync || g_open || !IsWindowEnabled(owner)) return;
  mc_sync_problems *p = NULL;
  if (mc_sync_get_problems(sync, &p) != MC_OK) return;
  BOOL lost = FALSE;
  for (int i = 0; i < p->issue_count && !lost; i++)
    lost = is_lost_archive(&p->issues[i]) && strcmp(p->issues[i].device_uid, g_checked);
  mc_sync_problems_free(p);
  if (!lost) return;
  /* The offer reads every track: only once there's a lost archive not yet seen to. */
  mc_sync_archive_offer *o = NULL;
  if (mc_sync_get_archive_offer(sync, &o) != MC_OK || !o) return;
  if (o->asked) snprintf(g_checked, sizeof g_checked, "%s", o->device_uid);
  else show_offer(owner, sync, o);
  mc_sync_archive_offer_free(o);
}

#include "tethered_bridge.h"

#include <dbt.h>
#include <stdlib.h>
#include <string.h>
#include <winioctl.h>

#include "wstr.h"

enum { PROGRESS_MS = 150 };

static struct {
  HWND wnd; /* set before the mc_tethered exists, so its threads only read it */
  mc_tethered *t;
  volatile LONG gen; /* bumped by every stop; events carry the one they were made in */
  /* Progress throttling: only the callbacks touch these, and core runs them one at a time. */
  ULONGLONG progress_at;
  int progress_phase;
  char progress_uid[MC_UID_STRLEN];
} g_tb = {.progress_phase = -1};

static tethered_event *new_event(tethered_event_kind kind) {
  tethered_event *ev = calloc(1, sizeof *ev);
  if (ev) ev->kind = kind;
  return ev;
}

static void copy_uid(char *dst, const char *src) {
  if (!src) return;
  strncpy(dst, src, MC_UID_STRLEN - 1);
  dst[MC_UID_STRLEN - 1] = '\0';
}

/* Never SendMessage: the UI thread may be waiting in mc_tethered_free() for this thread. */
static void post(void *user, tethered_event *ev) {
  if (!ev) return;
  if (!PostMessageW(g_tb.wnd, WM_APP_TETHERED, (WPARAM)(uintptr_t)user, (LPARAM)ev)) free(ev);
}

static void on_devices(void *user) {
  post(user, new_event(TETHERED_EV_DEVICES));
}

static void on_progress(void *user, const mc_tethered_progress *p) {
  ULONGLONG now = GetTickCount64();
  const char *uid = p->device_uid ? p->device_uid : "";
  BOOL same = (int)p->phase == g_tb.progress_phase && !strcmp(uid, g_tb.progress_uid);
  if (same && now - g_tb.progress_at < PROGRESS_MS && p->done < p->total) return;
  g_tb.progress_at = now;
  g_tb.progress_phase = (int)p->phase;
  copy_uid(g_tb.progress_uid, uid);
  tethered_event *ev = new_event(TETHERED_EV_PROGRESS);
  if (!ev) return;
  copy_uid(ev->uid, uid);
  ev->phase = p->phase;
  ev->done = p->done;
  ev->total = p->total;
  ev->bytes_done = p->bytes_done;
  ev->bytes_total = p->bytes_total;
  if (p->title) {
    size_t len = strlen(p->title);
    if (len >= sizeof ev->title) {
      len = sizeof ev->title - 1;
      while (len > 0 && ((unsigned char)p->title[len] & 0xC0) == 0x80) len--; /* not mid-character */
    }
    memcpy(ev->title, p->title, len);
    ev->title[len] = '\0';
  }
  post(user, ev);
}

static void on_done(void *user, const mc_tethered_result *r) {
  g_tb.progress_phase = -1;
  tethered_event *ev = new_event(TETHERED_EV_DONE);
  if (!ev) return;
  copy_uid(ev->uid, r->device_uid);
  ev->result = *r;
  ev->result.device_uid = NULL;
  post(user, ev);
}

/* --- Volumes --- */

/* Whether drive letter's volume may be a device: removable or fixed, not the network's, an
 * optical disc's or a RAM disk's, any of which could be slow or pointless to probe. */
static BOOL probe_worthy(const wchar_t *root) {
  UINT type = GetDriveTypeW(root);
  return type == DRIVE_REMOVABLE || type == DRIVE_FIXED;
}

static void announce(wchar_t letter, BOOL added) {
  if (!g_tb.t) return;
  wchar_t root[4] = {letter, L':', L'\\', 0};
  char path[8];
  if (!WideCharToMultiByte(CP_UTF8, 0, root, -1, path, (int)sizeof path, NULL, NULL)) return;
  if (!added) {
    mc_tethered_volume_removed(g_tb.t, path);
    return;
  }
  if (probe_worthy(root)) mc_tethered_volume_added(g_tb.t, path, NULL);
}

/* KITHSONG_TEST_VOLUMES: folders, separated by ;, announced as volumes. */
static void announce_test_volumes(void) {
  const wchar_t *list = _wgetenv(L"KITHSONG_TEST_VOLUMES");
  if (!list || !*list || !g_tb.t) return;
  wchar_t *copy = _wcsdup(list), *ctx = NULL;
  for (wchar_t *dir = copy ? wcstok_s(copy, L";", &ctx) : NULL; dir; dir = wcstok_s(NULL, L";", &ctx)) {
    char *utf8 = wstr_to_utf8(dir);
    if (utf8) mc_tethered_volume_added(g_tb.t, utf8, NULL);
    free(utf8);
  }
  free(copy);
}

void tethered_bridge_scan(void) {
  if (!g_tb.t) return;
  DWORD drives = GetLogicalDrives();
  for (int i = 0; i < 26; i++)
    if (drives & (1u << i)) announce((wchar_t)(L'A' + i), TRUE);
  announce_test_volumes();
}

void tethered_bridge_device_change(WPARAM wp, LPARAM lp) {
  if (!g_tb.t || (wp != DBT_DEVICEARRIVAL && wp != DBT_DEVICEREMOVECOMPLETE) || !lp) return;
  const DEV_BROADCAST_HDR *hdr = (const DEV_BROADCAST_HDR *)lp;
  if (hdr->dbch_devicetype != DBT_DEVTYP_VOLUME) return;
  DWORD mask = ((const DEV_BROADCAST_VOLUME *)lp)->dbcv_unitmask;
  for (int i = 0; i < 26; i++)
    if (mask & (1u << i)) announce((wchar_t)(L'A' + i), wp == DBT_DEVICEARRIVAL);
}

/* --- Lifetime --- */

void tethered_bridge_start(HWND wnd, mc_library *lib, const char *config_dir) {
  tethered_bridge_stop();
  g_tb.wnd = wnd;
  g_tb.progress_phase = -1;
  if (!lib || !config_dir) return;
  static const mc_tethered_events events = {on_devices, on_progress, on_done};
  void *gen = (void *)(uintptr_t)(ULONG)g_tb.gen;
  if (mc_tethered_new(lib, config_dir, &events, gen, &g_tb.t) != MC_OK) {
    g_tb.t = NULL;
    InterlockedIncrement(&g_tb.gen);
    return;
  }
  tethered_bridge_scan();
}

void tethered_bridge_stop(void) {
  if (!g_tb.t) return;
  InterlockedIncrement(&g_tb.gen); /* whatever the threads post while stopping is stale */
  mc_tethered_free(g_tb.t);
  g_tb.t = NULL;
}

mc_tethered *tethered_bridge_get(void) {
  return g_tb.t;
}

void tethered_bridge_set_scrobbler(mc_scrobbler *sc) {
  if (g_tb.t) mc_tethered_set_scrobbler(g_tb.t, sc);
}

BOOL tethered_bridge_has(const char *uid) {
  if (!g_tb.t || !uid || !*uid) return FALSE;
  mc_tethered_device_list *list = NULL;
  BOOL found = FALSE;
  if (mc_tethered_list(g_tb.t, &list) == MC_OK && list)
    for (int i = 0; i < list->count && !found; i++) found = !strcmp(list->items[i].uid, uid);
  mc_tethered_device_list_free(list);
  return found;
}

tethered_event *tethered_bridge_receive(WPARAM wp, LPARAM lp) {
  tethered_event *ev = (tethered_event *)lp;
  if (!ev) return NULL;
  if (!g_tb.t || (ULONG)wp != (ULONG)g_tb.gen) {
    free(ev);
    return NULL;
  }
  return ev;
}

/* --- Eject --- */

mc_status tethered_bridge_eject_volume(const char *mount_path) {
  if (!mount_path) return MC_ERR_INVALID_ARG;
  /* Only a drive's root: "E:\" or "E:". */
  size_t len = strlen(mount_path);
  BOOL root = (len == 2 || (len == 3 && (mount_path[2] == '\\' || mount_path[2] == '/'))) && mount_path[1] == ':' &&
              ((mount_path[0] >= 'A' && mount_path[0] <= 'Z') || (mount_path[0] >= 'a' && mount_path[0] <= 'z'));
  if (!root) return MC_OK;
  wchar_t device[8] = L"\\\\.\\X:";
  device[4] = (wchar_t)mount_path[0];
  HANDLE vol = CreateFileW(device, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, 0, NULL);
  if (vol == INVALID_HANDLE_VALUE) return MC_ERR_IO;
  DWORD bytes;
  BOOL locked = FALSE;
  for (int i = 0; i < 10 && !locked; i++) { /* the shell may hold it a moment */
    locked = DeviceIoControl(vol, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &bytes, NULL);
    if (!locked) Sleep(100);
  }
  mc_status st = MC_ERR_IN_USE;
  if (locked) {
    FlushFileBuffers(vol);
    st = MC_ERR_IO;
    if (DeviceIoControl(vol, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &bytes, NULL)) {
      PREVENT_MEDIA_REMOVAL allow = {FALSE};
      DeviceIoControl(vol, IOCTL_STORAGE_MEDIA_REMOVAL, &allow, sizeof allow, NULL, 0, &bytes, NULL);
      /* A device with no media to eject still counts: it's dismounted, so safe to unplug. */
      DeviceIoControl(vol, IOCTL_STORAGE_EJECT_MEDIA, NULL, 0, NULL, 0, &bytes, NULL);
      st = MC_OK;
    }
  }
  CloseHandle(vol);
  return st;
}

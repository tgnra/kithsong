#define COBJMACROS
#include "firewall.h"

#include <commctrl.h>
#include <initguid.h> /* defines netfw.h's CLSIDs and IIDs here: libuuid has none of them */
#include <netfw.h>
#include <oleauto.h>
#include <shellapi.h>
#include <stdint.h>
#include <stdio.h>
#include <wchar.h>

#include "musiccore/sync.h"
#include "sync_bridge.h"
#include "wstr.h"

#define SETTING_DONT_ASK "firewall_dont_ask"
#define RULE_GROUP L"musicapp" /* the rule's Grouping, which Windows' firewall settings show */
#define RULE_PROFILES (NET_FW_PROFILE2_PRIVATE | NET_FW_PROFILE2_DOMAIN)
#define PROTOCOL_ANY 256 /* NET_FW_IP_PROTOCOL_ANY, which mingw's icftypes.h leaves out */

enum {
  WM_FW_CHECKED = WM_APP + 1, /* wParam: TRUE if the rule is missing */
  WM_FW_ADDED,                /* wParam: the helper's exit code, an HRESULT */
  TIMER_CHECK = 1,
  /* After startup: the window has drawn and the library loaded first. */
  CHECK_DELAY_MS = 2000,
  BTN_ALLOW = 100,
  BTN_NOT_NOW,
  MAX_EXE = 1024,
};

static struct {
  HWND wnd; /* message-only: the threads report to it */
  HWND owner;
  mc_settings *settings;
  wchar_t exe[MAX_EXE]; /* this exe's full path, which the rule names */
  BOOL busy;            /* a check, the dialog or the helper is under way */
} g_fw;

static BOOL find_exe(void) {
  DWORD n = GetModuleFileNameW(NULL, g_fw.exe, MAX_EXE);
  return n > 0 && n < MAX_EXE;
}

/* --- Reading the rules --- */

/* A rule's program, which may hold environment variables, is this exe. */
static BOOL is_this_exe(const wchar_t *app) {
  wchar_t expanded[MAX_EXE];
  DWORD n = ExpandEnvironmentStringsW(app, expanded, MAX_EXE);
  return n > 0 && n <= MAX_EXE && CompareStringOrdinal(expanded, -1, g_fw.exe, -1, TRUE) == CSTR_EQUAL;
}

/* A rule's LocalPorts ("*", "47823", "47000-48000" or a comma-separated list of those; NULL
 * or "" for any) includes port. Keywords such as "RPC" never do. */
static BOOL port_listed(const wchar_t *ports, unsigned port) {
  if (!ports || !*ports) return TRUE;
  for (const wchar_t *p = ports; p; p = wcschr(p, L','), p = p ? p + 1 : NULL) {
    while (*p == L' ') p++;
    if (*p == L'*') return TRUE;
    wchar_t *end;
    unsigned long lo = wcstoul(p, &end, 10), hi = lo;
    if (end == p) continue;
    if (*end == L'-') hi = wcstoul(end + 1, &end, 10);
    if (port >= lo && port <= hi) return TRUE;
  }
  return FALSE;
}

/* What the firewall's rules say about sync's inbound connections, as profile masks. */
typedef struct verdict {
  long allow, block; /* enabled rules allowing or blocking them */
  BOOL mentioned;    /* some rule names this exe, enabled or not */
} verdict;

/* Adds rule r to v if it's about us: an inbound rule naming this exe, for TCP or any
 * protocol, or naming no program but TCP on sync's port. Rules for a service, or for a
 * program-less protocol other than TCP, are left out: those are Windows' own, and often
 * narrower than they look (an interface type, a remote address). */
static void weigh(INetFwRule *r, unsigned port, verdict *v) {
  NET_FW_RULE_DIRECTION dir = 0;
  if (FAILED(INetFwRule_get_Direction(r, &dir)) || dir != NET_FW_RULE_DIR_IN) return;
  BSTR app = NULL, service = NULL, ports = NULL;
  long protocol = 0, profiles = 0;
  VARIANT_BOOL enabled = VARIANT_FALSE;
  NET_FW_ACTION action = NET_FW_ACTION_BLOCK;
  INetFwRule_get_ApplicationName(r, &app);
  INetFwRule_get_ServiceName(r, &service);
  INetFwRule_get_Protocol(r, &protocol);
  BOOL ours;
  if (service && *service && wcscmp(service, L"*")) {
    ours = FALSE;
  } else if (app && *app) {
    ours = is_this_exe(app);
    if (ours) v->mentioned = TRUE;
    ours = ours && (protocol == PROTOCOL_ANY || protocol == NET_FW_IP_PROTOCOL_TCP);
  } else {
    ours = protocol == NET_FW_IP_PROTOCOL_TCP && SUCCEEDED(INetFwRule_get_LocalPorts(r, &ports)) &&
           port_listed(ports, port);
  }
  if (ours && SUCCEEDED(INetFwRule_get_Enabled(r, &enabled)) && enabled &&
      SUCCEEDED(INetFwRule_get_Profiles(r, &profiles)) && SUCCEEDED(INetFwRule_get_Action(r, &action))) {
    if (action == NET_FW_ACTION_ALLOW)
      v->allow |= profiles;
    else
      v->block |= profiles;
  }
  SysFreeString(app);
  SysFreeString(service);
  SysFreeString(ports);
}

static HRESULT weigh_rules(INetFwPolicy2 *policy, unsigned port, verdict *v) {
  INetFwRules *rules = NULL;
  IUnknown *unk = NULL;
  IEnumVARIANT *it = NULL;
  HRESULT hr = INetFwPolicy2_get_Rules(policy, &rules);
  if (SUCCEEDED(hr)) hr = INetFwRules_get__NewEnum(rules, &unk);
  if (SUCCEEDED(hr)) hr = IUnknown_QueryInterface(unk, &IID_IEnumVARIANT, (void **)&it);
  if (SUCCEEDED(hr)) {
    VARIANT var;
    VariantInit(&var);
    ULONG got = 0;
    while (IEnumVARIANT_Next(it, 1, &var, &got) == S_OK && got == 1) {
      INetFwRule *r = NULL;
      if (V_VT(&var) == VT_DISPATCH && V_DISPATCH(&var) &&
          SUCCEEDED(IDispatch_QueryInterface(V_DISPATCH(&var), &IID_INetFwRule, (void **)&r))) {
        weigh(r, port, v);
        INetFwRule_Release(r);
      }
      VariantClear(&var);
    }
    IEnumVARIANT_Release(it);
  }
  if (unk) IUnknown_Release(unk);
  if (rules) INetFwRules_Release(rules);
  return hr;
}

/* Whether firewall profile p lets sync in whatever the rules say: its firewall is off or
 * lets everything in. *shut: it lets nothing in whatever the rules say. */
static BOOL profile_open(INetFwPolicy2 *policy, NET_FW_PROFILE_TYPE2 p, BOOL *shut) {
  VARIANT_BOOL on = VARIANT_TRUE, block_all = VARIANT_FALSE;
  NET_FW_ACTION in = NET_FW_ACTION_BLOCK;
  INetFwPolicy2_get_FirewallEnabled(policy, p, &on);
  INetFwPolicy2_get_BlockAllInboundTraffic(policy, p, &block_all);
  INetFwPolicy2_get_DefaultInboundAction(policy, p, &in);
  *shut = on && block_all;
  return !on || (!block_all && in == NET_FW_ACTION_ALLOW);
}

/* TRUE if sync needs the rule: the firewall stops other devices connecting on the networks
 * this PC is on now, and on private ones, and adding it would change that. Quiet (FALSE)
 * whenever in doubt, including when the firewall can't be read. */
static BOOL rule_missing(unsigned port) {
  INetFwPolicy2 *policy = NULL;
  if (FAILED(CoCreateInstance(&CLSID_NetFwPolicy2, NULL, CLSCTX_INPROC_SERVER, &IID_INetFwPolicy2,
                              (void **)&policy)))
    return FALSE;
  long current = 0;
  INetFwPolicy2_get_CurrentProfileTypes(policy, &current);
  static const NET_FW_PROFILE_TYPE2 kinds[] = {NET_FW_PROFILE2_DOMAIN, NET_FW_PROFILE2_PRIVATE,
                                               NET_FW_PROFILE2_PUBLIC};
  long guarded = 0;  /* current profiles whose firewall could stop sync */
  long notifies = 0; /* ... and that show Windows' own alert */
  for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++) {
    BOOL shut;
    if (!(current & kinds[i]) || profile_open(policy, kinds[i], &shut)) continue;
    guarded |= kinds[i];
    VARIANT_BOOL quiet = VARIANT_TRUE;
    if (SUCCEEDED(INetFwPolicy2_get_NotificationsDisabled(policy, kinds[i], &quiet)) && !quiet)
      notifies |= kinds[i];
  }
  BOOL shut_private;
  BOOL missing = guarded && !profile_open(policy, NET_FW_PROFILE2_PRIVATE, &shut_private) && !shut_private;
  verdict v = {0};
  if (missing && FAILED(weigh_rules(policy, port, &v))) missing = FALSE;
  /* Sync gets in on a network this PC is on now, or the rule is there for private ones;
   * or the user blocked musicapp on private networks, which a rule of ours wouldn't undo. */
  if (missing && ((v.allow & ~v.block & guarded) || (v.allow & NET_FW_PROFILE2_PRIVATE) ||
                  (v.block & NET_FW_PROFILE2_PRIVATE)))
    missing = FALSE;
  /* Windows asks for itself the first time a program it has no rule for listens, and adds
   * the rule the user picks, so we'd only ask twice. */
  if (missing && !v.mentioned && notifies) missing = FALSE;
  INetFwPolicy2_Release(policy);
  return missing;
}

static DWORD WINAPI check_main(void *arg) {
  unsigned port = (unsigned)(uintptr_t)arg;
  BOOL missing = FALSE;
  if (SUCCEEDED(CoInitializeEx(NULL, COINIT_MULTITHREADED))) {
    missing = rule_missing(port);
    CoUninitialize();
  }
  if (!PostMessageW(g_fw.wnd, WM_FW_CHECKED, missing, 0)) g_fw.busy = FALSE;
  return 0;
}

/* --- Adding the rule --- */

/* The elevated helper's work. */
static HRESULT add_rule(void) {
  INetFwRule *rule = NULL;
  INetFwPolicy2 *policy = NULL;
  INetFwRules *rules = NULL;
  BSTR name = SysAllocString(mc_wstr(MC_STR_APP_NAME));
  BSTR description = SysAllocString(mc_wstr(MC_STR_WINDOWS_FIREWALL_RULE_DESCRIPTION));
  BSTR app = SysAllocString(g_fw.exe);
  BSTR group = SysAllocString(RULE_GROUP);
  HRESULT hr = name && description && app && group ? S_OK : E_OUTOFMEMORY;
  if (SUCCEEDED(hr))
    hr = CoCreateInstance(&CLSID_NetFwRule, NULL, CLSCTX_INPROC_SERVER, &IID_INetFwRule, (void **)&rule);
  if (SUCCEEDED(hr)) hr = INetFwRule_put_Name(rule, name);
  if (SUCCEEDED(hr)) hr = INetFwRule_put_Description(rule, description);
  if (SUCCEEDED(hr)) hr = INetFwRule_put_Grouping(rule, group);
  if (SUCCEEDED(hr)) hr = INetFwRule_put_ApplicationName(rule, app);
  if (SUCCEEDED(hr)) hr = INetFwRule_put_Protocol(rule, PROTOCOL_ANY); /* TCP sync, UDP mDNS */
  if (SUCCEEDED(hr)) hr = INetFwRule_put_Direction(rule, NET_FW_RULE_DIR_IN);
  if (SUCCEEDED(hr)) hr = INetFwRule_put_Action(rule, NET_FW_ACTION_ALLOW);
  if (SUCCEEDED(hr)) hr = INetFwRule_put_Profiles(rule, RULE_PROFILES);
  if (SUCCEEDED(hr)) hr = INetFwRule_put_Enabled(rule, VARIANT_TRUE);
  if (SUCCEEDED(hr))
    hr = CoCreateInstance(&CLSID_NetFwPolicy2, NULL, CLSCTX_INPROC_SERVER, &IID_INetFwPolicy2,
                          (void **)&policy);
  if (SUCCEEDED(hr)) hr = INetFwPolicy2_get_Rules(policy, &rules);
  if (SUCCEEDED(hr)) hr = INetFwRules_Add(rules, rule);
  if (rules) INetFwRules_Release(rules);
  if (policy) INetFwPolicy2_Release(policy);
  if (rule) INetFwRule_Release(rule);
  SysFreeString(name);
  SysFreeString(description);
  SysFreeString(app);
  SysFreeString(group);
  return hr;
}

BOOL firewall_helper_main(int *exit_code) {
  int argc = 0;
  wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  BOOL helper = argv && argc == 2 && !wcscmp(argv[1], FIREWALL_HELPER_ARG);
  if (argv) LocalFree(argv);
  if (!helper) return FALSE;
  HRESULT hr = find_exe() ? CoInitializeEx(NULL, COINIT_APARTMENTTHREADED) : E_FAIL;
  if (SUCCEEDED(hr)) {
    hr = add_rule();
    CoUninitialize();
  }
  *exit_code = (int)hr;
  return TRUE;
}

/* Waits for the helper, whose handle arg is, and reports its exit code. */
static DWORD WINAPI wait_main(void *arg) {
  HANDLE proc = arg;
  DWORD code = (DWORD)E_FAIL;
  if (WaitForSingleObject(proc, INFINITE) != WAIT_OBJECT_0 || !GetExitCodeProcess(proc, &code))
    code = (DWORD)E_FAIL;
  CloseHandle(proc);
  if (!PostMessageW(g_fw.wnd, WM_FW_ADDED, code, 0)) g_fw.busy = FALSE;
  return 0;
}

static void show_failure(HRESULT hr) {
  char code[16];
  snprintf(code, sizeof code, "0x%08lX", (unsigned long)hr);
  wchar_t text[256];
  mc_wformat(text, 256, mc_str(MC_STR_WINDOWS_FIREWALL_ADD_FAILED), MC_S(code));
  MessageBoxW(g_fw.owner, text, mc_wstr(MC_STR_APP_NAME), MB_OK | MB_ICONERROR);
}

/* Relaunches this exe elevated to add the rule. The user saying no to UAC isn't an error. */
static BOOL start_helper(void) {
  SHELLEXECUTEINFOW sei = {
    .cbSize = sizeof sei,
    .fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC,
    .hwnd = g_fw.owner,
    .lpVerb = L"runas",
    .lpFile = g_fw.exe,
    .lpParameters = FIREWALL_HELPER_ARG,
    .nShow = SW_HIDE,
  };
  if (!ShellExecuteExW(&sei)) {
    DWORD err = GetLastError();
    if (err != ERROR_CANCELLED) show_failure(HRESULT_FROM_WIN32(err));
    return FALSE;
  }
  if (!sei.hProcess) {
    show_failure(E_FAIL);
    return FALSE;
  }
  HANDLE thread = CreateThread(NULL, 0, wait_main, sei.hProcess, 0, NULL);
  if (!thread) {
    CloseHandle(sei.hProcess);
    show_failure(HRESULT_FROM_WIN32(GetLastError()));
    return FALSE;
  }
  CloseHandle(thread);
  return TRUE;
}

/* The networks this PC is on now are all public, which the rule doesn't cover. */
static BOOL on_public_only(void) {
  INetFwPolicy2 *policy = NULL;
  long current = 0;
  if (SUCCEEDED(CoCreateInstance(&CLSID_NetFwPolicy2, NULL, CLSCTX_INPROC_SERVER, &IID_INetFwPolicy2,
                                 (void **)&policy))) {
    INetFwPolicy2_get_CurrentProfileTypes(policy, &current);
    INetFwPolicy2_Release(policy);
  }
  return (current & NET_FW_PROFILE2_PUBLIC) && !(current & RULE_PROFILES);
}

static void helper_done(HRESULT hr) {
  if (FAILED(hr)) {
    show_failure(hr);
    return;
  }
  MessageBoxW(g_fw.owner,
              mc_wstr(on_public_only() ? MC_STR_WINDOWS_FIREWALL_ADDED_PUBLIC : MC_STR_WINDOWS_FIREWALL_ADDED),
              mc_wstr(MC_STR_APP_NAME), MB_OK | MB_ICONINFORMATION);
  /* Devices that found this PC blocked find out sooner. */
  mc_sync *sync = sync_bridge_get();
  if (sync) mc_sync_check_reach(sync, NULL);
}

/* --- Asking --- */

static HRESULT CALLBACK dialog_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp, LONG_PTR data) {
  (void)wp;
  (void)lp;
  (void)data;
  if (msg == TDN_CREATED) SendMessageW(dlg, TDM_SET_BUTTON_ELEVATION_REQUIRED_STATE, BTN_ALLOW, TRUE);
  return S_OK;
}

/* Returns TRUE if the helper started, and reports later. */
static BOOL ask(void) {
  const TASKDIALOG_BUTTON buttons[] = {
    {BTN_ALLOW, mc_wstr_mnemonic(MC_STR_WINDOWS_FIREWALL_ALLOW)},
    {BTN_NOT_NOW, mc_wstr_mnemonic(MC_STR_WINDOWS_FIREWALL_NOT_NOW)},
  };
  TASKDIALOGCONFIG config = {
    .cbSize = sizeof config,
    .hwndParent = g_fw.owner,
    .dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW,
    .pszWindowTitle = mc_wstr(MC_STR_APP_NAME),
    .pszMainIcon = TD_INFORMATION_ICON,
    .pszMainInstruction = mc_wstr(MC_STR_WINDOWS_FIREWALL_TITLE),
    .pszContent = mc_wstr(MC_STR_WINDOWS_FIREWALL_TEXT),
    .cButtons = sizeof buttons / sizeof buttons[0],
    .pButtons = buttons,
    .nDefaultButton = BTN_ALLOW,
    .pszVerificationText = mc_wstr_mnemonic(MC_STR_WINDOWS_FIREWALL_DONT_ASK),
    .pfCallback = dialog_proc,
  };
  int button = 0;
  BOOL dont_ask = FALSE;
  if (FAILED(TaskDialogIndirect(&config, &button, NULL, &dont_ask))) return FALSE;
  if (dont_ask) {
    mc_settings_set_bool(g_fw.settings, SETTING_DONT_ASK, 1);
    mc_settings_save(g_fw.settings); /* failing, it's asked again next time */
  }
  return button == BTN_ALLOW && start_helper();
}

static LRESULT CALLBACK fw_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_TIMER: {
      KillTimer(wnd, TIMER_CHECK);
      mc_sync *sync = sync_bridge_get();
      HANDLE thread = sync ? CreateThread(NULL, 0, check_main, (void *)(uintptr_t)mc_sync_port(sync), 0, NULL) : NULL;
      if (thread)
        CloseHandle(thread);
      else
        g_fw.busy = FALSE;
      return 0;
    }
    case WM_FW_CHECKED:
      /* Not while the window is away (Move Library), or once sync has stopped. */
      if (!wp || !IsWindowVisible(g_fw.owner) || !sync_bridge_get() || !ask()) g_fw.busy = FALSE;
      return 0;
    case WM_FW_ADDED:
      g_fw.busy = FALSE;
      helper_done((HRESULT)wp);
      return 0;
    default:
      return DefWindowProcW(wnd, msg, wp, lp);
  }
}

void firewall_check_later(HWND owner, mc_settings *settings) {
  if (g_fw.busy || !sync_bridge_get() || mc_settings_bool(settings, SETTING_DONT_ASK, 0) || !find_exe())
    return;
  if (!g_fw.wnd) {
    WNDCLASSW wc = {.lpfnWndProc = fw_proc, .hInstance = GetModuleHandleW(NULL), .lpszClassName = L"musicapp_firewall"};
    RegisterClassW(&wc);
    g_fw.wnd = CreateWindowExW(0, wc.lpszClassName, NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);
    if (!g_fw.wnd) return;
  }
  g_fw.owner = owner;
  g_fw.settings = settings;
  g_fw.busy = TRUE;
  SetTimer(g_fw.wnd, TIMER_CHECK, CHECK_DELAY_MS, NULL);
}

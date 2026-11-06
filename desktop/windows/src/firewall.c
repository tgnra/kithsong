#define COBJMACROS
#include <winsock2.h> /* before windows.h, and ws2ipdef.h for netioapi.h's MIB_IF_ROW2 */
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <netioapi.h>

#include "firewall.h"

#include <initguid.h> /* defines netfw.h's CLSIDs and IIDs here: libuuid has none of them */
#include <netfw.h>
#include <netlistmgr.h>
#include <oleauto.h>
#include <shellapi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "kithcore/source_run.h"
#include "kithcore/sync.h"
#include "prompt_dialog.h"
#include "sync_bridge.h"
#include "wstr.h"

#define SETTING_DONT_ASK "firewall_dont_ask"
#define RULE_GROUP L"Kithsong" /* the rule's Grouping, which Windows' firewall settings show */
#define RULE_PROFILES (NET_FW_PROFILE2_PRIVATE | NET_FW_PROFILE2_DOMAIN)
#define PROTOCOL_ANY 256 /* NET_FW_IP_PROTOCOL_ANY, which mingw's icftypes.h leaves out */
#define RULE_CATEGORY_FIREWALL 2 /* NET_FW_RULE_CATEGORY_FIREWALL, which it leaves out too */

enum {
  WM_FW_CHECKED = WM_APP + 1, /* lParam: a check *, which the receiver frees */
  WM_FW_ADDED,                /* wParam: the helper's exit code, an HRESULT */
  TIMER_CHECK = 1,
  TIMER_WATCH,
  /* After startup: the window has drawn and the library loaded first. */
  CHECK_DELAY_MS = 2000,
  /* While the page shows, for what no message tells us: a network changing, or a rule added
   * in Windows' own settings while Kithsong stays in front. A check takes about 400 ms, all
   * of it on a thread. Coming back to the app checks at once (firewall_recheck()). */
  WATCH_MS = 20000,
  MAX_EXE = 1024,
  MAX_STALE = 16, /* old rules removed at once */
};

/* One check, on its thread: what it was asked, then what it found. */
typedef struct check {
  unsigned port, seq;
  BOOL ask;     /* for the startup prompt, which acts on missing */
  BOOL missing; /* the prompt should offer the rule */
  firewall_status status;
} check;

static struct {
  HWND wnd; /* message-only: the threads report to it */
  HWND owner;
  mc_settings *settings;
  wchar_t exe[MAX_EXE];    /* this exe's full path, which the rule names */
  const wchar_t *rule_name; /* the name add_rule() gives it: mc_wstr() is for the UI thread */
  BOOL busy;               /* the prompt's check, the dialog or the helper is under way */
  BOOL asked;              /* the check is Settings' Ask Again, which says when there's no need */
  BOOL adding_public;      /* the helper running adds the rule for public networks */
  BOOL watching;           /* firewall_watch() is on */
  int running;             /* checks on their threads */
  unsigned seq;            /* of the last check started: only its status counts */
  firewall_status status;
  void (*listener)(void);
#ifndef NDEBUG
  BOOL forced_allowed; /* the debug override's Allow "worked" */
#endif
} g_fw;

/* Running from an MSIX package. Its manifest declares the rule, which Windows adds at
 * install, and a packaged exe relaunched elevated needs a restricted capability (and may not
 * match a rule naming its path), so the check and the helper are left out. */
static BOOL packaged(void) {
  typedef LONG(WINAPI * package_name_fn)(UINT32 *, PWSTR);
  HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
  package_name_fn get = (package_name_fn)(void (*)(void))GetProcAddress(kernel, "GetCurrentPackageFullName");
  UINT32 n = 0;
  return get && get(&n, NULL) == ERROR_INSUFFICIENT_BUFFER; /* else APPMODEL_ERROR_NO_PACKAGE */
}

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
  BOOL moved;        /* a rule of ours names an exe that's gone */
} verdict;

static BOOL stale(INetFwRule *r, const wchar_t *name);

/* Adds rule r to v if it's about us: an inbound rule naming this exe, for TCP or any
 * protocol, or naming no program but TCP on sync's port. Rules for a service, or for a
 * program-less protocol other than TCP, are left out: those are Windows' own, and often
 * narrower than they look (an interface type, a remote address). */
static void weigh(INetFwRule *r, unsigned port, verdict *v) {
  if (stale(r, g_fw.rule_name)) v->moved = TRUE;
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

static const NET_FW_PROFILE_TYPE2 kinds[] = {NET_FW_PROFILE2_DOMAIN, NET_FW_PROFILE2_PRIVATE,
                                             NET_FW_PROFILE2_PUBLIC};
#define NUM_KINDS (sizeof kinds / sizeof kinds[0])

/* TRUE if the startup prompt should offer the rule: the firewall stops other devices
 * connecting on the networks this PC is on now (current), and on private ones, and adding
 * it would change that. Quiet (FALSE) whenever in doubt. Stricter than the banner, which
 * interrupts nobody: see judge(). */
static BOOL rule_missing(INetFwPolicy2 *policy, long current, const verdict *v) {
  long guarded = 0;  /* current profiles whose firewall could stop sync */
  long notifies = 0; /* ... and that show Windows' own alert */
  for (size_t i = 0; i < NUM_KINDS; i++) {
    BOOL shut;
    if (!(current & kinds[i]) || profile_open(policy, kinds[i], &shut)) continue;
    guarded |= kinds[i];
    VARIANT_BOOL quiet = VARIANT_TRUE;
    if (SUCCEEDED(INetFwPolicy2_get_NotificationsDisabled(policy, kinds[i], &quiet)) && !quiet)
      notifies |= kinds[i];
  }
  BOOL shut_private;
  if (!guarded || profile_open(policy, NET_FW_PROFILE2_PRIVATE, &shut_private) || shut_private) return FALSE;
  /* Sync gets in on a network this PC is on now, or the rule is there for private ones;
   * or the user blocked Kithsong on private networks, which a rule of ours wouldn't undo. */
  if ((v->allow & ~v->block & guarded) || (v->allow & NET_FW_PROFILE2_PRIVATE) || (v->block & NET_FW_PROFILE2_PRIVATE))
    return FALSE;
  /* Windows asks for itself the first time a program it has no rule for listens, and adds
   * the rule the user picks, so we'd only ask twice. */
  return v->mentioned || !notifies;
}

/* Network n isn't only a virtual adapter's. A VPN or a virtual machine's switch often comes
 * up as public beside a home network that's fine, and devices don't sync across those, so
 * saying it's blocked would be a false alarm. In doubt it counts. */
static BOOL on_real_adapter(INetwork *n) {
  IEnumNetworkConnections *conns = NULL;
  if (FAILED(INetwork_GetNetworkConnections(n, &conns))) return TRUE;
  BOOL real = FALSE, any = FALSE;
  INetworkConnection *c = NULL;
  ULONG got = 0;
  while (!real && IEnumNetworkConnections_Next(conns, 1, &c, &got) == S_OK && got == 1) {
    GUID adapter;
    MIB_IF_ROW2 row = {0};
    any = TRUE;
    real = FAILED(INetworkConnection_GetAdapterId(c, &adapter)) ||
           ConvertInterfaceGuidToLuid(&adapter, &row.InterfaceLuid) != NO_ERROR || GetIfEntry2(&row) != NO_ERROR ||
           row.InterfaceAndOperStatusFlags.HardwareInterface;
    INetworkConnection_Release(c);
  }
  IEnumNetworkConnections_Release(conns);
  return real || !any;
}

/* The networks this PC is connected to, by name and firewall profile, into nets (room for
 * FIREWALL_MAX_NETS). Returns how many, or -1 if Windows wouldn't say. */
static int read_networks(firewall_net *nets) {
  INetworkListManager *list = NULL;
  IEnumNetworks *it = NULL;
  int count = -1;
  if (SUCCEEDED(CoCreateInstance(&CLSID_NetworkListManager, NULL, CLSCTX_ALL, &IID_INetworkListManager,
                                 (void **)&list)) &&
      SUCCEEDED(INetworkListManager_GetNetworks(list, NLM_ENUM_NETWORK_CONNECTED, &it))) {
    count = 0;
    INetwork *n = NULL;
    ULONG got = 0;
    while (count < FIREWALL_MAX_NETS && IEnumNetworks_Next(it, 1, &n, &got) == S_OK && got == 1) {
      NLM_NETWORK_CATEGORY category = NLM_NETWORK_CATEGORY_PUBLIC;
      BSTR name = NULL;
      if (SUCCEEDED(INetwork_GetCategory(n, &category)) && on_real_adapter(n)) {
        firewall_net *net = &nets[count++];
        net->profile = category == NLM_NETWORK_CATEGORY_PRIVATE                ? FIREWALL_PRIVATE
                       : category == NLM_NETWORK_CATEGORY_DOMAIN_AUTHENTICATED ? FIREWALL_DOMAIN
                                                                               : FIREWALL_PUBLIC;
        if (SUCCEEDED(INetwork_GetName(n, &name)) && name) lstrcpynW(net->name, name, FIREWALL_NAME_MAX);
        SysFreeString(name);
      }
      INetwork_Release(n);
    }
    IEnumNetworks_Release(it);
  }
  if (list) INetworkListManager_Release(list);
  return count;
}

/* Product p says it's in charge of firewall rules, rather than of boot-time or stealth
 * filtering alone. An answer in a shape we don't know counts as yes: it registered with
 * the firewall, so "can't check" is the honest thing to say. */
static BOOL filters_inbound(INetFwProduct *p) {
  VARIANT categories;
  VariantInit(&categories);
  if (FAILED(INetFwProduct_get_RuleCategories(p, &categories))) return TRUE;
  BOOL yes = TRUE;
  VARTYPE vt = V_VT(&categories) & VT_TYPEMASK;
  if ((V_VT(&categories) & VT_ARRAY) && V_ARRAY(&categories) && (vt == VT_VARIANT || vt == VT_I4)) {
    LONG lo = 0, hi = -1;
    if (SUCCEEDED(SafeArrayGetLBound(V_ARRAY(&categories), 1, &lo)) &&
        SUCCEEDED(SafeArrayGetUBound(V_ARRAY(&categories), 1, &hi))) {
      yes = FALSE;
      for (LONG i = lo; i <= hi && !yes; i++) {
        VARIANT item;
        VariantInit(&item);
        LONG category = -1;
        if (vt == VT_I4) {
          SafeArrayGetElement(V_ARRAY(&categories), &i, &category);
        } else if (SUCCEEDED(SafeArrayGetElement(V_ARRAY(&categories), &i, &item)) &&
                   SUCCEEDED(VariantChangeType(&item, &item, 0, VT_I4))) {
          category = V_I4(&item);
        }
        VariantClear(&item);
        yes = category == RULE_CATEGORY_FIREWALL;
      }
    }
  }
  VariantClear(&categories);
  return yes;
}

/* Another firewall has taken over from Windows Firewall: its name goes in name (room for
 * FIREWALL_NAME_MAX) and it returns TRUE. Windows Firewall isn't one of the products. */
static BOOL read_product(wchar_t *name) {
  INetFwProducts *products = NULL;
  if (FAILED(CoCreateInstance(&CLSID_NetFwProducts, NULL, CLSCTX_INPROC_SERVER, &IID_INetFwProducts,
                              (void **)&products)))
    return FALSE;
  LONG count = 0;
  BOOL found = FALSE;
  if (SUCCEEDED(INetFwProducts_get_Count(products, &count))) {
    for (LONG i = 0; i < count && !found; i++) {
      INetFwProduct *p = NULL;
      BSTR display = NULL;
      if (FAILED(INetFwProducts_Item(products, i, &p))) continue;
      found = filters_inbound(p);
      if (found && SUCCEEDED(INetFwProduct_get_DisplayName(p, &display)) && display)
        lstrcpynW(name, display, FIREWALL_NAME_MAX);
      SysFreeString(display);
      INetFwProduct_Release(p);
    }
  }
  INetFwProducts_Release(products);
  return found;
}

/* Sets st to the networks of the profiles in mask. nnets < 0: the networks aren't known, so
 * it's the profiles alone. */
static void list_networks(firewall_status *st, long mask, const firewall_net *nets, int nnets) {
  st->nnets = 0;
  if (nnets < 0) {
    for (size_t i = 0; i < NUM_KINDS; i++)
      if (mask & kinds[i]) st->nets[st->nnets++].profile = (long)kinds[i];
    return;
  }
  for (int i = 0; i < nnets; i++)
    if (mask & nets[i].profile) st->nets[st->nnets++] = nets[i];
}

/* What the banner says: for each network this PC is on, does the firewall let other devices
 * in, and if not, could a rule of ours change that? Where it could, that's what's shown
 * first; then what only the user can change. Unlike rule_missing(), a covered network doesn't
 * excuse an uncovered one, and public networks count. */
static void judge(INetFwPolicy2 *policy, long current, const verdict *v, firewall_status *st) {
  firewall_net nets[FIREWALL_MAX_NETS] = {0};
  int nnets = read_networks(nets);
  long active = current;
  if (nnets >= 0) {
    active = 0;
    for (int i = 0; i < nnets; i++) active |= nets[i].profile;
  }
  long fixable = 0, ruled = 0, shut = 0;
  for (size_t i = 0; i < NUM_KINDS; i++) {
    BOOL all_shut;
    if (!(active & kinds[i]) || profile_open(policy, kinds[i], &all_shut)) continue;
    if (all_shut)
      shut |= kinds[i];
    else if (v->block & kinds[i]) /* a block rule beats any allow rule */
      ruled |= kinds[i];
    else if (!(v->allow & kinds[i]))
      fixable |= kinds[i];
  }
  long blocked = fixable | ruled | shut;
  NET_FW_MODIFY_STATE modify = NET_FW_MODIFY_STATE_OK;
  INetFwPolicy2_get_LocalPolicyModifyState(policy, &modify);
  if (!blocked) {
    st->state = FIREWALL_CLEAR;
  } else if (modify == NET_FW_MODIFY_STATE_GP_OVERRIDE) { /* our rule would be ignored */
    st->state = FIREWALL_MANAGED;
    list_networks(st, blocked, nets, nnets);
  } else if (fixable) {
    st->state = FIREWALL_BLOCKED;
    st->public_only = !(fixable & RULE_PROFILES);
    st->moved = v->moved;
    list_networks(st, fixable, nets, nnets);
  } else {
    st->state = ruled ? FIREWALL_RULE : FIREWALL_SHUT;
    list_networks(st, ruled ? ruled : shut, nets, nnets);
  }
}

#ifndef NDEBUG
/* KITHSONG_DEBUG_FIREWALL sets what detection reports, to see each banner without touching
 * this PC's firewall: a state (clear, blocked, rule, shut, managed, thirdparty, unreadable),
 * then any of private, public, domain, moved, fail (Allow "fails") and noreach (show thirdparty
 * and unreadable even though devices have connected), comma-separated.
 * "@path" reads that from a file at every check, to see the banner change while running.
 * While it's set nothing is changed or opened: Allow only pretends (start_helper()), and
 * the buttons that open Windows' settings say what they'd open. */
static BOOL forced_spec(char *spec, size_t n) {
  const char *value = getenv("KITHSONG_DEBUG_FIREWALL");
  if (!value || !*value) return FALSE;
  if (*value != '@') {
    snprintf(spec, n, "%s", value);
    return TRUE;
  }
  spec[0] = '\0';
  FILE *f = fopen(value + 1, "r");
  if (f) {
    if (!fgets(spec, (int)n, f)) spec[0] = '\0';
    fclose(f);
  }
  spec[strcspn(spec, "\r\n")] = '\0';
  return TRUE;
}

static BOOL forced_has(const char *spec, const char *word) {
  size_t n = strlen(word);
  for (const char *p = spec; (p = strstr(p, word)) != NULL; p += n)
    if ((p == spec || p[-1] == ',') && (p[n] == ',' || p[n] == '\0')) return TRUE;
  return FALSE;
}

static BOOL forced_status(firewall_status *st) {
  char spec[128];
  if (!forced_spec(spec, sizeof spec)) return FALSE;
  static const struct {
    const char *word;
    firewall_state state;
  } states[] = {{"blocked", FIREWALL_BLOCKED}, {"rule", FIREWALL_RULE},
                {"shut", FIREWALL_SHUT},       {"managed", FIREWALL_MANAGED},
                {"thirdparty", FIREWALL_THIRD_PARTY}, {"unreadable", FIREWALL_UNREADABLE}};
  static const struct {
    const char *word;
    const wchar_t *name;
    long profile;
  } networks[] = {{"domain", L"corp.example.com", FIREWALL_DOMAIN},
                  {"private", L"Home Wi-Fi", FIREWALL_PRIVATE},
                  {"public", L"Airport Free Wi-Fi", FIREWALL_PUBLIC}};
  st->state = FIREWALL_CLEAR;
  for (size_t i = 0; i < sizeof states / sizeof states[0]; i++)
    if (forced_has(spec, states[i].word)) st->state = states[i].state;
  if (g_fw.forced_allowed) st->state = FIREWALL_CLEAR;
  if (st->state == FIREWALL_CLEAR) return TRUE;
  long covered = 0;
  for (size_t i = 0; i < sizeof networks / sizeof networks[0]; i++) {
    if (!forced_has(spec, networks[i].word)) continue;
    st->nets[st->nnets].profile = networks[i].profile;
    lstrcpynW(st->nets[st->nnets++].name, networks[i].name, FIREWALL_NAME_MAX);
    covered |= networks[i].profile;
  }
  st->public_only = covered == FIREWALL_PUBLIC;
  st->moved = forced_has(spec, "moved");
  if (st->state == FIREWALL_THIRD_PARTY) lstrcpynW(st->product, L"Example Firewall", FIREWALL_NAME_MAX);
  return TRUE;
}
#endif

/* The one reading of the firewall, for the prompt (*missing) and the banner (*st). */
static void read_firewall(unsigned port, BOOL *missing, firewall_status *st) {
#ifndef NDEBUG
  if (forced_status(st)) return; /* *missing stays FALSE: no prompt over the banner */
#endif
  st->state = FIREWALL_UNREADABLE;
  BOOL third_party = read_product(st->product);
  INetFwPolicy2 *policy = NULL;
  HRESULT hr = CoCreateInstance(&CLSID_NetFwPolicy2, NULL, CLSCTX_INPROC_SERVER, &IID_INetFwPolicy2, (void **)&policy);
  long current = 0;
  verdict v = {0};
  if (SUCCEEDED(hr)) hr = INetFwPolicy2_get_CurrentProfileTypes(policy, &current);
  if (SUCCEEDED(hr)) hr = weigh_rules(policy, port, &v);
  if (SUCCEEDED(hr)) {
    *missing = rule_missing(policy, current, &v);
    if (!third_party) judge(policy, current, &v, st);
  }
  if (policy) INetFwPolicy2_Release(policy);
  if (third_party) st->state = FIREWALL_THIRD_PARTY; /* Windows' rules don't decide */
}

static DWORD WINAPI check_main(void *arg) {
  check *c = arg;
  c->status.state = FIREWALL_UNREADABLE;
  if (SUCCEEDED(CoInitializeEx(NULL, COINIT_MULTITHREADED))) {
    read_firewall(c->port, &c->missing, &c->status);
    CoUninitialize();
  }
  if (!PostMessageW(g_fw.wnd, WM_FW_CHECKED, 0, (LPARAM)c)) {
    if (c->ask) g_fw.busy = FALSE;
    free(c);
  }
  return 0;
}

/* Starts a check on a thread; a later one replaces its status. Returns FALSE if it didn't. */
static BOOL start_check(BOOL ask) {
  mc_sync *sync = sync_bridge_get();
  check *c = sync ? calloc(1, sizeof *c) : NULL;
  if (!c) return FALSE;
  c->port = mc_sync_port(sync);
  c->seq = ++g_fw.seq;
  c->ask = ask;
  HANDLE thread = CreateThread(NULL, 0, check_main, c, 0, NULL);
  if (!thread) {
    free(c);
    return FALSE;
  }
  CloseHandle(thread);
  g_fw.running++;
  return TRUE;
}

/* A new status from a check, or from the helper starting or ending. What can't be read says
 * nothing about sync, so once devices have been seen to connect it's left unsaid: the banner
 * then isn't there for good on every PC with another firewall. */
static void set_status(const firewall_status *st) {
  firewall_status next = *st;
  next.adding = g_fw.status.adding;
  if (next.state == FIREWALL_THIRD_PARTY || next.state == FIREWALL_UNREADABLE) {
    mc_sync *sync = sync_bridge_get();
    mc_sync_self_reach reach;
#ifndef NDEBUG
    char spec[128];
    if (forced_spec(spec, sizeof spec) && forced_has(spec, "noreach")) sync = NULL;
#endif
    if (sync && mc_sync_reach_self(sync, &reach) == MC_OK && reach.state == MC_REACH_OK) {
      BOOL adding = next.adding;
      memset(&next, 0, sizeof next);
      next.adding = adding;
    }
  }
  if (!memcmp(&next, &g_fw.status, sizeof next)) return;
  g_fw.status = next;
  if (g_fw.listener) g_fw.listener();
}

static void set_adding(BOOL adding) {
  if (g_fw.status.adding == adding) return;
  g_fw.status.adding = adding;
  if (g_fw.listener) g_fw.listener();
}

/* --- Adding the rule --- */

/* The name add_rule() gives a rule. Run from a checkout it's another name: builds come and go
 * with their folders, and an installed copy would take their rules for its own, moved. */
static const wchar_t *rule_name(void) {
  return mc_source_run() ? L"Kithsong (source)" : mc_wstr(MC_STR_APP_NAME);
}

/* Rule r is one add_rule() added, by its name and group, for an exe that's gone. Only a path
 * on a fixed drive that's there counts: one on a share, or on a drive that's unplugged or
 * removable, may come back. */
static BOOL stale(INetFwRule *r, const wchar_t *name) {
  NET_FW_RULE_DIRECTION dir = 0;
  NET_FW_ACTION action = NET_FW_ACTION_BLOCK;
  BSTR rname = NULL, group = NULL, app = NULL;
  BOOL gone = FALSE;
  if (SUCCEEDED(INetFwRule_get_Direction(r, &dir)) && dir == NET_FW_RULE_DIR_IN &&
      SUCCEEDED(INetFwRule_get_Action(r, &action)) && action == NET_FW_ACTION_ALLOW &&
      SUCCEEDED(INetFwRule_get_Name(r, &rname)) && rname && !wcscmp(rname, name) &&
      SUCCEEDED(INetFwRule_get_Grouping(r, &group)) && group && !wcscmp(group, RULE_GROUP) &&
      SUCCEEDED(INetFwRule_get_ApplicationName(r, &app)) && app && *app && !is_this_exe(app)) {
    wchar_t path[MAX_EXE];
    DWORD n = ExpandEnvironmentStringsW(app, path, MAX_EXE);
    if (n > 3 && n <= MAX_EXE && iswalpha(path[0]) && path[1] == L':' && path[2] == L'\\') {
      wchar_t root[] = {path[0], L':', L'\\', L'\0'};
      if (GetDriveTypeW(root) == DRIVE_FIXED && GetFileAttributesW(root) != INVALID_FILE_ATTRIBUTES &&
          GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        DWORD err = GetLastError();
        gone = err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND;
      }
    }
  }
  SysFreeString(rname);
  SysFreeString(group);
  SysFreeString(app);
  return gone;
}

/* Removes the stale() rules. INetFwRules_Remove() goes by name, which they share with live
 * ones (this exe's, another copy's), so each is first renamed to a name of its own. Failing
 * is left quiet: adding the rule matters more. */
static void remove_stale(INetFwRules *rules, BSTR name) {
  INetFwRule *found[MAX_STALE];
  int n = 0;
  IUnknown *unk = NULL;
  IEnumVARIANT *it = NULL;
  if (SUCCEEDED(INetFwRules_get__NewEnum(rules, &unk)) &&
      SUCCEEDED(IUnknown_QueryInterface(unk, &IID_IEnumVARIANT, (void **)&it))) {
    VARIANT var;
    VariantInit(&var);
    ULONG got = 0;
    while (n < MAX_STALE && IEnumVARIANT_Next(it, 1, &var, &got) == S_OK && got == 1) {
      INetFwRule *r = NULL;
      if (V_VT(&var) == VT_DISPATCH && V_DISPATCH(&var) &&
          SUCCEEDED(IDispatch_QueryInterface(V_DISPATCH(&var), &IID_INetFwRule, (void **)&r))) {
        if (stale(r, name))
          found[n++] = r;
        else
          INetFwRule_Release(r);
      }
      VariantClear(&var);
    }
    IEnumVARIANT_Release(it);
  }
  if (unk) IUnknown_Release(unk);
  for (int i = 0; i < n; i++) {
    wchar_t unique[64] = L"Kithsong stale ";
    size_t len = wcslen(unique);
    GUID guid;
    BSTR temp = NULL;
    if (SUCCEEDED(CoCreateGuid(&guid)) && StringFromGUID2(&guid, unique + len, (int)(64 - len)))
      temp = SysAllocString(unique);
    if (temp && SUCCEEDED(INetFwRule_put_Name(found[i], temp)) && FAILED(INetFwRules_Remove(rules, temp)))
      INetFwRule_put_Name(found[i], name); /* as it was */
    SysFreeString(temp);
    INetFwRule_Release(found[i]);
  }
}

/* The elevated helper's work: a rule for the networks of profiles. */
static HRESULT add_rule(long profiles) {
  INetFwRule *rule = NULL;
  INetFwPolicy2 *policy = NULL;
  INetFwRules *rules = NULL;
  BSTR name = SysAllocString(rule_name());
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
  if (SUCCEEDED(hr)) hr = INetFwRule_put_Profiles(rule, profiles);
  if (SUCCEEDED(hr)) hr = INetFwRule_put_Enabled(rule, VARIANT_TRUE);
  if (SUCCEEDED(hr))
    hr = CoCreateInstance(&CLSID_NetFwPolicy2, NULL, CLSCTX_INPROC_SERVER, &IID_INetFwPolicy2,
                          (void **)&policy);
  if (SUCCEEDED(hr)) hr = INetFwPolicy2_get_Rules(policy, &rules);
  if (SUCCEEDED(hr)) remove_stale(rules, name);
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
  BOOL public_nets = argv && argc == 2 && !wcscmp(argv[1], FIREWALL_HELPER_PUBLIC_ARG);
  BOOL helper = public_nets || (argv && argc == 2 && !wcscmp(argv[1], FIREWALL_HELPER_ARG));
  if (argv) LocalFree(argv);
  if (!helper) return FALSE;
  HRESULT hr = find_exe() ? CoInitializeEx(NULL, COINIT_APARTMENTTHREADED) : E_FAIL;
  if (SUCCEEDED(hr)) {
    hr = add_rule(public_nets ? NET_FW_PROFILE2_PUBLIC : RULE_PROFILES);
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
  message_dialog(g_fw.owner, mc_wstr(MC_STR_APP_NAME), text, MB_ICONERROR);
}

/* Relaunches this exe elevated to add the rule, for public networks or for the private and
 * domain ones. The user saying no to UAC isn't an error. */
static BOOL start_helper(BOOL public_nets) {
  g_fw.adding_public = public_nets;
#ifndef NDEBUG
  char spec[128];
  if (forced_spec(spec, sizeof spec)) { /* no UAC, no rule: straight to the helper's answer */
    BOOL fail = forced_has(spec, "fail");
    g_fw.forced_allowed = !fail && getenv("KITHSONG_DEBUG_FIREWALL")[0] != '@'; /* a file says for itself */
    return PostMessageW(g_fw.wnd, WM_FW_ADDED, fail ? (WPARAM)E_ACCESSDENIED : S_OK, 0);
  }
#endif
  SHELLEXECUTEINFOW sei = {
    .cbSize = sizeof sei,
    .fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC,
    .hwnd = g_fw.owner,
    .lpVerb = L"runas",
    .lpFile = g_fw.exe,
    .lpParameters = public_nets ? FIREWALL_HELPER_PUBLIC_ARG : FIREWALL_HELPER_ARG,
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

static void helper_done(HRESULT hr) {
  /* The status from before the rule, which the check below replaces under the dialog. */
  BOOL still_public = g_fw.status.state == FIREWALL_BLOCKED && g_fw.status.public_only;
  set_adding(FALSE);
  if (g_fw.watching || SUCCEEDED(hr)) start_check(FALSE);
  if (FAILED(hr)) {
    show_failure(hr);
    return;
  }
  enum mc_str_id text = g_fw.adding_public ? MC_STR_WINDOWS_FIREWALL_ADDED_ON_PUBLIC
                        : still_public     ? MC_STR_WINDOWS_FIREWALL_ADDED_PUBLIC
                                           : MC_STR_WINDOWS_FIREWALL_ADDED;
  message_dialog(g_fw.owner, mc_wstr(MC_STR_APP_NAME), mc_wstr(text), MB_ICONINFORMATION);
  /* Devices that found this PC blocked find out sooner. */
  mc_sync *sync = sync_bridge_get();
  if (sync) mc_sync_check_reach(sync, NULL);
}

/* --- Asking --- */

/* Returns TRUE if the helper started, and reports later. */
static BOOL ask(void) {
  prompt_dialog prompt = {
    .caption = mc_wstr(MC_STR_APP_NAME),
    .icon = MB_ICONINFORMATION,
    .heading = mc_wstr(MC_STR_WINDOWS_FIREWALL_TITLE),
    .text = mc_wstr(MC_STR_WINDOWS_FIREWALL_TEXT),
    .ok = mc_wstr_mnemonic(MC_STR_WINDOWS_FIREWALL_ALLOW),
    .cancel = mc_wstr_mnemonic(MC_STR_WINDOWS_FIREWALL_NOT_NOW),
    .shield = TRUE,
    .check = mc_wstr_mnemonic(MC_STR_WINDOWS_FIREWALL_DONT_ASK),
  };
  INT_PTR button = prompt_dialog_show(g_fw.owner, &prompt);
  if (button <= 0) return FALSE;
  if (prompt.checked) {
    mc_settings_set_bool(g_fw.settings, SETTING_DONT_ASK, 1);
    mc_settings_save(g_fw.settings); /* failing, it's asked again next time */
  }
  if (button != IDOK) return FALSE;
  set_adding(TRUE);
  if (start_helper(FALSE)) return TRUE;
  set_adding(FALSE);
  return FALSE;
}

/* Nothing to say: sync is off, so nothing is listening. */
static void clear_status(void) {
  firewall_status none = {0};
  set_status(&none);
}

static LRESULT CALLBACK fw_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_TIMER:
      if (wp == TIMER_WATCH) {
        if (!sync_bridge_get())
          clear_status();
        else if (!g_fw.running)
          start_check(FALSE);
        return 0;
      }
      KillTimer(wnd, TIMER_CHECK);
      if (!start_check(TRUE)) g_fw.busy = FALSE;
      return 0;
    case WM_FW_CHECKED: {
      check *c = (check *)lp;
      BOOL for_prompt = c->ask, missing = c->missing;
      g_fw.running--;
      if (c->seq == g_fw.seq) set_status(&c->status);
      free(c);
      if (!for_prompt) return 0;
      BOOL asked = g_fw.asked;
      g_fw.asked = FALSE;
      /* Not while the window is away (Move Library), or once sync has stopped. */
      if (!missing || !IsWindowVisible(g_fw.owner) || !sync_bridge_get() || !ask()) g_fw.busy = FALSE;
      if (!missing && asked)
        message_dialog(g_fw.owner, mc_wstr(MC_STR_APP_NAME), mc_wstr(MC_STR_WINDOWS_FIREWALL_NOTHING_TO_ALLOW),
                       MB_ICONINFORMATION);
      return 0;
    }
    case WM_FW_ADDED:
      g_fw.busy = FALSE;
      helper_done((HRESULT)wp);
      return 0;
    default:
      return DefWindowProcW(wnd, msg, wp, lp);
  }
}

/* The window the threads report to, made the first time; and what they need. */
static BOOL ready(void) {
  if (packaged() || !find_exe()) return FALSE;
  if (!g_fw.wnd) {
    WNDCLASSW wc = {.lpfnWndProc = fw_proc, .hInstance = GetModuleHandleW(NULL), .lpszClassName = L"kithsong_firewall"};
    RegisterClassW(&wc);
    g_fw.wnd = CreateWindowExW(0, wc.lpszClassName, NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);
    g_fw.rule_name = rule_name();
  }
  return g_fw.wnd != NULL;
}

/* Checks after delay_ms, unless sync is off, the user said not to ask, or a check, the
 * dialog or the helper is under way already. */
static void check_after(HWND owner, mc_settings *settings, UINT delay_ms, BOOL asked) {
  if (g_fw.busy || !sync_bridge_get() || mc_settings_bool(settings, SETTING_DONT_ASK, 0) || !ready()) return;
  g_fw.owner = owner;
  g_fw.settings = settings;
  g_fw.busy = TRUE;
  g_fw.asked = asked;
  SetTimer(g_fw.wnd, TIMER_CHECK, delay_ms, NULL);
}

void firewall_check_later(HWND owner, mc_settings *settings) {
  check_after(owner, settings, CHECK_DELAY_MS, FALSE);
}

BOOL firewall_can_ask_again(const mc_settings *settings) {
  return settings && mc_settings_bool(settings, SETTING_DONT_ASK, 0) && !packaged();
}

void firewall_ask_again(HWND owner, mc_settings *settings) {
  mc_settings_set_bool(settings, SETTING_DONT_ASK, 0);
  mc_settings_save(settings); /* failing, it's saved with the next setting */
  check_after(owner, settings, 0, TRUE);
}

/* --- The banner's side --- */

void firewall_set_listener(void (*changed)(void)) {
  g_fw.listener = changed;
}

const firewall_status *firewall_get_status(void) {
  return &g_fw.status;
}

void firewall_watch(HWND owner, BOOL on) {
  if (on == g_fw.watching) return;
  if (!on) {
    g_fw.watching = FALSE;
    if (g_fw.wnd) KillTimer(g_fw.wnd, TIMER_WATCH);
    return;
  }
  if (!ready()) return;
  if (!g_fw.owner) g_fw.owner = owner;
  g_fw.watching = TRUE;
  SetTimer(g_fw.wnd, TIMER_WATCH, WATCH_MS, NULL);
  firewall_recheck();
}

void firewall_recheck(void) {
  if (!g_fw.watching) return;
  if (!start_check(FALSE) && !sync_bridge_get()) clear_status();
}

void firewall_allow(void) {
  if (g_fw.busy || g_fw.status.state != FIREWALL_BLOCKED) return;
  g_fw.busy = TRUE;
  set_adding(TRUE);
  if (start_helper(g_fw.status.public_only)) return;
  g_fw.busy = FALSE;
  set_adding(FALSE);
}

void firewall_open(firewall_page page) {
  static const struct {
    const wchar_t *file, *args;
  } pages[] = {
    [FIREWALL_PAGE_NETWORK] = {L"ms-settings:network", NULL},
    [FIREWALL_PAGE_SECURITY] = {L"windowsdefender://network/", NULL},
    [FIREWALL_PAGE_ALLOWED_APPS] = {L"control.exe", L"/name Microsoft.WindowsFirewall /page pageConfigureApps"},
  };
#ifndef NDEBUG
  char spec[128];
  if (forced_spec(spec, sizeof spec)) {
    message_dialog(g_fw.owner, L"KITHSONG_DEBUG_FIREWALL", pages[page].file, MB_ICONINFORMATION);
    return;
  }
#endif
  ShellExecuteW(g_fw.owner, L"open", pages[page].file, pages[page].args, NULL, SW_SHOWNORMAL);
}

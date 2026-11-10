/* The Windows Firewall rule sync needs. Other devices connect to this PC's sync listener
 * (TCP MC_SYNC_DEFAULT_PORT, or an ephemeral port when that's taken) and send it mDNS
 * (UDP 5353), and with the firewall on, Windows drops both unless a rule allows them. So
 * after startup, while sync runs, a thread reads the firewall's rules through the
 * INetFwPolicy2 COM API (which needs no admin rights) and, if none lets kithsong.exe in on
 * private networks, asks whether to add one.
 *
 * The rule allows this exe, any protocol and port, inbound on the Private and Domain
 * profiles: scoping it by program covers the listener's fallback port and mDNS alike, and
 * follows the exe rather than opening ports to every program. It covers Google Cast too: the
 * device fetches the music from a server the core runs on a random port while casting
 * (docs/cast.md), and finds nothing to fetch without the rule. The prompt leaves public
 * networks alone; on one, its rule only starts to help once the network is set to private.
 *
 * While a device's page shows, the same reading of the firewall feeds the banner at the top
 * of that page (firewall_banner.c), which says what's blocked on which networks and offers
 * the fix, whether or not the prompt would ask, and whatever "Don't ask again" says. Only
 * when the networks left blocked are all public does it offer a second rule, for the Public
 * profile alone (FIREWALL_HELPER_PUBLIC_ARG): that opens Kithsong on every public network
 * this PC joins, so it's never added along with the first, and always the user's explicit
 * choice, beside setting the network to private.
 *
 * Adding a rule needs admin rights, so Allow relaunches this exe elevated (UAC) with
 * FIREWALL_HELPER_ARG, which adds the rule and exits with its HRESULT; firewall_helper_main()
 * is that process. Before adding it, the helper removes rules it added before for an exe
 * that's gone (the portable zip moved): with admin rights only then, that's when one's left.
 * "Don't ask again" is saved in settings.json as "firewall_dont_ask"; Settings offers to ask
 * again (firewall_ask_again()).
 *
 * The MSIX package (packaging/msix) declares the same rule in its manifest instead, so when
 * running from it none of this happens. */
#ifndef KITHSONG_FIREWALL_H
#define KITHSONG_FIREWALL_H

#include <windows.h>

#include "kithcore/settings.h"

#define FIREWALL_HELPER_ARG L"--add-firewall-rule"
#define FIREWALL_HELPER_PUBLIC_ARG L"--add-firewall-rule-public"

enum { FIREWALL_MAX_NETS = 8, FIREWALL_NAME_MAX = 64 };

/* A firewall profile, with NET_FW_PROFILE2's values. */
enum { FIREWALL_DOMAIN = 1, FIREWALL_PRIVATE = 2, FIREWALL_PUBLIC = 4 };

/* What the firewall means for other devices connecting to this PC on the networks it's on
 * now. */
typedef enum firewall_state {
  FIREWALL_CLEAR,       /* nothing to say: not blocked, or not running, or sync is off */
  FIREWALL_BLOCKED,     /* no rule lets Kithsong in on nets, and firewall_allow() can add it */
  FIREWALL_RULE,        /* a rule blocks Kithsong on nets, which one of ours wouldn't undo */
  FIREWALL_SHUT,        /* the profile of nets blocks all incoming connections, even allowed apps */
  FIREWALL_MANAGED,     /* blocked on nets, and group policy overrides rules added here */
  FIREWALL_THIRD_PARTY, /* product has taken over from Windows Firewall: can't tell, can't add */
  FIREWALL_UNREADABLE,  /* Windows Firewall won't answer: can't tell, can't add */
} firewall_state;

typedef struct firewall_net {
  wchar_t name[FIREWALL_NAME_MAX]; /* "" if only the kind of network is known */
  long profile;                    /* one FIREWALL_DOMAIN, _PRIVATE or _PUBLIC */
} firewall_net;

typedef struct firewall_status {
  firewall_state state;
  firewall_net nets[FIREWALL_MAX_NETS]; /* the networks it's about */
  int nnets;
  BOOL public_only; /* BLOCKED: all of them public, so Allow is for public networks */
  BOOL moved;       /* BLOCKED: a rule of ours names where this exe used to be */
  BOOL adding;      /* the helper is running */
  wchar_t product[FIREWALL_NAME_MAX];
} firewall_status;

/* The parts of Windows' settings that can fix what the banner says. */
typedef enum firewall_page {
  FIREWALL_PAGE_NETWORK,      /* where a network is set to private */
  FIREWALL_PAGE_SECURITY,     /* Windows Security's firewall page */
  FIREWALL_PAGE_ALLOWED_APPS, /* the list of apps the firewall allows */
} firewall_page;

/* Call at the top of WinMain, after wstr_init(). If the command line is the elevated
 * helper's (FIREWALL_HELPER_ARG or FIREWALL_HELPER_PUBLIC_ARG alone), adds the rule, sets
 * *exit_code (0, or the failing HRESULT) and returns TRUE: the process should exit. Otherwise
 * returns FALSE and the app starts as usual. */
BOOL firewall_helper_main(int *exit_code);

/* Once the main window is up and sync started: checks the firewall on a thread and, if
 * sync needs a rule, asks owner's user whether to add one. Does nothing if sync is off, the
 * user said not to ask again, the app runs from its MSIX package, the firewall is off or a
 * rule already lets Kithsong in. settings must last as long as owner. UI thread only. */
void firewall_check_later(HWND owner, mc_settings *settings);

/* The user said not to ask again, and asking could still help (not running from the MSIX
 * package): Settings offers firewall_ask_again(). */
BOOL firewall_can_ask_again(const mc_settings *settings);

/* Settings' button: forgets "Don't ask again" and, while sync runs, checks now, asking as at
 * startup if sync needs the rule, else saying there's nothing to allow. UI thread only. */
void firewall_ask_again(HWND owner, mc_settings *settings);

/* The rest is the banner's, and for the UI thread only. Nothing here reads the firewall on
 * it: checks run on a thread, and changed is called, on the UI thread, when one finds
 * something new. */
void firewall_set_listener(void (*changed)(void));
/* The latest status. Valid until the next message is handled. */
const firewall_status *firewall_get_status(void);
/* Keeps the status up to date while on: checks now, then every so often. Does nothing when
 * running from the MSIX package, whose status stays FIREWALL_CLEAR. */
void firewall_watch(HWND owner, BOOL on);
/* Checks now if firewall_watch() is on, as when the user comes back from Windows' settings. */
void firewall_recheck(void);
/* FIREWALL_BLOCKED's Allow: starts the helper, with no prompt first, as pressing it is the
 * answer. For public networks if status.public_only. status.adding is set meanwhile; a check
 * follows, so the banner goes by itself. */
void firewall_allow(void);
void firewall_open(firewall_page page);
/* --demo-library: the firewall reads as letting everything through from now on, so the
 * screenshots show no banner whatever network the machine taking them is on. */
void firewall_demo_clear(void);

#endif

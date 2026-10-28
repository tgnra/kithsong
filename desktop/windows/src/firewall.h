/* The Windows Firewall rule sync needs. Other devices connect to this PC's sync listener
 * (TCP MC_SYNC_DEFAULT_PORT, or an ephemeral port when that's taken) and send it mDNS
 * (UDP 5353), and with the firewall on, Windows drops both unless a rule allows them. So
 * after startup, while sync runs, a thread reads the firewall's rules through the
 * INetFwPolicy2 COM API (which needs no admin rights) and, if none lets musicapp.exe in on
 * private networks, asks whether to add one.
 *
 * The rule allows this exe, any protocol and port, inbound on the Private and Domain
 * profiles: scoping it by program covers the listener's fallback port and mDNS alike, and
 * follows the exe rather than opening ports to every program. Public networks are left
 * alone; on one, the rule only starts to help once the network is set to private.
 *
 * Adding a rule needs admin rights, so Allow relaunches this exe elevated (UAC) with
 * FIREWALL_HELPER_ARG, which adds the rule and exits with its HRESULT; firewall_helper_main()
 * is that process. "Don't ask again" is saved in settings.json as "firewall_dont_ask". */
#ifndef MUSICAPP_FIREWALL_H
#define MUSICAPP_FIREWALL_H

#include <windows.h>

#include "musiccore/settings.h"

#define FIREWALL_HELPER_ARG L"--add-firewall-rule"

/* Call at the top of WinMain, after wstr_init(). If the command line is the elevated
 * helper's (FIREWALL_HELPER_ARG alone), adds the rule, sets *exit_code (0, or the failing
 * HRESULT) and returns TRUE: the process should exit. Otherwise returns FALSE and the app
 * starts as usual. */
BOOL firewall_helper_main(int *exit_code);

/* Once the main window is up and sync started: checks the firewall on a thread and, if
 * sync needs a rule, asks owner's user whether to add one. Does nothing if sync is off, the
 * user said not to ask again, the firewall is off or a rule already lets musicapp in.
 * settings must last as long as owner. UI thread only. */
void firewall_check_later(HWND owner, mc_settings *settings);

#endif

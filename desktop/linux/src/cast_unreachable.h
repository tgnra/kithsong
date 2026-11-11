/* The dialog shown when a cast ends with MC_CAST_END_UNREACHABLE: the device said a track
 * failed without ever connecting back to fetch it, so something between them, almost always
 * this computer's firewall, blocks the music port. Linux has no per-app firewall rule to ask
 * for, as Windows does (firewall.c there), and inside the Flatpak there's no talking to
 * firewalld, so it says what's wrong and what to allow, with commands for the two common
 * firewalls, each with a button that copies it:
 *
 *   +--------------------------------------------------------+
 *   |         Living Room TV can't reach this computer         |
 *   | Casting plays the music from this computer, so Living   |
 *   | Room TV has to connect to it, and it couldn't. A        |
 *   | firewall is probably blocking it: allow incoming TCP    |
 *   | connections on port 47824 from your local network,      |
 *   | then cast again.                                        |
 *   |                                                         |
 *   | If you use firewalld or ufw, its command here allows    |
 *   | it. Run it in a terminal:                               |
 *   |  +---------------------------------------------------+  |
 *   |  | firewalld                                          |  |
 *   |  | sudo firewall-cmd --permanent --zone="$(firewall-  |  |
 *   |  | cmd --get-zone-of-interface=wlan0)" --add-     [⎘] |  |
 *   |  | port=47824/tcp && sudo firewall-cmd --reload       |  |
 *   |  |---------------------------------------------------|  |
 *   |  | ufw                                                |  |
 *   |  | sudo ufw allow from 192.168.0.0/24 to any      [⎘] |  |
 *   |  | port 47824 proto tcp                               |  |
 *   |  +---------------------------------------------------+  |
 *   |                       [ Close ]                         |
 *   +--------------------------------------------------------+
 *
 * The firewalld command puts the port in the zone of the interface the device is on, which
 * may not be the default zone; ufw's allows only that interface's network. Both from the
 * address the music server listened on (mc_cast_status.music_address).
 *
 * Needs libadwaita 1.5 (AdwAlertDialog). */
#ifndef KITHSONG_CAST_UNREACHABLE_H
#define KITHSONG_CAST_UNREACHABLE_H

#include <adwaita.h>

/* name: the device's name ("" for MC_STR_CAST_DEVICE_UNNAMED); address and port: where the
 * music server listened, from the cast's status. */
void cast_unreachable_show(GtkWidget *parent, const char *name, const char *address, unsigned port);

#endif

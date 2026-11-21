/* The Add Device dialog, opened by the + next to the sidebar's Devices heading. For networks
 * where discovery can't see a device (a firewall, or a router that blocks multicast), it takes
 * the device's address directly; pairing then continues as for a discovered device.
 *
 * Needs libadwaita 1.5 (AdwAlertDialog).
 */
#ifndef KITHSONG_ADD_DEVICE_H
#define KITHSONG_ADD_DEVICE_H

#include <adwaita.h>

#define ADD_DEVICE_DEFAULT_PORT 47823

/* Called on the main thread after the dialog closes with Connect. host is trimmed and
 * non-empty: an IPv4 or IPv6 literal, or a hostname. */
typedef void (*add_device_connect_fn)(void *user, const char *host, guint16 port);

/* Shows the dialog over parent (any widget in the main window). */
void add_device_present(GtkWidget *parent, add_device_connect_fn connect, void *user);

#endif

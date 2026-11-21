/* The Add Device dialog, opened by the + beside the sidebar's DEVICES heading. For networks
 * where discovery can't see a device (a firewall, or a router that blocks multicast), it takes
 * the device's address directly; pairing then continues as for a discovered device. The Linux
 * counterpart is desktop/linux/src/add_device.c.
 *
 * The dialog's template is built in memory, so it needs no resources.
 */
#ifndef KITHSONG_ADD_DEVICE_H
#define KITHSONG_ADD_DEVICE_H

#include <stdint.h>
#include <windows.h>

#define ADD_DEVICE_DEFAULT_PORT 47823

/* Called after the dialog closes with Connect. host (UTF-8) is trimmed and non-empty: an
 * IPv4 or IPv6 literal, or a hostname. */
typedef void (*add_device_connect_fn)(void *user, const char *host, uint16_t port);

/* Shows the dialog, modal over owner, and returns once it's closed. */
void add_device_show(HWND owner, add_device_connect_fn connect, void *user);

#endif

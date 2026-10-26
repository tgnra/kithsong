/* The Add Device dialog, opened by the + beside the sidebar's DEVICES heading. For networks
 * where discovery can't see a device (a firewall, or a router that blocks multicast), it
 * takes the device's address directly:
 *
 *   +- Add Device ---------------------------- X -+
 *   | Enter the address of a device on your        |
 *   | network. It will show a code to enter next.  |
 *   |                                              |
 *   | IP address or hostname:                      |
 *   | [                                          ] |
 *   | Port:                                        |
 *   | [47823 |^v]                                  |
 *   |                          [Connect] [Cancel]  |
 *   +----------------------------------------------+
 *
 * Connect is enabled once there's an address (without spaces inside) and a port from 1 to
 * 65535; Enter connects. Pairing then continues as for a discovered device: the other device
 * shows a 6-digit code, and the user types it here. The Linux counterpart is
 * desktop/linux/src/add_device.c.
 *
 * The dialog's template is built in memory, so it needs no resources.
 */
#ifndef MUSICAPP_ADD_DEVICE_H
#define MUSICAPP_ADD_DEVICE_H

#include <stdint.h>
#include <windows.h>

#define ADD_DEVICE_DEFAULT_PORT 47823

/* Called after the dialog closes with Connect. host (UTF-8) is trimmed and non-empty: an
 * IPv4 or IPv6 literal, or a hostname. */
typedef void (*add_device_connect_fn)(void *user, const char *host, uint16_t port);

/* Shows the dialog, modal over owner, and returns once it's closed. */
void add_device_show(HWND owner, add_device_connect_fn connect, void *user);

#endif

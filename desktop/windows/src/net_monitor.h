/* Tells of network changes (an interface added, removed or changed: NotifyIpInterfaceChange),
 * for mc_sync_network_changed(). fn runs on a system thread; it should only post. */
#ifndef KITHSONG_NET_MONITOR_H
#define KITHSONG_NET_MONITOR_H

#include <windows.h>

typedef void (*net_monitor_fn)(void *user);

/* Starts watching, stopping any watch first. FALSE if it couldn't. */
BOOL net_monitor_start(net_monitor_fn fn, void *user);
/* Stops, waiting for a callback that's running to return. Safe when not started. */
void net_monitor_stop(void);

#endif

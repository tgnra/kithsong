/* LAN discovery of other musicapp devices via embedded mDNS/DNS-SD (service type
 * "_musicapp._tcp"), with no OS mDNS daemon. See docs/sync-protocol.md, "Discovery".
 *
 * Threading: mc_discovery_start() starts a discovery thread that answers queries, browses
 * every scan interval and calls the callback. The callback always runs on that thread, so
 * frontends marshal it onto their UI thread. mc_discovery_set_group() may be called from
 * any thread, the callback included, but not concurrently with mc_discovery_stop(). Stop
 * joins the thread, so it must not be called from the callback.
 *
 * Android and Wear OS: the frontend must hold a WifiManager.MulticastLock while discovery
 * runs, or the OS drops incoming multicast. A watch is discoverable only on Wi-Fi. */
#ifndef MUSICCORE_DISCOVERY_H
#define MUSICCORE_DISCOVERY_H

#include "musiccore/musiccore.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Protocol version announced as "v" in the TXT record. Peers announcing another are
 * ignored. */
#define MC_DISCOVERY_VERSION 1
/* Scan interval used when 0 is passed to mc_discovery_start(). */
#define MC_DISCOVERY_DEFAULT_INTERVAL_S 30
/* Length of a group hash (16 hex digits) including the terminating NUL. */
#define MC_GROUP_HASH_STRLEN 17

typedef struct mc_discovery mc_discovery;

typedef struct mc_peer_info {
  const char *device_uid; /* TXT "dev" */
  const char *name;
  const char *kind;       /* "desktop", "phone" or "watch" */
  const char *group_hash; /* TXT "grp": 16 hex digits, "" when not in a group */
  int version;            /* TXT "v" */
  const char *address;    /* numeric IPv4, or IPv6 if the peer has no IPv4 address */
  uint16_t port;          /* the peer's TCP port, from its SRV record */
} mc_peer_info;

/* online = 1: the peer appeared, or its address, port, name, kind or group changed.
 * online = 0: it hasn't been heard for two scan intervals, or it said goodbye. Runs on
 * the discovery thread; the strings are valid only during the call. Stopping discovery
 * reports nothing. */
typedef void (*mc_peer_cb)(const mc_peer_info *peer, int online, void *user);

/* Announces this device and browses for others until mc_discovery_stop(). kind is
 * "desktop", "phone" or "watch". group_hash is from mc_discovery_group_hash(), or NULL or
 * "" when not in a group. port is the TCP port this device listens on. scan_interval_s is
 * how often to browse (0 for the default); a peer not heard for two intervals is offline.
 * Fails with MC_ERR_NET if no mDNS socket can be opened (no network, or multicast is
 * blocked). */
mc_status mc_discovery_start(const char *device_uid, const char *device_name, const char *kind,
                             const char *group_hash, uint16_t port, int scan_interval_s,
                             mc_peer_cb cb, void *user, mc_discovery **out);
/* Changes the announced group, e.g. after pairing or leaving a group, and re-announces at
 * once. group_hash as for mc_discovery_start(). */
mc_status mc_discovery_set_group(mc_discovery *disc, const char *group_hash);
/* Says goodbye to the network, stops the thread and frees disc. NULL is a no-op. */
void mc_discovery_stop(mc_discovery *disc);

/* The TXT "grp" value for a group: the first 8 bytes of SHA-256(group_uid) as lowercase
 * hex, so group members recognise each other without broadcasting the group id. */
mc_status mc_discovery_group_hash(const char *group_uid, char out[MC_GROUP_HASH_STRLEN]);

#ifdef __cplusplus
}
#endif

#endif

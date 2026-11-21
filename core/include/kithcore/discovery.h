/* LAN discovery of other Kithsong devices via embedded mDNS/DNS-SD (service type
 * "_kithsong._tcp"), with no OS mDNS daemon. See docs/sync-protocol.md, "Discovery".
 *
 * Threading: mc_discovery_start() starts a thread that answers queries, browses, announces
 * and runs the callback. mc_discovery_set_group() may be called from any thread, the
 * callback included, but not concurrently with mc_discovery_stop(). Stop joins the thread,
 * so it must not be called from the callback.
 *
 * Android: the frontend must hold a WifiManager.MulticastLock while discovery runs, or the
 * OS drops incoming multicast. */
#ifndef KITHCORE_DISCOVERY_H
#define KITHCORE_DISCOVERY_H

#include "kithcore/kithcore.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The sync protocol version this build speaks, announced as TXT "v" (and sent in HELLO
 * and PAIR_HELLO), and the oldest one it still syncs with, TXT "vmin". Peers of any version
 * are reported, so the frontends can say which can't sync. See "Versions" in
 * docs/sync-protocol.md before changing either. */
#define MC_DISCOVERY_VERSION 2
#define MC_DISCOVERY_MIN_VERSION 2
/* Scan interval used when 0 is passed to mc_discovery_start(). */
#define MC_DISCOVERY_DEFAULT_INTERVAL_S 30
/* 16 hex digits and a NUL. */
#define MC_GROUP_HASH_STRLEN 17

typedef struct mc_discovery mc_discovery;

typedef struct mc_peer_info {
  const char *device_uid; /* TXT "dev" */
  const char *name;
  const char *kind;       /* "desktop" or "phone"; announcements of any other kind are ignored */
  const char *host;       /* TXT "host": its host name, "" if it announced none */
  const char *group_hash; /* TXT "grp": 16 hex digits, "" when not in a group */
  int version;            /* TXT "v"; -1 if missing or malformed */
  int min_version;        /* TXT "vmin"; -1 if missing (versions before 2 don't send it) */
  const char *app;        /* TXT "app": its Kithsong version, "" if it announced none */
  const char *address;    /* numeric IPv4, or IPv6 if the peer has no IPv4 address */
  uint16_t port;          /* the peer's TCP port, from its SRV record */
} mc_peer_info;

/* online = 1: the peer appeared, or its address, port, name, kind or group changed.
 * online = 0: not heard for two scan intervals, or it said goodbye. Strings are valid only
 * during the call. Stopping discovery reports nothing. */
typedef void (*mc_peer_cb)(const mc_peer_info *peer, int online, void *user);

/* Announces this device and browses for others until mc_discovery_stop(). kind is
 * "desktop" or "phone". group_hash is from mc_discovery_group_hash(), NULL or "" outside a
 * group. port is the TCP port sync listens on. scan_interval_s 0 means the default. Works
 * without a network, opening sockets once there is one, and reopens sockets that go deaf
 * (e.g. after a sleep). MC_ERR_NET only if the socket API can't be initialised. */
mc_status mc_discovery_start(const char *device_uid, const char *device_name, const char *kind,
                             const char *group_hash, uint16_t port, int scan_interval_s,
                             mc_peer_cb cb, void *user, mc_discovery **out);
/* Re-announces at once. group_hash as for mc_discovery_start(). */
mc_status mc_discovery_set_group(mc_discovery *disc, const char *group_hash);
/* TXT "name"; re-announces at once. The service instance label stays as started. Same
 * threading as mc_discovery_set_group(). */
mc_status mc_discovery_set_name(mc_discovery *disc, const char *device_name);
/* Says goodbye, stops the thread and frees disc. NULL is a no-op. */
void mc_discovery_stop(mc_discovery *disc);
/* The network changed: reopens every socket, scans and announces at once. Any thread;
 * returns at once. */
void mc_discovery_network_changed(mc_discovery *disc);

/* The TXT "grp" value for a group: the first 8 bytes of SHA-256(group_uid) as lowercase
 * hex, so group members recognise each other without broadcasting the group id. */
mc_status mc_discovery_group_hash(const char *group_uid, char out[MC_GROUP_HASH_STRLEN]);

#ifdef __cplusplus
}
#endif

#endif

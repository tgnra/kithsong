/* LAN discovery of other musicapp instances via embedded mDNS/DNS-SD
 * (service type "_musicapp._tcp"). Runs on its own thread. */
#ifndef MUSICCORE_DISCOVERY_H
#define MUSICCORE_DISCOVERY_H

#include "musiccore/musiccore.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mc_discovery mc_discovery;

typedef struct mc_peer_info {
  const char *node_uid;
  const char *name;
  const char *address;
  uint16_t port;
} mc_peer_info;

/* Called on the discovery thread whenever a peer appears or disappears. */
typedef void (*mc_peer_cb)(const mc_peer_info *peer, int online, void *user);

mc_status mc_discovery_start(const char *node_uid, const char *device_name, uint16_t port,
                             int scan_interval_s, mc_peer_cb cb, void *user, mc_discovery **out);
void mc_discovery_stop(mc_discovery *disc);

#ifdef __cplusplus
}
#endif

#endif

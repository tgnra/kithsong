/* mDNS/DNS-SD announce + browse on a background thread, using mjansson/mdns (vendored
 * into third_party/mdns when this lands). Android additionally needs the frontend to hold
 * a WifiManager.MulticastLock while discovery runs. */
#include "musiccore/discovery.h"

mc_status mc_discovery_start(const char *node_uid, const char *device_name, uint16_t port,
                             int scan_interval_s, mc_peer_cb cb, void *user, mc_discovery **out) {
  (void)node_uid;
  (void)device_name;
  (void)port;
  (void)scan_interval_s;
  (void)cb;
  (void)user;
  if (out) *out = NULL;
  return MC_ERR_UNIMPLEMENTED;
}

void mc_discovery_stop(mc_discovery *disc) {
  (void)disc;
}

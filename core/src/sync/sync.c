/* Sync sessions over an authenticated channel (Noise handshake from the pairing keys).
 * Protocol: docs/sync-protocol.md. */
#include "musiccore/sync.h"

mc_status mc_sync_new(mc_library *lib, mc_sync **out) {
  (void)lib;
  if (out) *out = NULL;
  return MC_ERR_UNIMPLEMENTED;
}

void mc_sync_free(mc_sync *sync) {
  (void)sync;
}

mc_status mc_sync_pair_begin(mc_sync *sync, const char *peer_node_uid, char out_code[8]) {
  (void)sync;
  (void)peer_node_uid;
  (void)out_code;
  return MC_ERR_UNIMPLEMENTED;
}

mc_status mc_sync_pair_confirm(mc_sync *sync, const char *peer_node_uid, const char *code) {
  (void)sync;
  (void)peer_node_uid;
  (void)code;
  return MC_ERR_UNIMPLEMENTED;
}

mc_status mc_sync_unpair(mc_sync *sync, const char *peer_node_uid) {
  (void)sync;
  (void)peer_node_uid;
  return MC_ERR_UNIMPLEMENTED;
}

mc_status mc_sync_set_preferred_format(mc_sync *sync, const mc_transcode_opts *opts) {
  (void)sync;
  (void)opts;
  return MC_ERR_UNIMPLEMENTED;
}

mc_status mc_sync_with_peer(mc_sync *sync, const char *peer_node_uid) {
  (void)sync;
  (void)peer_node_uid;
  return MC_ERR_UNIMPLEMENTED;
}

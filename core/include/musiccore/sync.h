/* Pairing and bidirectional sync with peers. There is no server: every device keeps a
 * change log stamped with hybrid logical clocks, and peers exchange the entries the other
 * hasn't seen. See docs/sync-protocol.md. */
#ifndef MUSICCORE_SYNC_H
#define MUSICCORE_SYNC_H

#include "musiccore/musiccore.h"
#include "musiccore/transcode.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mc_library mc_library;
typedef struct mc_sync mc_sync;
typedef struct mc_transcode_opts mc_transcode_opts;

mc_status mc_sync_new(mc_library *lib, mc_sync **out);
void mc_sync_free(mc_sync *sync);

/* Pairing: one side shows a short code, the other enters it. */
mc_status mc_sync_pair_begin(mc_sync *sync, const char *peer_node_uid, char out_code[8]);
mc_status mc_sync_pair_confirm(mc_sync *sync, const char *peer_node_uid, const char *code);
mc_status mc_sync_unpair(mc_sync *sync, const char *peer_node_uid);

/* Format this device wants audio delivered in (e.g. a phone asking for Opus). */
mc_status mc_sync_set_preferred_format(mc_sync *sync, const mc_transcode_opts *opts);

mc_status mc_sync_with_peer(mc_sync *sync, const char *peer_node_uid);

#ifdef __cplusplus
}
#endif

#endif

/* One sync session over an established channel: HELLO, then the group changelog, then the
 * library's if both devices hold the group's library, then BYE. See "Session" in
 * docs/sync-protocol.md. Internal to core. */
#ifndef MC_SYNC_SESSION_H
#define MC_SYNC_SESSION_H

#include "sync/sync_internal.h"

typedef struct mc_session {
  mc_sync *sync;
  mc_sync_conn *conn;
  mc_channel *ch;
  char peer_uid[MC_UID_STRLEN]; /* known before run: the KK peer, or the pairing partner */
  /* Results. */
  int64_t rows_sent, rows_received;
  int64_t rows_new; /* received rows that were new here */
  int library_synced;
  int converged;   /* every database exchanged ended with equal digests */
  int we_removed;  /* this device learnt it was removed from the group */
  int follow_up;   /* group rows were written after the group exchange; sync again soon */
} mc_session;

/* Noise KK as the initiator, to the member whose static key is peer_key. */
mc_status mc_session_handshake_initiate(mc_session *s, const unsigned char *peer_key);
/* Noise KK as the responder: finds the calling member and sets peer_uid. A removed
 * member is told so (ERROR "removed") and gets MC_ERR_NOT_FOUND, like a stranger. */
mc_status mc_session_handshake_respond(mc_session *s);

/* Ends a session before HELLO with ERROR {reason, msg}, then reads what the peer sent
 * meanwhile so that closing doesn't reset the connection before it reads the ERROR. */
void mc_session_refuse(mc_channel *ch, const char *reason, const char *msg);

/* Runs the session from HELLO to BYE. */
mc_status mc_session_run(mc_session *s);

/* Brings this device's own group rows up to date: its devices row (name, kind, host, key)
 * and its capabilities (benchmark, free space, power, address hints). Logged only when
 * something changed meaningfully. No-op outside a group. */
mc_status mc_session_refresh_self(mc_sync *sync, mc_sync_conn *c);

#endif

/* Reach checks: whether two devices can open TCP connections to each other. A firewall that
 * lets mDNS through but blocks the sync port shows a device in the list that can't be
 * synced with, or that can only sync when the other side calls. See "Reachability" in
 * docs/sync-protocol.md.
 *
 * Device A connects in MC_MODE_REACH and sends REACH_REQ {"dev", "port"} in the clear. B
 * opens a plain TCP connection to the address A's connection came from, at that port, closes
 * it, and answers REACH_RESP {"dev", "ok"}. So A's connecting says whether A reaches B, and
 * B's answer whether B reaches A; B learns both too. Nothing is authenticated: the answers
 * only drive warnings. Internal to core. */
#ifndef MC_SYNC_REACH_H
#define MC_SYNC_REACH_H

#include <stdint.h>

#include "sync/channel.h"
#include "sync/sync_internal.h"

/* How long B waits for the connection back. */
#define MC_REACH_CONNECT_MS 3000

typedef struct mc_reach_answer {
  int answered;              /* B replied; older versions close the connection instead */
  int ok;                    /* B connected back */
  char uid[MC_UID_STRLEN];   /* the device that answered */
} mc_reach_answer;

/* A, on a channel connected in MC_MODE_REACH: asks to be called back at self_port. *out is
 * zeroed first; MC_OK only when it was answered. */
mc_status mc_reach_ask(mc_channel *ch, const char *self_uid, uint16_t self_port,
                       mc_reach_answer *out);

/* B, on an accepted MC_MODE_REACH channel: reads the request, calls back and answers.
 * claimed gets the uid the caller gave ("" if none or malformed), host the address it
 * called from, *ok whether calling back worked. */
mc_status mc_reach_serve(mc_channel *ch, const char *self_uid, char claimed[MC_UID_STRLEN],
                         char host[MC_SYNC_ADDR_MAX], int *ok);

/* The host part of "1.2.3.4:5" or "[::1]:5" into host; "" if it isn't one. */
void mc_reach_host_of(const char *addr, char host[MC_SYNC_ADDR_MAX]);

#endif

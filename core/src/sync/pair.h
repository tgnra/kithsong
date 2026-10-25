/* Pairing crypto: EC J-PAKE keyed by a 6-digit code, then Noise XXpsk3 with a psk derived
 * from the J-PAKE secret, so both devices learn each other's static key. See "Pairing" in
 * docs/sync-protocol.md. Device A (the initiator, who types the code) is the J-PAKE client;
 * device B (who shows it) is the server.
 *
 * Only the crypto lives here. PAIR_HELLO, PAIR_READY, PAIR_REFUSE, PAIR_ACCEPT and the join
 * rules come before and after it, in the caller. Internal to core. */
#ifndef MC_SYNC_PAIR_H
#define MC_SYNC_PAIR_H

#include "musiccore/musiccore.h"
#include "sync/channel.h"

#define MC_PAIR_CODE_DIGITS 6
/* Failed attempts allowed per code before B draws a new one. */
#define MC_PAIR_MAX_FAILURES 3
/* HKDF info for the psk. */
#define MC_PAIR_PSK_INFO "musicapp pair v1"

/* A uniformly random code, "000000" to "999999". */
mc_status mc_pair_new_code(char code[MC_PAIR_CODE_DIGITS + 1]);

/* J-PAKE over ch's clear-text messages (JPAKE_1 both ways, then JPAKE_2), with the code's
 * six ASCII digits as the secret, then psk = HKDF-SHA256(secret, info MC_PAIR_PSK_INFO).
 * A wrong code still yields a psk, just a different one on each side. *code_used, if
 * given, is set once this side has sent anything that depends on the code. */
mc_status mc_pair_jpake(mc_channel *ch, const char *code, unsigned char psk[MC_NOISE_PSK_LEN],
                        int *code_used);

/* Device A, on a pairing-mode channel it opened, once B has answered PAIR_READY: J-PAKE
 * with the code the user typed, then XXpsk3. On MC_OK the channel is encrypted and
 * peer_key is B's static key. A's side of XXpsk3 ends before B checks the psk, so a wrong
 * code shows up as a failed (or closed) next receive, not here. */
mc_status mc_pair_initiate(mc_channel *ch, const char *code,
                           const unsigned char s_priv[MC_NOISE_KEY_LEN],
                           const unsigned char s_pub[MC_NOISE_KEY_LEN],
                           unsigned char peer_key[MC_NOISE_KEY_LEN]);

/* Device B's side of one pairing request: the code it shows, and how often it has failed. */
typedef struct mc_pair_responder {
  char code[MC_PAIR_CODE_DIGITS + 1];
  int failures;
} mc_pair_responder;

/* Draws the first code. */
mc_status mc_pair_responder_init(mc_pair_responder *r);
/* One attempt on ch, after B has sent PAIR_READY. On MC_OK the channel is encrypted and
 * peer_key is A's static key. A wrong code fails with MC_ERR_FORMAT. Every failure after
 * B has used the code counts; the MC_PAIR_MAX_FAILURES-th draws a new code, sets
 * *new_code (if given) and starts the count again, so each attempt is one online guess.
 * Close the channel after a failure. */
mc_status mc_pair_respond(mc_pair_responder *r, mc_channel *ch,
                          const unsigned char s_priv[MC_NOISE_KEY_LEN],
                          const unsigned char s_pub[MC_NOISE_KEY_LEN],
                          unsigned char peer_key[MC_NOISE_KEY_LEN], int *new_code);
void mc_pair_responder_wipe(mc_pair_responder *r);

#endif

/* Wire constants of the sync protocol: the connection preamble and the message types of
 * "Session" in docs/sync-protocol.md. Every message is <type: u8> <payload>. Internal to
 * core. */
#ifndef MC_SYNC_MSG_H
#define MC_SYNC_MSG_H

/* Preamble: 'M' 'A' <version> <mode>, sent by the initiator; also the Noise prologue. */
#define MC_PREAMBLE_LEN 4
#define MC_PROTOCOL_VERSION 1
#define MC_MODE_PAIRING 1
#define MC_MODE_SESSION 2

/* Session */
#define MC_MSG_HELLO 0x01
#define MC_MSG_BYE 0x02
#define MC_MSG_ERROR 0x03

/* Changelog exchange */
#define MC_MSG_VV 0x10
#define MC_MSG_ROWS 0x11
#define MC_MSG_ROWS_END 0x12

/* Files */
#define MC_MSG_HAVE_REQ 0x20
#define MC_MSG_HAVE_RESP 0x21
#define MC_MSG_FILE_REQ 0x22
#define MC_MSG_FILE_HDR 0x23
#define MC_MSG_FILE_DATA 0x24
#define MC_MSG_FILE_ERR 0x25
#define MC_MSG_PREPARE_REQ 0x26
#define MC_MSG_FILES_DONE 0x27 /* the receiver has asked for all it wants; the turn passes */
#define MC_MSG_COVER_REQ 0x28  /* only to a peer whose HELLO has "covers": true */
#define MC_MSG_COVER_HDR 0x29

/* Pairing. PAIR_HELLO to JPAKE_2 travel in the clear, PAIR_ACCEPT encrypted. */
#define MC_MSG_PAIR_HELLO 0x40
#define MC_MSG_PAIR_READY 0x41
#define MC_MSG_PAIR_REFUSE 0x42
#define MC_MSG_JPAKE_1 0x43
#define MC_MSG_JPAKE_2 0x44
#define MC_MSG_PAIR_ACCEPT 0x45

#endif

/* A C face for Apple's C++ ALAC reference codec. Each handle is independent, so different
 * threads may each use their own. */
#ifndef KITHIPOD_ALAC_SHIM_H
#define KITHIPOD_ALAC_SHIM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IPOD_ALAC_FRAMES_PER_PACKET 4096

typedef struct ipod_alac_enc ipod_alac_enc;

/* bits: 16 or 24. NULL on bad parameters or no memory. */
ipod_alac_enc *ipod_alac_enc_new(uint32_t sample_rate, uint32_t channels, uint32_t bits);
void ipod_alac_enc_free(ipod_alac_enc *e);
/* The magic cookie (ALACSpecificConfig, 24 bytes for mono and stereo) for the 'alac' box. */
uint32_t ipod_alac_enc_cookie(ipod_alac_enc *e, uint8_t *buf, uint32_t cap);
/* Largest packet ipod_alac_enc_packet can produce. */
uint32_t ipod_alac_enc_max_packet(const ipod_alac_enc *e);
/* Encodes up to IPOD_ALAC_FRAMES_PER_PACKET frames of interleaved PCM: int16_t for 16 bits,
 * int32_t holding 24-bit values for 24. Returns the packet size, or 0 on error. */
uint32_t ipod_alac_enc_packet(ipod_alac_enc *e, const void *pcm, uint32_t frames, uint8_t *out);

/* Decoding, for checking the encoder's output. */
typedef struct ipod_alac_dec ipod_alac_dec;
ipod_alac_dec *ipod_alac_dec_new(const uint8_t *cookie, uint32_t len);
void ipod_alac_dec_free(ipod_alac_dec *d);
/* Decodes one packet into interleaved samples (int16_t for 16 bits, packed 3-byte little
 * endian for 24). Returns frames decoded, or -1. */
int32_t ipod_alac_dec_packet(ipod_alac_dec *d, const uint8_t *pkt, uint32_t len, uint8_t *out, uint32_t channels);

#ifdef __cplusplus
}
#endif

#endif

/* A minimal MP4 (M4A) writer for ALAC: ftyp, moov (one sound track with an 'alac' sample
 * entry, stts/stsc/stsz/stco, iTunes-style metadata in udta/meta/ilst) and mdat, with moov
 * before mdat as iTunes writes it. Packets go to a side file while encoding and the final
 * file is assembled at the end. */
#ifndef KITHIPOD_M4A_H
#define KITHIPOD_M4A_H

#include <stdint.h>

#include "kithipod/common.h"

typedef struct ipod_m4a ipod_m4a;

ipod_m4a *ipod_m4a_open(const char *path, uint32_t sample_rate, uint16_t channels, uint16_t bits,
                        const uint8_t *cookie, uint32_t cookie_len, ipod_error *err);
int ipod_m4a_packet(ipod_m4a *m, const uint8_t *pkt, uint32_t len, uint32_t frames, ipod_error *err);
/* Writes the final file and frees m. */
int ipod_m4a_finish(ipod_m4a *m, const ipod_tags *tags, uint64_t *file_size, ipod_error *err);
/* Deletes what was written and frees m. */
void ipod_m4a_abort(ipod_m4a *m);

/* The ilst box (iTunes metadata) for tags, appended to b. Exposed for the tests. */
struct ipod_buf;
void ipod_m4a_put_ilst(struct ipod_buf *b, const ipod_tags *tags);

#endif

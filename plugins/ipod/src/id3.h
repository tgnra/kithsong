/* ID3v2.3 tags for MP3 files: text frames (ISO-8859-1 when ASCII, else UTF-16 with BOM),
 * TRCK/TPOS as "n/total", iTunes' compilation (TCMP) and sort frames, and APIC cover. */
#ifndef KITHIPOD_ID3_H
#define KITHIPOD_ID3_H

#include "kithipod/common.h"
#include "util.h"

/* Appends a complete tag (header, frames, padding) to b. */
void ipod_id3_build(ipod_buf *b, const ipod_tags *tags);

#endif

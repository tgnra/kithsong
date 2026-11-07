/* What the iPod plugin's files share. The plugin includes kithcore/plugin.h and kithipod's
 * public headers only. */
#ifndef KITHIPOD_PLUGIN_INTERNAL_H
#define KITHIPOD_PLUGIN_INTERNAL_H

#include "kithcore/plugin.h"

/* Core's table, kept from kith_plugin_init(). */
extern const kith_host *ipod_plugin_host;

/* The strings shown to the user, in the UI language. */
typedef struct ipod_plugin_strings {
  const char *plugin_name;     /* "iPod sync" */
  const char *target_name;     /* "iPod" */
  const char *artwork_label;   /* the "artwork" option */
  const char *artwork_desc;
  const char *mp3_v0, *mp3_v2, *mp3_320, *mp3_256, *mp3_192, *mp3_128;
} ipod_plugin_strings;

const ipod_plugin_strings *ipod_plugin_strings_for(const char *language);

/* The encoder (encode.c): ALAC and MP3. */
void ipod_plugin_encoder_init(const ipod_plugin_strings *s, kith_encoder *out);

/* Logs through the host, printf-style. */
void ipod_plugin_log(kith_log_level level, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
  __attribute__((format(printf, 2, 3)))
#endif
  ;

#endif

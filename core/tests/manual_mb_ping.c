/* Manual check that HTTPS (curl + mbedTLS + embedded CA bundle) and JSON parsing work:
 *   build/<preset>/core/tests/manual_mb_ping
 * Expected output: "MusicBrainz OK: Radiohead". */
#include <stdio.h>

#include "musiccore/musiccore.h"

int main(void) {
  if (mc_init() != MC_OK) {
    fprintf(stderr, "mc_init failed\n");
    return 1;
  }
  char *name = NULL;
  mc_status st = mc_musicbrainz_ping(&name);
  if (st == MC_OK) printf("MusicBrainz OK: %s\n", name);
  else fprintf(stderr, "MusicBrainz ping failed: %s\n", mc_status_str(st));
  mc_free(name);
  mc_shutdown();
  return st == MC_OK ? 0 : 1;
}

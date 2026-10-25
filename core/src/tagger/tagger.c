/* Autotagger pipeline (see docs/tagging-rules.md):
 *   1. walk the directory and read existing tags (tag_io.cpp)
 *   2. group files into album candidates by tags, then folder layout
 *   3. optionally fingerprint (Chromaprint) and look up on AcoustID
 *   4. search MusicBrainz releases (musicbrainz.c)
 *   5. score each release by weighted distance; assign files to tracks (Hungarian)
 *   6. hand ranked candidates to the frontend; write tags + library rows on confirm
 */
#include "musiccore/tagger.h"

mc_status mc_tagger_new(mc_library *lib, mc_tagger **out) {
  (void)lib;
  if (out) *out = NULL;
  return MC_ERR_UNIMPLEMENTED;
}

void mc_tagger_free(mc_tagger *tagger) {
  (void)tagger;
}

mc_status mc_tagger_import_dir(mc_tagger *tagger, const char *path) {
  (void)tagger;
  (void)path;
  return MC_ERR_UNIMPLEMENTED;
}

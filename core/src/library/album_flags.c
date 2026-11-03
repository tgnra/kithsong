/* See musiccore/edit.h. */
#include "musiccore/edit.h"

#include "musiccore/library.h"

mc_status mc_album_flags_get(mc_library *lib, const char *album_uid, mc_album_flags *out) {
  (void)lib, (void)album_uid, (void)out;
  return MC_ERR_UNIMPLEMENTED;
}
mc_status mc_album_flags_set(mc_library *lib, const char *album_uid, const mc_album_flags *flags) {
  (void)lib, (void)album_uid, (void)flags;
  return MC_ERR_UNIMPLEMENTED;
}

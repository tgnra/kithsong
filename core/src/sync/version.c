#include "sync/version.h"

#include <stdio.h>
#include <string.h>

#include <yyjson.h>

#include "kithcore/i18n.h"

static const char *const k_features[MC_FEAT_COUNT] = {
  [MC_FEAT_TEST] = "test",
};

void mc_version_self_default(mc_version_self *self) {
  memset(self, 0, sizeof *self);
  self->protocol = MC_PROTOCOL_VERSION;
  self->protocol_min = MC_PROTOCOL_MIN_VERSION;
  snprintf(self->app, sizeof self->app, "%s", mc_version());
  self->check = 1;
  self->features = MC_FEAT_DEFAULT;
}

void mc_version_write(yyjson_mut_doc *doc, yyjson_mut_val *obj, const mc_version_self *self,
                      int features) {
  if (self->protocol >= 0) yyjson_mut_obj_add_int(doc, obj, "v", self->protocol);
  if (self->protocol_min >= 0) yyjson_mut_obj_add_int(doc, obj, "v_min", self->protocol_min);
  if (self->app[0]) yyjson_mut_obj_add_strcpy(doc, obj, "app", self->app);
  if (!features) return;
  yyjson_mut_val *arr = yyjson_mut_arr(doc);
  for (int i = 0; arr && i < MC_FEAT_COUNT; i++)
    if (self->features & MC_FEAT_BIT(i)) yyjson_mut_arr_add_str(doc, arr, k_features[i]);
  if (arr) yyjson_mut_obj_add_val(doc, obj, "feat", arr);
}

static int small_int(yyjson_val *v) {
  if (!yyjson_is_int(v)) return 0;
  int64_t n = yyjson_get_sint(v);
  return n > 0 && n < 1000000 ? (int)n : 0;
}

int mc_version_read(const mc_version_self *self, yyjson_val *obj, mc_sync_version *peer) {
  memset(peer, 0, sizeof *peer);
  peer->protocol = small_int(yyjson_obj_get(obj, "v"));
  peer->protocol_min = small_int(yyjson_obj_get(obj, "v_min"));
  const char *app = yyjson_get_str(yyjson_obj_get(obj, "app"));
  snprintf(peer->app, sizeof peer->app, "%s", app ? app : "");
  return mc_version_judge(self, peer);
}

int mc_version_judge(const mc_version_self *self, mc_sync_version *peer) {
  peer->outdated = MC_SYNC_VERSION_OK;
  if (!self->check) return 1;
  int mine = self->protocol > 0 ? self->protocol : 0;
  if (peer->protocol < self->protocol_min)
    peer->outdated = MC_SYNC_PEER_OUTDATED;
  else if (mine < peer->protocol_min)
    peer->outdated = MC_SYNC_SELF_OUTDATED;
  return peer->outdated == MC_SYNC_VERSION_OK;
}

unsigned mc_version_read_features(yyjson_val *obj) {
  yyjson_val *arr = yyjson_obj_get(obj, "feat"), *v;
  unsigned out = 0;
  size_t idx, max;
  yyjson_arr_foreach(arr, idx, max, v) {
    const char *name = yyjson_get_str(v);
    for (int i = 0; name && i < MC_FEAT_COUNT; i++)
      if (!strcmp(name, k_features[i])) out |= MC_FEAT_BIT(i);
  }
  return out;
}

size_t mc_sync_version_text(const char *name, const mc_sync_version *v, char *buf, size_t size) {
  if (!v || v->outdated == MC_SYNC_VERSION_OK) {
    if (buf && size) buf[0] = '\0';
    return 0;
  }
  int named = name && *name;
  const char *start = named ? name : mc_str(MC_STR_OTHER_DEVICE_START);
  const char *again = named ? name : mc_str(MC_STR_SYNC_OTHER_DEVICE);
  if (v->outdated == MC_SYNC_PEER_OUTDATED)
    return v->app[0] ? mc_format(buf, size, mc_str(MC_STR_SYNC_VERSION_PEER_OUTDATED), MC_S(start),
                                 MC_S(v->app), MC_S(again))
                     : mc_format(buf, size, mc_str(MC_STR_SYNC_VERSION_PEER_OUTDATED_UNKNOWN),
                                 MC_S(start), MC_S(again));
  return v->app[0] ? mc_format(buf, size, mc_str(MC_STR_SYNC_VERSION_SELF_OUTDATED), MC_S(start),
                               MC_S(v->app))
                   : mc_format(buf, size, mc_str(MC_STR_SYNC_VERSION_SELF_OUTDATED_UNKNOWN),
                               MC_S(start));
}

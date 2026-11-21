/* Protocol versions and optional features, as HELLO, PAIR_HELLO, PAIR_READY and the
 * "version" refusals carry them. See "Versions" in docs/sync-protocol.md. Internal to core. */
#ifndef MC_SYNC_VERSION_H
#define MC_SYNC_VERSION_H

#include "kithcore/discovery.h"
#include "kithcore/sync.h"

struct yyjson_mut_doc;
struct yyjson_mut_val;
struct yyjson_val;

#define MC_PROTOCOL_VERSION MC_DISCOVERY_VERSION
#define MC_PROTOCOL_MIN_VERSION MC_DISCOVERY_MIN_VERSION

/* HELLO "feat": a feature is used in a session only when both HELLOs name it
 * (mc_session.features). A new optional feature older peers can go without gets a flag here
 * and its name in version.c, rather than a raised minimum version. */
typedef enum mc_feature {
  MC_FEAT_TEST, /* "test": announced only when a test asks (mc_sync_set_test_version()) */
  MC_FEAT_COUNT
} mc_feature;

#define MC_FEAT_BIT(f) (1u << (f))
/* What a build announces: every flag but MC_FEAT_TEST. */
#define MC_FEAT_DEFAULT ((MC_FEAT_BIT(MC_FEAT_COUNT) - 1) & ~MC_FEAT_BIT(MC_FEAT_TEST))

/* What this device says of itself. The build's own, unless a test poses as another version. */
typedef struct mc_version_self {
  int protocol;     /* < 0: sends no "v" */
  int protocol_min; /* < 0: sends no "v_min" */
  char app[MC_SYNC_APP_VERSION_MAX]; /* "": sends no "app" */
  int check;        /* 0: refuses nobody and takes a "version" refusal for an unknown one, as
                     * releases up to 0.9.4 do */
  unsigned features;
} mc_version_self;

void mc_version_self_default(mc_version_self *self);

/* Adds what self says to obj: "v", "v_min", "app", and with features "feat". */
void mc_version_write(struct yyjson_mut_doc *doc, struct yyjson_mut_val *obj,
                      const mc_version_self *self, int features);
/* Sets peer->outdated from its protocol versions. 1 if self and the peer can sync: each one's
 * version is at least the other's minimum. A peer that says no version can't. */
int mc_version_judge(const mc_version_self *self, mc_sync_version *peer);
/* Reads "v", "v_min" and "app" from obj (anything else counts as not said), then judges. */
int mc_version_read(const mc_version_self *self, struct yyjson_val *obj,
                    mc_sync_version *peer);
/* The flags of obj's "feat" that this build knows. */
unsigned mc_version_read_features(struct yyjson_val *obj);

#endif

/* JNI bindings for ng.kithso.core.SyncHereNatives: removing an album or artist from this
 * phone, and syncing it here again (musiccore/sync_here.h), on an open CoreSync's handle.
 * Kept apart from jni_bridge.c; as thin as it is. Failures throw MusicCoreException. */
#include <jni.h>
#include <stdint.h>

#include "jni_bridge.h"
#include "musiccore/musiccore.h"

/* kind (an mc_profile_item_kind) checked: albums and artists only. */
static int check_kind(JNIEnv *env, jint kind) {
  if (kind == MC_PROFILE_ALBUM || kind == MC_PROFILE_ARTIST) return 1;
  throw_status(env, MC_ERR_INVALID_ARG);
  return 0;
}

/* The mc_here_state of uid. */
JNIEXPORT jint JNICALL Java_ng_kithso_core_SyncHereNatives_nativeState(JNIEnv *env, jclass cls, jlong handle,
                                                                     jint kind, jstring uid) {
  (void)cls;
  if (!check_kind(env, kind)) return 0;
  const char *cuid = (*env)->GetStringUTFChars(env, uid, NULL);
  if (!cuid) return 0;
  mc_here_state state = MC_HERE_UNAVAILABLE;
  mc_status st = mc_sync_here_state(jni_sync_of(handle), (mc_profile_item_kind)kind, cuid, &state);
  (*env)->ReleaseStringUTFChars(env, uid, cuid);
  if (st != MC_OK) throw_status(env, st);
  return (jint)state;
}

/* The plan for removing uid, or NULL with an exception pending. */
static mc_here_plan *plan_of(JNIEnv *env, jlong handle, jint kind, jstring uid) {
  if (!check_kind(env, kind)) return NULL;
  const char *cuid = (*env)->GetStringUTFChars(env, uid, NULL);
  if (!cuid) return NULL;
  mc_here_plan *plan = NULL;
  mc_status st = mc_sync_here_plan_new(jni_sync_of(handle), (mc_profile_item_kind)kind, cuid, &plan);
  (*env)->ReleaseStringUTFChars(env, uid, cuid);
  if (st != MC_OK) {
    throw_status(env, st);
    return NULL;
  }
  return plan;
}

/* What removing uid would do, for asking first: {files removed now, originals kept}. */
JNIEXPORT jintArray JNICALL Java_ng_kithso_core_SyncHereNatives_nativePreview(JNIEnv *env, jclass cls,
                                                                            jlong handle, jint kind,
                                                                            jstring uid) {
  (void)cls;
  mc_here_plan *plan = plan_of(env, handle, kind, uid);
  if (!plan) return NULL;
  jint values[] = {mc_here_plan_files(plan), mc_here_plan_kept(plan)};
  mc_here_plan_free(plan);
  return jni_ints(env, values, 2);
}

/* Removes uid from this phone: {files removed, files kept}. */
JNIEXPORT jintArray JNICALL Java_ng_kithso_core_SyncHereNatives_nativeRemove(JNIEnv *env, jclass cls,
                                                                           jlong handle, jint kind,
                                                                           jstring uid) {
  (void)cls;
  mc_here_plan *plan = plan_of(env, handle, kind, uid);
  if (!plan) return NULL;
  mc_here_result res;
  mc_status st = mc_sync_remove_here(jni_sync_of(handle), plan, &res);
  mc_here_plan_free(plan);
  if (st != MC_OK) {
    throw_status(env, st);
    return NULL;
  }
  jint values[] = {res.files_removed, res.files_kept};
  return jni_ints(env, values, 2);
}

/* Syncs uid to this phone again. */
JNIEXPORT void JNICALL Java_ng_kithso_core_SyncHereNatives_nativeRestore(JNIEnv *env, jclass cls, jlong handle,
                                                                       jint kind, jstring uid) {
  (void)cls;
  if (!check_kind(env, kind)) return;
  const char *cuid = (*env)->GetStringUTFChars(env, uid, NULL);
  if (!cuid) return;
  mc_status st = mc_sync_restore_here(jni_sync_of(handle), (mc_profile_item_kind)kind, cuid);
  (*env)->ReleaseStringUTFChars(env, uid, cuid);
  if (st != MC_OK) throw_status(env, st);
}

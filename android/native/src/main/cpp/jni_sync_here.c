/* JNI bindings for ng.kithso.core.SyncHereNatives: removing an album or artist from this
 * phone, and syncing it here again (musiccore/sync_here.h), on an open CoreSync's handle.
 * Kept apart from jni_bridge.c; as thin as it is. Failures throw KithCoreException. */
#include <jni.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "jni_bridge.h"
#include "kithcore/kithcore.h"

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

/* What removing uid would do, for asking first: {files removed now, originals kept, tracks a
 * playlist keeps}. */
JNIEXPORT jintArray JNICALL Java_ng_kithso_core_SyncHereNatives_nativePreview(JNIEnv *env, jclass cls,
                                                                            jlong handle, jint kind,
                                                                            jstring uid) {
  (void)cls;
  mc_here_plan *plan = plan_of(env, handle, kind, uid);
  if (!plan) return NULL;
  jint values[] = {mc_here_plan_files(plan), mc_here_plan_kept(plan), mc_here_plan_staying(plan)};
  mc_here_plan_free(plan);
  return jni_ints(env, values, 3);
}

/* Whether removing uid would remove a file of any of track_uids (the play queue's), so the app
 * can stop playing them first. */
JNIEXPORT jboolean JNICALL Java_ng_kithso_core_SyncHereNatives_nativeTakesAny(JNIEnv *env, jclass cls,
                                                                            jlong handle, jint kind,
                                                                            jstring uid,
                                                                            jobjectArray track_uids) {
  (void)cls;
  mc_here_plan *plan = plan_of(env, handle, kind, uid);
  if (!plan) return JNI_FALSE;
  jboolean takes = JNI_FALSE;
  jsize n = (*env)->GetArrayLength(env, track_uids);
  for (jsize i = 0; i < n && !takes; i++) {
    jstring s = (jstring)(*env)->GetObjectArrayElement(env, track_uids, i);
    if (!s) continue;
    const char *c = (*env)->GetStringUTFChars(env, s, NULL);
    if (c) {
      takes = mc_here_plan_has(plan, c) ? JNI_TRUE : JNI_FALSE;
      (*env)->ReleaseStringUTFChars(env, s, c);
    }
    (*env)->DeleteLocalRef(env, s);
    if (!c) break; /* OutOfMemoryError pending */
  }
  mc_here_plan_free(plan);
  return takes;
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

/* A tick under Sync everything: excludes uid from device_uid's profile, or with on false takes
 * it out of what's removed. */
JNIEXPORT void JNICALL Java_ng_kithso_core_SyncHereNatives_nativeExclude(JNIEnv *env, jclass cls, jlong handle,
                                                                       jstring device_uid, jint kind, jstring uid,
                                                                       jboolean on) {
  (void)cls;
  if (!check_kind(env, kind)) return;
  const char *cdevice = (*env)->GetStringUTFChars(env, device_uid, NULL);
  const char *cuid = cdevice ? (*env)->GetStringUTFChars(env, uid, NULL) : NULL;
  if (cuid) {
    char uids[1][MC_UID_STRLEN];
    snprintf(uids[0], MC_UID_STRLEN, "%s", cuid);
    mc_status st = mc_sync_exclude_items(jni_sync_of(handle), cdevice, (mc_profile_item_kind)kind,
                                         (const char(*)[MC_UID_STRLEN])uids, 1, on ? 1 : 0);
    (*env)->ReleaseStringUTFChars(env, uid, cuid);
    if (st != MC_OK) throw_status(env, st);
  }
  if (cdevice) (*env)->ReleaseStringUTFChars(env, device_uid, cdevice);
}

/* Select all or Select none under What to sync, on the rows shown. */
JNIEXPORT void JNICALL Java_ng_kithso_core_SyncHereNatives_nativeSelect(JNIEnv *env, jclass cls, jlong handle,
                                                                      jstring device_uid, jint kind, jobjectArray uids,
                                                                      jboolean on) {
  (void)cls;
  jsize count = (*env)->GetArrayLength(env, uids);
  char (*cuids)[MC_UID_STRLEN] = calloc(count ? (size_t)count : 1, sizeof *cuids);
  if (!cuids) {
    throw_status(env, MC_ERR_NOMEM);
    return;
  }
  for (jsize i = 0; i < count; i++) {
    jstring uid = (*env)->GetObjectArrayElement(env, uids, i);
    const char *cuid = uid ? (*env)->GetStringUTFChars(env, uid, NULL) : NULL;
    if (!cuid) {
      free(cuids);
      return;
    }
    snprintf(cuids[i], MC_UID_STRLEN, "%s", cuid);
    (*env)->ReleaseStringUTFChars(env, uid, cuid);
    (*env)->DeleteLocalRef(env, uid);
  }
  const char *cdevice = (*env)->GetStringUTFChars(env, device_uid, NULL);
  if (cdevice) {
    mc_status st = mc_sync_select_items(jni_sync_of(handle), cdevice, (mc_profile_item_kind)kind,
                                        (const char(*)[MC_UID_STRLEN])cuids, count, on ? 1 : 0);
    (*env)->ReleaseStringUTFChars(env, device_uid, cdevice);
    if (st != MC_OK) throw_status(env, st);
  }
  free(cuids);
}

/* The recycle bin (musiccore/delete.h): whether deletes here go to it. */
JNIEXPORT jboolean JNICALL Java_ng_kithso_core_SyncHereNatives_nativeBinOffered(JNIEnv *env, jclass cls,
                                                                             jlong handle) {
  (void)env;
  (void)cls;
  return mc_sync_bin_offered(jni_sync_of(handle)) ? JNI_TRUE : JNI_FALSE;
}

/* How many entries it holds; while any, sync waits for the Sync button. */
JNIEXPORT jint JNICALL Java_ng_kithso_core_SyncHereNatives_nativeBinCount(JNIEnv *env, jclass cls,
                                                                       jlong handle) {
  (void)env;
  (void)cls;
  int n = mc_sync_bin_count(jni_sync_of(handle));
  return n < 0 ? 0 : n;
}

/* Takes entry id back out of it, as removed from this phone: how many entries came back. */
JNIEXPORT jint JNICALL Java_ng_kithso_core_SyncHereNatives_nativeBinRestore(JNIEnv *env, jclass cls,
                                                                         jlong handle, jlong id) {
  (void)cls;
  int entries = 0;
  mc_status st = mc_sync_bin_restore(jni_sync_of(handle), (int64_t)id, &entries);
  if (st != MC_OK) throw_status(env, st);
  return entries;
}

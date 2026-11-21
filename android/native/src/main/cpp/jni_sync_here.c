/* JNI bindings for ng.kithso.core.SyncHereNatives: removing an album or artist from this
 * phone, and syncing it here again (kithcore/sync_here.h), on an open CoreSync's handle.
 * Failures throw KithCoreException. */
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
JNI_FN(jint, SyncHereNatives, nativeState, jlong handle, jint kind, jstring uid) {
  if (!check_kind(env, kind)) return 0;
  JNI_STR(cuid, uid, 0);
  mc_here_state state = MC_HERE_UNAVAILABLE;
  jni_check(env, mc_sync_here_state(jni_sync_of(handle), (mc_profile_item_kind)kind, cuid, &state));
  return (jint)state;
}

/* The plan for removing uid, or NULL with an exception pending. */
static mc_here_plan *plan_of(JNIEnv *env, jlong handle, jint kind, jstring uid) {
  if (!check_kind(env, kind)) return NULL;
  JNI_STR(cuid, uid, NULL);
  mc_here_plan *plan = NULL;
  JNI_CHECK(mc_sync_here_plan_new(jni_sync_of(handle), (mc_profile_item_kind)kind, cuid, &plan), NULL);
  return plan;
}

/* What removing uid would do, for asking first: {files removed now, originals kept, tracks a
 * playlist keeps}. */
JNI_FN(jintArray, SyncHereNatives, nativePreview, jlong handle, jint kind, jstring uid) {
  mc_here_plan *plan = plan_of(env, handle, kind, uid);
  if (!plan) return NULL;
  jint values[] = {mc_here_plan_files(plan), mc_here_plan_kept(plan), mc_here_plan_staying(plan)};
  mc_here_plan_free(plan);
  return jni_ints(env, values, 3);
}

/* Whether removing uid would remove a file of any of track_uids (the play queue's), so the app
 * can stop playing them first. */
JNI_FN(jboolean, SyncHereNatives, nativeTakesAny, jlong handle, jint kind, jstring uid, jobjectArray track_uids) {
  mc_here_plan *plan = plan_of(env, handle, kind, uid);
  if (!plan) return JNI_FALSE;
  jboolean takes = JNI_FALSE;
  jsize n = (*env)->GetArrayLength(env, track_uids);
  for (jsize i = 0; i < n && !takes; i++) {
    jstring s = (jstring)(*env)->GetObjectArrayElement(env, track_uids, i);
    if (!s) continue;
    int ok = 1;
    const char *c = jni_chars(env, s, &ok);
    if (c) takes = JNI_BOOL(mc_here_plan_has(plan, c));
    jni_release(env, s, c);
    (*env)->DeleteLocalRef(env, s);
    if (!ok) break; /* OutOfMemoryError pending */
  }
  mc_here_plan_free(plan);
  return takes;
}

/* Removes uid from this phone: {files removed, files kept}. */
JNI_FN(jintArray, SyncHereNatives, nativeRemove, jlong handle, jint kind, jstring uid) {
  mc_here_plan *plan = plan_of(env, handle, kind, uid);
  if (!plan) return NULL;
  mc_here_result res;
  mc_status st = mc_sync_remove_here(jni_sync_of(handle), plan, &res);
  mc_here_plan_free(plan);
  JNI_CHECK(st, NULL);
  jint values[] = {res.files_removed, res.files_kept};
  return jni_ints(env, values, 2);
}

/* Syncs uid to this phone again. */
JNI_FN(void, SyncHereNatives, nativeRestore, jlong handle, jint kind, jstring uid) {
  if (!check_kind(env, kind)) return;
  JNI_STR(cuid, uid);
  JNI_CHECK(mc_sync_restore_here(jni_sync_of(handle), (mc_profile_item_kind)kind, cuid));
}

/* A tick under Sync everything: excludes uid from device_uid's profile, or with on false takes
 * it out of what's removed. */
JNI_FN(void, SyncHereNatives, nativeExclude, jlong handle, jstring device_uid, jint kind, jstring uid, jboolean on) {
  if (!check_kind(env, kind)) return;
  JNI_STR(cdevice, device_uid);
  JNI_STR(cuid, uid);
  char uids[1][MC_UID_STRLEN];
  snprintf(uids[0], MC_UID_STRLEN, "%s", cuid);
  JNI_CHECK(mc_sync_exclude_items(jni_sync_of(handle), cdevice, (mc_profile_item_kind)kind,
                                  (const char(*)[MC_UID_STRLEN])uids, 1, on ? 1 : 0));
}

/* Select all or Select none under What to sync, on the rows shown. */
JNI_FN(void, SyncHereNatives, nativeSelect, jlong handle, jstring device_uid, jint kind, jobjectArray uids,
       jboolean on) {
  jsize count = (*env)->GetArrayLength(env, uids);
  char (*cuids)[MC_UID_STRLEN] = calloc(count ? (size_t)count : 1, sizeof *cuids);
  if (!cuids) {
    throw_status(env, MC_ERR_NOMEM);
    return;
  }
  for (jsize i = 0; i < count; i++) {
    jstring uid = (*env)->GetObjectArrayElement(env, uids, i);
    int ok = 1;
    const char *cuid = jni_chars(env, uid, &ok);
    if (!cuid) {
      free(cuids);
      return;
    }
    snprintf(cuids[i], MC_UID_STRLEN, "%s", cuid);
    jni_release(env, uid, cuid);
    (*env)->DeleteLocalRef(env, uid);
  }
  int ok = 1;
  const char *cdevice = jni_chars(env, device_uid, &ok);
  if (cdevice)
    jni_check(env, mc_sync_select_items(jni_sync_of(handle), cdevice, (mc_profile_item_kind)kind,
                                        (const char(*)[MC_UID_STRLEN])cuids, count, on ? 1 : 0));
  jni_release(env, device_uid, cdevice);
  free(cuids);
}

/* The recycle bin (kithcore/delete.h): whether deletes here go to it. */
JNI_FN(jboolean, SyncHereNatives, nativeBinOffered, jlong handle) {
  return JNI_BOOL(mc_sync_bin_offered(jni_sync_of(handle)));
}

/* How many entries it holds; while any, sync waits for the Sync button. */
JNI_FN(jint, SyncHereNatives, nativeBinCount, jlong handle) {
  int n = mc_sync_bin_count(jni_sync_of(handle));
  return n < 0 ? 0 : n;
}

/* Takes entry id back out of it, as removed from this phone: how many entries came back. */
JNI_FN(jint, SyncHereNatives, nativeBinRestore, jlong handle, jlong id) {
  int entries = 0;
  jni_check(env, mc_sync_bin_restore(jni_sync_of(handle), (int64_t)id, &entries));
  return entries;
}

/* JNI bindings for ng.kithso.core.DeleteNatives: deleting albums, artists and tracks
 * (musiccore/delete.h) on an open CoreLibrary's handle. Kept apart from jni_bridge.c; as thin
 * as it is: marshal arguments, call musiccore, convert results. Android has no trash, so a
 * delete is mc_delete_run_permanently(). Failures throw ng.kithso.core.MusicCoreException. */
#include <jni.h>
#include <stdint.h>

#include "musiccore/musiccore.h"

/* Throws MusicCoreException(status). */
static void throw_status(JNIEnv *env, mc_status st) {
  jclass cls = (*env)->FindClass(env, "ng/kithso/core/MusicCoreException");
  if (!cls) return; /* NoClassDefFoundError is already pending */
  jmethodID ctor = (*env)->GetMethodID(env, cls, "<init>", "(I)V");
  jobject ex = ctor ? (*env)->NewObject(env, cls, ctor, (jint)st) : NULL;
  if (ex) (*env)->Throw(env, (jthrowable)ex);
}

/* An int[] of count values, or NULL with OutOfMemoryError pending. */
static jintArray ints(JNIEnv *env, const jint *values, jsize count) {
  jintArray out = (*env)->NewIntArray(env, count);
  if (out) (*env)->SetIntArrayRegion(env, out, 0, count, values);
  return out;
}

/* The plan for deleting uid, of kind (mc_delete_kind), or NULL with an exception pending. */
static mc_delete_plan *plan_of(JNIEnv *env, jlong handle, jint kind, jstring uid) {
  if (kind < MC_DELETE_TRACKS || kind > MC_DELETE_ARTISTS) {
    throw_status(env, MC_ERR_INVALID_ARG);
    return NULL;
  }
  const char *cuid = (*env)->GetStringUTFChars(env, uid, NULL);
  if (!cuid) return NULL;
  const char *uids[] = {cuid};
  mc_delete_plan *plan = NULL;
  mc_status st = mc_delete_plan_new((mc_library *)(intptr_t)handle, (mc_delete_kind)kind, uids, 1, &plan);
  (*env)->ReleaseStringUTFChars(env, uid, cuid);
  if (st != MC_OK) {
    throw_status(env, st);
    return NULL;
  }
  return plan;
}

/* What deleting uid would delete, for asking first: {tracks, files on this device}. */
JNIEXPORT jintArray JNICALL Java_ng_kithso_core_DeleteNatives_nativePreview(JNIEnv *env, jclass cls,
                                                                          jlong handle, jint kind,
                                                                          jstring uid) {
  (void)cls;
  mc_delete_plan *plan = plan_of(env, handle, kind, uid);
  if (!plan) return NULL;
  jint values[] = {mc_delete_plan_tracks(plan), mc_delete_plan_files(plan)};
  mc_delete_plan_free(plan);
  return ints(env, values, 2);
}

/* Deletes uid for good: {tracks deleted, files removed, tracks kept}. */
JNIEXPORT jintArray JNICALL Java_ng_kithso_core_DeleteNatives_nativeDelete(JNIEnv *env, jclass cls,
                                                                         jlong handle, jint kind,
                                                                         jstring uid) {
  (void)cls;
  mc_delete_plan *plan = plan_of(env, handle, kind, uid);
  if (!plan) return NULL;
  mc_delete_result res;
  mc_status st = mc_delete_run_permanently((mc_library *)(intptr_t)handle, plan, &res);
  mc_delete_plan_free(plan);
  if (st != MC_OK) {
    throw_status(env, st);
    return NULL;
  }
  jint values[] = {res.tracks_deleted, res.files_trashed, res.tracks_kept};
  return ints(env, values, 3);
}

/* Whether deleting uid would delete any of track_uids (a String[]), e.g. the play queue's, so
 * playback can stop first as on the desktops. */
JNIEXPORT jboolean JNICALL Java_ng_kithso_core_DeleteNatives_nativeTakesAny(JNIEnv *env, jclass cls,
                                                                         jlong handle, jint kind,
                                                                         jstring uid,
                                                                         jobjectArray track_uids) {
  (void)cls;
  mc_delete_plan *plan = plan_of(env, handle, kind, uid);
  if (!plan) return JNI_FALSE;
  jboolean takes = JNI_FALSE;
  jsize n = (*env)->GetArrayLength(env, track_uids);
  for (jsize i = 0; i < n && !takes; i++) {
    jstring s = (jstring)(*env)->GetObjectArrayElement(env, track_uids, i);
    if (!s) continue;
    const char *c = (*env)->GetStringUTFChars(env, s, NULL);
    if (c) {
      takes = mc_delete_plan_has(plan, c) ? JNI_TRUE : JNI_FALSE;
      (*env)->ReleaseStringUTFChars(env, s, c);
    }
    (*env)->DeleteLocalRef(env, s);
    if (!c) break; /* OutOfMemoryError pending */
  }
  mc_delete_plan_free(plan);
  return takes;
}

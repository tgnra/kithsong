/* JNI bindings for ng.kithso.core.DeleteNatives: deleting albums and tracks
 * (kithcore/delete.h) on an open CoreLibrary's handle. Android has no trash, so a delete is
 * mc_delete_run_permanently(), or in a group mc_delete_run_to_bin(), which keeps it in the
 * recycle bin until the next sync. Failures throw ng.kithso.core.KithCoreException. */
#include <jni.h>
#include <stdint.h>

#include "kithcore/kithcore.h"

#include "jni_bridge.h"

/* The plan for deleting uid, of kind (mc_delete_kind), or NULL with an exception pending. */
static mc_delete_plan *plan_of(JNIEnv *env, jlong handle, jint kind, jstring uid) {
  if (kind < MC_DELETE_TRACKS || kind > MC_DELETE_ALBUMS) {
    throw_status(env, MC_ERR_INVALID_ARG);
    return NULL;
  }
  JNI_STR(cuid, uid, NULL);
  const char *uids[] = {cuid};
  mc_delete_plan *plan = NULL;
  JNI_CHECK(mc_delete_plan_new(jni_lib_of(handle), (mc_delete_kind)kind, uids, 1, &plan), NULL);
  return plan;
}

/* What deleting uid would delete, for asking first: {tracks, files on this device}. */
JNI_FN(jintArray, DeleteNatives, nativePreview, jlong handle, jint kind, jstring uid) {
  mc_delete_plan *plan = plan_of(env, handle, kind, uid);
  if (!plan) return NULL;
  jint values[] = {mc_delete_plan_tracks(plan), mc_delete_plan_files(plan)};
  mc_delete_plan_free(plan);
  return jni_ints(env, values, 2);
}

typedef mc_status (*delete_fn)(mc_library *, const mc_delete_plan *, mc_delete_result *);

/* Runs the plan for uid with run: {tracks deleted, files removed, tracks kept}. */
static jintArray delete_with(JNIEnv *env, jlong handle, jint kind, jstring uid, delete_fn run) {
  mc_delete_plan *plan = plan_of(env, handle, kind, uid);
  if (!plan) return NULL;
  mc_delete_result res;
  mc_status st = run(jni_lib_of(handle), plan, &res);
  mc_delete_plan_free(plan);
  JNI_CHECK(st, NULL);
  jint values[] = {res.tracks_deleted, res.files_trashed, res.tracks_kept};
  return jni_ints(env, values, 3);
}

/* Deletes uid for good. */
JNI_FN(jintArray, DeleteNatives, nativeDelete, jlong handle, jint kind, jstring uid) {
  return delete_with(env, handle, kind, uid, mc_delete_run_permanently);
}

/* Whether deleting uid would delete any of track_uids (a String[]), e.g. the play queue's, so
 * playback can stop first as on the desktops. */
JNI_FN(jboolean, DeleteNatives, nativeTakesAny, jlong handle, jint kind, jstring uid, jobjectArray track_uids) {
  mc_delete_plan *plan = plan_of(env, handle, kind, uid);
  if (!plan) return JNI_FALSE;
  jboolean takes = JNI_FALSE;
  jsize n = (*env)->GetArrayLength(env, track_uids);
  for (jsize i = 0; i < n && !takes; i++) {
    jstring s = (jstring)(*env)->GetObjectArrayElement(env, track_uids, i);
    if (!s) continue;
    int ok = 1;
    const char *c = jni_chars(env, s, &ok);
    if (c) takes = JNI_BOOL(mc_delete_plan_has(plan, c));
    jni_release(env, s, c);
    (*env)->DeleteLocalRef(env, s);
    if (!ok) break; /* OutOfMemoryError pending */
  }
  mc_delete_plan_free(plan);
  return takes;
}

/* Deletes uid into the recycle bin (in a group, where mc_sync_bin_offered() says so): as
 * nativeDelete, but it can be taken back until the next sync. */
JNI_FN(jintArray, DeleteNatives, nativeDeleteToBin, jlong handle, jint kind, jstring uid) {
  return delete_with(env, handle, kind, uid, mc_delete_run_to_bin);
}

static jobject new_bin_entry(JNIEnv *env, jclass cls, jmethodID ctor, const void *item) {
  const mc_bin_entry *e = item;
  jstring title = new_string(env, e->title);
  jstring artist = new_string(env, e->artist);
  jobject out = (*env)->NewObject(env, cls, ctor, (jlong)e->id, (jint)e->kind, (jint)e->items, title, artist,
                                  (jint)e->tracks, (jlong)e->deleted_ms);
  JNI_DELETE(title, artist);
  return out;
}

/* The recycle bin's entries, newest first, as a BinEntry[]. */
JNI_FN(jobjectArray, DeleteNatives, nativeBinList, jlong handle) {
  mc_bin_list *list = NULL;
  JNI_CHECK(mc_delete_bin_list(jni_lib_of(handle), &list), NULL);
  jclass entry;
  jmethodID ctor = jni_ctor(env, JNI_CLASS("BinEntry"), "(JII" J_STRING J_STRING "IJ)V", &entry);
  jobjectArray out = ctor ? JNI_OBJECT_ARRAY(entry, ctor, list->items, list->count, new_bin_entry) : NULL;
  mc_delete_bin_list_free(list);
  return out;
}

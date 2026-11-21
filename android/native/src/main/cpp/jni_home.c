/* JNI bindings for ng.kithso.core.HomeNatives: the Home page's rows (kithcore/home_picks.h). As
 * thin as it is; which rows are shown, and how, is the app's business. */
#include <jni.h>

#include "jni_bridge.h"
#include "kithcore/home_picks.h"

/* A HomePick for row's pick p, or NULL with an exception pending. */
static jobject new_pick(JNIEnv *env, jclass cls, jmethodID ctor, int row, const mc_home_pick *p) {
  jstring uid = new_string(env, p->uid);
  jstring title = new_string(env, p->title ? p->title : "");
  jstring sub = new_string(env, p->sub);
  jstring cover = new_string(env, p->cover_dir);
  jobject item =
    (*env)->NewObject(env, cls, ctor, (jint)row, (jint)p->kind, JNI_BOOL(p->smart), uid, title, sub, cover);
  JNI_DELETE(uid, title, sub, cover);
  return item;
}

/* The rows as of now, as a HomePicks of every row's picks in turn. seed is Random's. */
JNI_FN(jobject, HomeNatives, nativeLoad, jlong handle, jint seed) {
  jclass picks_cls, pick_cls;
  jmethodID picks_ctor = jni_ctor(env, JNI_CLASS("HomePicks"), "([L" JNI_CLASS("HomePick") ";ZZ)V", &picks_cls);
  if (!picks_ctor) return NULL;
  jmethodID pick_ctor =
    jni_ctor(env, JNI_CLASS("HomePick"), "(IIZ" J_STRING J_STRING J_STRING J_STRING ")V", &pick_cls);
  if (!pick_ctor) return NULL;

  mc_home_picks picks;
  mc_home_picks_load(jni_lib_of(handle), (unsigned)seed, &picks);
  int n = 0;
  for (int r = 0; r < MC_HOME_ROW_COUNT; r++) n += picks.counts[r];
  jobject out = NULL;
  jobjectArray items = (*env)->NewObjectArray(env, n, pick_cls, NULL);
  for (int r = 0, i = 0; items && r < MC_HOME_ROW_COUNT; r++) {
    for (int k = 0; k < picks.counts[r]; k++, i++) {
      jobject item = new_pick(env, pick_cls, pick_ctor, r, &picks.rows[r][k]);
      if (!item) {
        (*env)->DeleteLocalRef(env, items);
        items = NULL;
        break;
      }
      (*env)->SetObjectArrayElement(env, items, i, item);
      (*env)->DeleteLocalRef(env, item);
    }
  }
  if (items)
    out = (*env)->NewObject(env, picks_cls, picks_ctor, items, JNI_BOOL(picks.has_music), JNI_BOOL(picks.has_plays));
  mc_home_picks_free(&picks);
  return out;
}

/* JNI bindings for ng.kithso.core.HomeNatives: the Home page's rows (kithcore/home_picks.h). As
 * thin as it is; which rows are shown, and how, is the app's business. */
#include <jni.h>

#include "jni_bridge.h"
#include "kithcore/home_picks.h"

/* A HomePick for row's pick p, or NULL with an exception pending. */
static jobject new_pick(JNIEnv *env, jclass cls, jmethodID ctor, int row, const mc_home_pick *p) {
  jstring uid = new_string(env, p->uid);
  jstring seed = new_string(env, p->seed_uid[0] ? p->seed_uid : NULL);
  jstring start = new_string(env, p->start_uid[0] ? p->start_uid : NULL);
  jstring title = new_string(env, p->title ? p->title : "");
  jstring sub = new_string(env, p->sub);
  jstring cover = new_string(env, p->cover_dir);
  jstring seed_title = new_string(env, p->seed_title);
  jobject item = (*env)->NewObject(env, cls, ctor, (jint)row, (jint)p->kind, (jboolean)(p->station != 0),
                                   (jboolean)(p->smart != 0), uid, seed, start, title, sub, cover, seed_title);
  (*env)->DeleteLocalRef(env, uid);
  if (seed) (*env)->DeleteLocalRef(env, seed);
  if (start) (*env)->DeleteLocalRef(env, start);
  (*env)->DeleteLocalRef(env, title);
  if (sub) (*env)->DeleteLocalRef(env, sub);
  if (cover) (*env)->DeleteLocalRef(env, cover);
  if (seed_title) (*env)->DeleteLocalRef(env, seed_title);
  return item;
}

/* The rows as of now, as a HomePicks of every row's picks in turn. seed is Random's. */
JNIEXPORT jobject JNICALL Java_ng_kithso_core_HomeNatives_nativeLoad(JNIEnv *env, jclass cls, jlong handle,
                                                                    jint seed) {
  (void)cls;
  jclass picks_cls = (*env)->FindClass(env, "ng/kithso/core/HomePicks");
  if (!picks_cls) return NULL;
  jmethodID picks_ctor = (*env)->GetMethodID(env, picks_cls, "<init>", "([Lng/kithso/core/HomePick;ZZ)V");
  if (!picks_ctor) return NULL;
  jclass pick_cls = (*env)->FindClass(env, "ng/kithso/core/HomePick");
  if (!pick_cls) return NULL;
  jmethodID pick_ctor = (*env)->GetMethodID(env, pick_cls, "<init>",
                                            "(IIZZLjava/lang/String;Ljava/lang/String;Ljava/lang/String;"
                                            "Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
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
    out = (*env)->NewObject(env, picks_cls, picks_ctor, items, (jboolean)(picks.has_music != 0),
                            (jboolean)(picks.has_plays != 0));
  mc_home_picks_free(&picks);
  return out;
}

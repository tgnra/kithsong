/* JNI bindings for ng.kithso.core.CoreSettings: the core's settings.json (musiccore/settings.h).
 * Kept apart from jni_bridge.c; as thin as it is: marshal arguments, call musiccore, convert
 * results. Failures throw ng.kithso.core.MusicCoreException. */
#include <jni.h>
#include <stdint.h>

#include "musiccore/musiccore.h"
#include "musiccore/settings.h"

#include "jni_bridge.h"

static mc_settings *settings_of(jlong handle) {
  return (mc_settings *)(intptr_t)handle;
}

/* s's UTF-8, or NULL for a null s; *ok is cleared if it couldn't be had (OutOfMemoryError is
 * pending then). Release with release(). */
static const char *chars(JNIEnv *env, jstring s, int *ok) {
  if (!s) return NULL;
  const char *c = (*env)->GetStringUTFChars(env, s, NULL);
  if (!c) *ok = 0;
  return c;
}

static void release(JNIEnv *env, jstring s, const char *c) {
  if (c) (*env)->ReleaseStringUTFChars(env, s, c);
}

JNIEXPORT jlong JNICALL Java_ng_kithso_core_CoreSettings_nativeLoad(JNIEnv *env, jclass cls, jstring dir) {
  (void)cls;
  int ok = 1;
  const char *cdir = chars(env, dir, &ok);
  if (!ok) return 0;
  mc_settings *s = NULL;
  mc_status st = mc_settings_load(cdir ? cdir : "", &s);
  release(env, dir, cdir);
  if (st != MC_OK) {
    throw_status(env, st);
    return 0;
  }
  return (jlong)(intptr_t)s;
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CoreSettings_nativeFree(JNIEnv *env, jclass cls, jlong handle) {
  (void)env;
  (void)cls;
  mc_settings_free(settings_of(handle));
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CoreSettings_nativeSave(JNIEnv *env, jclass cls, jlong handle) {
  (void)cls;
  mc_status st = mc_settings_save(settings_of(handle));
  if (st != MC_OK) throw_status(env, st);
}

JNIEXPORT jstring JNICALL Java_ng_kithso_core_CoreSettings_nativeDeviceName(JNIEnv *env, jclass cls,
                                                                           jlong handle) {
  (void)cls;
  return new_string(env, mc_settings_device_name(settings_of(handle)));
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CoreSettings_nativeSetDeviceName(JNIEnv *env, jclass cls,
                                                                           jlong handle, jstring name) {
  (void)cls;
  int ok = 1;
  const char *cname = chars(env, name, &ok);
  if (!ok) return;
  mc_status st = mc_settings_set_device_name(settings_of(handle), cname);
  release(env, name, cname);
  if (st != MC_OK) throw_status(env, st);
}

JNIEXPORT jboolean JNICALL Java_ng_kithso_core_CoreSettings_nativeBool(JNIEnv *env, jclass cls, jlong handle,
                                                                      jstring key, jboolean def) {
  (void)cls;
  int ok = 1;
  const char *ckey = chars(env, key, &ok);
  if (!ok) return def;
  int value = mc_settings_bool(settings_of(handle), ckey ? ckey : "", def ? 1 : 0);
  release(env, key, ckey);
  return value ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CoreSettings_nativeSetBool(JNIEnv *env, jclass cls, jlong handle,
                                                                     jstring key, jboolean value) {
  (void)cls;
  int ok = 1;
  const char *ckey = chars(env, key, &ok);
  if (!ok) return;
  mc_status st = mc_settings_set_bool(settings_of(handle), ckey ? ckey : "", value ? 1 : 0);
  release(env, key, ckey);
  if (st != MC_OK) throw_status(env, st);
}

JNIEXPORT jboolean JNICALL Java_ng_kithso_core_CoreSettings_nativeShuffle(JNIEnv *env, jclass cls,
                                                                         jlong handle) {
  (void)env;
  (void)cls;
  return mc_settings_shuffle(settings_of(handle)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CoreSettings_nativeSetShuffle(JNIEnv *env, jclass cls,
                                                                        jlong handle, jboolean on) {
  (void)cls;
  mc_status st = mc_settings_set_shuffle(settings_of(handle), on ? 1 : 0);
  if (st != MC_OK) throw_status(env, st);
}

JNIEXPORT jint JNICALL Java_ng_kithso_core_CoreSettings_nativeVolume(JNIEnv *env, jclass cls, jlong handle) {
  (void)env;
  (void)cls;
  return mc_settings_volume(settings_of(handle));
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CoreSettings_nativeSetVolume(JNIEnv *env, jclass cls,
                                                                       jlong handle, jint volume) {
  (void)cls;
  mc_status st = mc_settings_set_volume(settings_of(handle), volume);
  if (st != MC_OK) throw_status(env, st);
}

JNIEXPORT jboolean JNICALL Java_ng_kithso_core_CoreSettings_nativeScrobble(JNIEnv *env, jclass cls,
                                                                          jlong handle) {
  (void)env;
  (void)cls;
  return mc_settings_scrobble(settings_of(handle)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CoreSettings_nativeSetScrobble(JNIEnv *env, jclass cls,
                                                                         jlong handle, jboolean on) {
  (void)cls;
  mc_status st = mc_settings_set_scrobble(settings_of(handle), on ? 1 : 0);
  if (st != MC_OK) throw_status(env, st);
}

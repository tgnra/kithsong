/* JNI bindings for ng.kithso.core.CoreScrobbler (mc_scrobbler; see scrobbler.h). Thin, like
 * jni_bridge.c: failures throw ng.kithso.core.MusicCoreException. The scrobbler may be used
 * from any thread, so these may be called from any thread too. */
#include <jni.h>
#include <stdint.h>

#include "musiccore/musiccore.h"

static mc_status throw_status(JNIEnv *env, mc_status st) {
  jclass cls = (*env)->FindClass(env, "ng/kithso/core/MusicCoreException");
  if (!cls) return st; /* NoClassDefFoundError is already pending */
  jmethodID ctor = (*env)->GetMethodID(env, cls, "<init>", "(I)V");
  jobject ex = ctor ? (*env)->NewObject(env, cls, ctor, (jint)st) : NULL;
  if (ex) (*env)->Throw(env, (jthrowable)ex);
  return st;
}

static mc_scrobbler *scrobbler_of(jlong handle) {
  return (mc_scrobbler *)(intptr_t)handle;
}

/* lib is a CoreLibrary's handle, which must stay open while the scrobbler exists. */
JNIEXPORT jlong JNICALL Java_ng_kithso_core_CoreScrobbler_nativeNew(JNIEnv *env, jclass cls, jlong lib,
                                                                   jstring config_dir) {
  (void)cls;
  const char *cdir = (*env)->GetStringUTFChars(env, config_dir, NULL);
  if (!cdir) return 0;
  mc_scrobbler *sc = NULL;
  mc_status st = mc_scrobbler_new((mc_library *)(intptr_t)lib, cdir, &sc);
  (*env)->ReleaseStringUTFChars(env, config_dir, cdir);
  if (st != MC_OK) throw_status(env, st);
  return (jlong)(intptr_t)sc;
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CoreScrobbler_nativeFree(JNIEnv *env, jclass cls, jlong handle) {
  (void)env;
  (void)cls;
  mc_scrobbler_free(scrobbler_of(handle));
}

/* started_ms < 0: now playing; else a play that counts, begun then (wall clock ms). */
JNIEXPORT void JNICALL Java_ng_kithso_core_CoreScrobbler_nativePost(JNIEnv *env, jclass cls, jlong handle,
                                                                  jstring track_uid, jlong started_ms) {
  (void)cls;
  const char *uid = (*env)->GetStringUTFChars(env, track_uid, NULL);
  if (!uid) return;
  mc_status st = started_ms < 0 ? mc_scrobbler_now_playing(scrobbler_of(handle), uid)
                                : mc_scrobbler_scrobble(scrobbler_of(handle), uid, (int64_t)started_ms);
  (*env)->ReleaseStringUTFChars(env, track_uid, uid);
  if (st != MC_OK) throw_status(env, st);
}

/* Blocks on the network. The page where the user approves the sign-in. */
JNIEXPORT jstring JNICALL Java_ng_kithso_core_CoreScrobbler_nativeAuthBegin(JNIEnv *env, jclass cls,
                                                                          jlong handle) {
  (void)cls;
  char *url = NULL;
  mc_status st = mc_scrobbler_auth_begin(scrobbler_of(handle), &url);
  if (st != MC_OK) {
    throw_status(env, st);
    return NULL;
  }
  jstring s = (*env)->NewStringUTF(env, url);
  mc_free(url);
  return s;
}

/* Blocks on the network. The mc_status: MC_OK once signed in, MC_ERR_AUTH while the user
 * hasn't approved yet; others are failures. Not thrown, since polling expects MC_ERR_AUTH. */
JNIEXPORT jint JNICALL Java_ng_kithso_core_CoreScrobbler_nativeAuthFinish(JNIEnv *env, jclass cls,
                                                                        jlong handle) {
  (void)env;
  (void)cls;
  return mc_scrobbler_auth_finish(scrobbler_of(handle));
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CoreScrobbler_nativeAuthCancel(JNIEnv *env, jclass cls, jlong handle) {
  (void)env;
  (void)cls;
  mc_scrobbler_auth_cancel(scrobbler_of(handle));
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CoreScrobbler_nativeSignOut(JNIEnv *env, jclass cls, jlong handle) {
  (void)cls;
  mc_status st = mc_scrobbler_sign_out(scrobbler_of(handle));
  if (st != MC_OK) throw_status(env, st);
}

/* The user name (null when signed out); out gets pending, has_key and last_error. */
JNIEXPORT jstring JNICALL Java_ng_kithso_core_CoreScrobbler_nativeStatus(JNIEnv *env, jclass cls, jlong handle,
                                                                       jlongArray out) {
  (void)cls;
  mc_scrobbler_status status;
  mc_status st = mc_scrobbler_status_get(scrobbler_of(handle), &status);
  if (st != MC_OK) {
    throw_status(env, st);
    return NULL;
  }
  jlong v[3] = {status.pending, status.has_key, status.last_error};
  (*env)->SetLongArrayRegion(env, out, 0, 3, v);
  jstring name = status.username ? (*env)->NewStringUTF(env, status.username) : NULL;
  mc_scrobbler_status_clear(&status);
  return name;
}

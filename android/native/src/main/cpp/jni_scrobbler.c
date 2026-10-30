/* JNI bindings for ng.kithso.core.CoreScrobbler (mc_scrobbler; see scrobbler.h). Thin, like
 * jni_bridge.c: failures throw ng.kithso.core.MusicCoreException. The scrobbler may be used
 * from any thread, so these may be called from any thread too. */
#include <jni.h>
#include <stdint.h>

#include "musiccore/musiccore.h"

#include "jni_bridge.h"

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

/* kind is an mc_scrobbler_service: Last.fm and Libre.fm take their own addresses (and
 * Libre.fm its placeholder key), ignoring the strings; another service takes them. */
JNIEXPORT void JNICALL Java_ng_kithso_core_CoreScrobbler_nativeSetService(JNIEnv *env, jclass cls, jlong handle,
                                                                        jint kind, jstring api_url, jstring auth_url,
                                                                        jstring api_key, jstring api_secret) {
  (void)cls;
  mc_status st;
  if (kind == MC_SCROBBLER_SERVICE_LASTFM) {
    st = mc_scrobbler_set_service(scrobbler_of(handle), NULL, NULL, NULL, NULL);
  } else if (kind == MC_SCROBBLER_SERVICE_LIBREFM) {
    st = mc_scrobbler_set_service(scrobbler_of(handle), MC_SCROBBLER_LIBREFM_API_URL, MC_SCROBBLER_LIBREFM_AUTH_URL,
                                  MC_SCROBBLER_LIBREFM_API_KEY, MC_SCROBBLER_LIBREFM_API_SECRET);
  } else {
    int ok = 1;
    const char *url = jni_chars(env, api_url, &ok), *auth = jni_chars(env, auth_url, &ok);
    const char *key = jni_chars(env, api_key, &ok), *secret = jni_chars(env, api_secret, &ok);
    st = ok ? mc_scrobbler_set_service(scrobbler_of(handle), url, auth, key, secret) : MC_OK;
    jni_release(env, api_url, url);
    jni_release(env, auth_url, auth);
    jni_release(env, api_key, key);
    jni_release(env, api_secret, secret);
    if (!ok) return; /* OutOfMemoryError is pending */
  }
  if (st != MC_OK) throw_status(env, st);
}

/* {user name (null when signed out), service name, API address, sign-in page}; out gets
 * pending, has_key, last_error and service (an mc_scrobbler_service). */
JNIEXPORT jobjectArray JNICALL Java_ng_kithso_core_CoreScrobbler_nativeStatus(JNIEnv *env, jclass cls, jlong handle,
                                                                            jlongArray out) {
  (void)cls;
  mc_scrobbler_status status;
  mc_status st = mc_scrobbler_status_get(scrobbler_of(handle), &status);
  if (st != MC_OK) {
    throw_status(env, st);
    return NULL;
  }
  jlong v[4] = {status.pending, status.has_key, status.last_error, status.service};
  (*env)->SetLongArrayRegion(env, out, 0, 4, v);
  const char *texts[] = {status.username, status.service_name, status.api_url, status.auth_url};
  jclass string = (*env)->FindClass(env, "java/lang/String");
  jobjectArray strings = string ? (*env)->NewObjectArray(env, 4, string, NULL) : NULL;
  for (int i = 0; strings && i < 4; i++) {
    jstring s = new_string(env, texts[i]);
    if (texts[i] && !s) {
      strings = NULL; /* OutOfMemoryError is pending */
      break;
    }
    (*env)->SetObjectArrayElement(env, strings, i, s);
    if (s) (*env)->DeleteLocalRef(env, s);
  }
  mc_scrobbler_status_clear(&status);
  return strings;
}

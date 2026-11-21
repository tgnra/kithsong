/* JNI bindings for ng.kithso.core.CoreScrobbler (mc_scrobbler; see scrobbler.h). Thin, like
 * jni_bridge.c: failures throw ng.kithso.core.KithCoreException. The scrobbler may be used
 * from any thread, so these may be called from any thread too. */
#include <jni.h>
#include <stdint.h>

#include "kithcore/kithcore.h"

#include "jni_bridge.h"

static mc_scrobbler *scrobbler_of(jlong handle) {
  return JNI_PTR(mc_scrobbler, handle);
}

/* lib is a CoreLibrary's handle, which must stay open while the scrobbler exists. */
JNI_FN(jlong, CoreScrobbler, nativeNew, jlong lib, jstring config_dir) {
  JNI_STR(cdir, config_dir, 0);
  mc_scrobbler *sc = NULL;
  jni_check(env, mc_scrobbler_new(jni_lib_of(lib), cdir, &sc));
  return JNI_HANDLE(sc);
}

JNI_FN(void, CoreScrobbler, nativeFree, jlong handle) {
  mc_scrobbler_free(scrobbler_of(handle));
}

/* started_ms < 0: now playing; else a play that counts, begun then (wall clock ms). */
JNI_FN(void, CoreScrobbler, nativePost, jlong handle, jstring track_uid, jlong started_ms) {
  JNI_STR(uid, track_uid);
  JNI_CHECK(started_ms < 0 ? mc_scrobbler_now_playing(scrobbler_of(handle), uid)
                           : mc_scrobbler_scrobble(scrobbler_of(handle), uid, (int64_t)started_ms));
}

/* Blocks on the network. The page where the user approves the sign-in. */
JNI_FN(jstring, CoreScrobbler, nativeAuthBegin, jlong handle) {
  char *url = NULL;
  JNI_CHECK(mc_scrobbler_auth_begin(scrobbler_of(handle), &url), NULL);
  jstring s = (*env)->NewStringUTF(env, url);
  mc_free(url);
  return s;
}

/* Blocks on the network. The mc_status: MC_OK once signed in, MC_ERR_AUTH while the user
 * hasn't approved yet; others are failures. Not thrown, since polling expects MC_ERR_AUTH. */
JNI_FN(jint, CoreScrobbler, nativeAuthFinish, jlong handle) {
  return mc_scrobbler_auth_finish(scrobbler_of(handle));
}

JNI_FN(void, CoreScrobbler, nativeAuthCancel, jlong handle) {
  mc_scrobbler_auth_cancel(scrobbler_of(handle));
}

JNI_FN(void, CoreScrobbler, nativeSignOut, jlong handle) {
  JNI_CHECK(mc_scrobbler_sign_out(scrobbler_of(handle)));
}

/* kind is an mc_scrobbler_service: Last.fm and Libre.fm take their own addresses (and
 * Libre.fm its placeholder key), ignoring the strings; another service takes them. */
JNI_FN(void, CoreScrobbler, nativeSetService, jlong handle, jint kind, jstring api_url, jstring auth_url,
       jstring api_key, jstring api_secret) {
  if (kind == MC_SCROBBLER_SERVICE_LASTFM) {
    JNI_CHECK(mc_scrobbler_set_service(scrobbler_of(handle), NULL, NULL, NULL, NULL));
  } else if (kind == MC_SCROBBLER_SERVICE_LIBREFM) {
    JNI_CHECK(mc_scrobbler_set_service(scrobbler_of(handle), MC_SCROBBLER_LIBREFM_API_URL,
                                       MC_SCROBBLER_LIBREFM_AUTH_URL, MC_SCROBBLER_LIBREFM_API_KEY,
                                       MC_SCROBBLER_LIBREFM_API_SECRET));
  } else {
    JNI_STR(url, api_url);
    JNI_STR(auth, auth_url);
    JNI_STR(key, api_key);
    JNI_STR(secret, api_secret);
    JNI_CHECK(mc_scrobbler_set_service(scrobbler_of(handle), url, auth, key, secret));
  }
}

/* {user name (null when signed out), service name, API address, sign-in page}; out gets
 * pending, has_key, last_error and service (an mc_scrobbler_service). */
JNI_FN(jobjectArray, CoreScrobbler, nativeStatus, jlong handle, jlongArray out) {
  mc_scrobbler_status status;
  JNI_CHECK(mc_scrobbler_status_get(scrobbler_of(handle), &status), NULL);
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
    JNI_DELETE(s);
  }
  mc_scrobbler_status_clear(&status);
  return strings;
}

/* JNI bindings for ng.kithso.core.CoreSettings: the core's settings.json (kithcore/settings.h).
 * Failures throw ng.kithso.core.KithCoreException. */
#include <jni.h>
#include <stdint.h>

#include "kithcore/kithcore.h"
#include "kithcore/settings.h"

#include "jni_bridge.h"

static mc_settings *settings_of(jlong handle) {
  return JNI_PTR(mc_settings, handle);
}

JNI_FN(jlong, CoreSettings, nativeLoad, jstring dir) {
  JNI_STR(cdir, dir, 0);
  mc_settings *s = NULL;
  JNI_CHECK(mc_settings_load(JNI_OR_EMPTY(cdir), &s), 0);
  return JNI_HANDLE(s);
}

JNI_FN(void, CoreSettings, nativeFree, jlong handle) {
  mc_settings_free(settings_of(handle));
}

JNI_FN(void, CoreSettings, nativeSave, jlong handle) {
  JNI_CHECK(mc_settings_save(settings_of(handle)));
}

JNI_FN(jstring, CoreSettings, nativeDeviceName, jlong handle) {
  return new_string(env, mc_settings_device_name(settings_of(handle)));
}

JNI_FN(void, CoreSettings, nativeSetDeviceName, jlong handle, jstring name) {
  JNI_STR(cname, name);
  JNI_CHECK(mc_settings_set_device_name(settings_of(handle), cname));
}

JNI_FN(jstring, CoreSettings, nativeString, jlong handle, jstring key) {
  JNI_STR(ckey, key, NULL);
  return new_string(env, mc_settings_string(settings_of(handle), ckey));
}

JNI_FN(void, CoreSettings, nativeSetString, jlong handle, jstring key, jstring value) {
  int ok = 1;
  const char *ckey = jni_chars(env, key, &ok), *cvalue = ok ? jni_chars(env, value, &ok) : NULL;
  mc_status st = ok ? mc_settings_set_string(settings_of(handle), ckey, cvalue) : MC_ERR_NOMEM;
  jni_release(env, key, ckey);
  jni_release(env, value, cvalue);
  jni_check(env, st);
}

JNI_FN(jboolean, CoreSettings, nativeBool, jlong handle, jstring key, jboolean def) {
  JNI_STR(ckey, key, def);
  return JNI_BOOL(mc_settings_bool(settings_of(handle), JNI_OR_EMPTY(ckey), def ? 1 : 0));
}

JNI_FN(void, CoreSettings, nativeSetBool, jlong handle, jstring key, jboolean value) {
  JNI_STR(ckey, key);
  JNI_CHECK(mc_settings_set_bool(settings_of(handle), JNI_OR_EMPTY(ckey), value ? 1 : 0));
}

JNI_FN(jboolean, CoreSettings, nativeShuffle, jlong handle) {
  return JNI_BOOL(mc_settings_shuffle(settings_of(handle)));
}

JNI_FN(void, CoreSettings, nativeSetShuffle, jlong handle, jboolean on) {
  JNI_CHECK(mc_settings_set_shuffle(settings_of(handle), on ? 1 : 0));
}

JNI_FN(jboolean, CoreSettings, nativeAlbumInOrder, jlong handle) {
  return JNI_BOOL(mc_settings_album_in_order(settings_of(handle)));
}

JNI_FN(void, CoreSettings, nativeSetAlbumInOrder, jlong handle, jboolean on) {
  JNI_CHECK(mc_settings_set_album_in_order(settings_of(handle), on ? 1 : 0));
}

JNI_FN(jboolean, CoreSettings, nativeQueueTracks, jlong handle) {
  return JNI_BOOL(mc_settings_queue_tracks(settings_of(handle)));
}

JNI_FN(void, CoreSettings, nativeSetQueueTracks, jlong handle, jboolean on) {
  JNI_CHECK(mc_settings_set_queue_tracks(settings_of(handle), on ? 1 : 0));
}

JNI_FN(jint, CoreSettings, nativeVolume, jlong handle) {
  return mc_settings_volume(settings_of(handle));
}

JNI_FN(void, CoreSettings, nativeSetVolume, jlong handle, jint volume) {
  JNI_CHECK(mc_settings_set_volume(settings_of(handle), volume));
}

JNI_FN(jboolean, CoreSettings, nativeScrobble, jlong handle) {
  return JNI_BOOL(mc_settings_scrobble(settings_of(handle)));
}

JNI_FN(void, CoreSettings, nativeSetScrobble, jlong handle, jboolean on) {
  JNI_CHECK(mc_settings_set_scrobble(settings_of(handle), on ? 1 : 0));
}

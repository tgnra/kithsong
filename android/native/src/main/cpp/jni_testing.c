/* JNI bindings for ng.kithso.core.CoreTesting: the profiler (musiccore/perf.h) and stress test
 * libraries (musiccore/stress.h). Kept apart from jni_bridge.c; as thin as it is: marshal
 * arguments, call musiccore, convert results. Failures throw MusicCoreException. */
#include <jni.h>
#include <stdint.h>

#include "musiccore/musiccore.h"

#include "jni_bridge.h"

JNIEXPORT jlong JNICALL Java_ng_kithso_core_CoreTesting_nowUs(JNIEnv *env, jobject self) {
  (void)env;
  (void)self;
  return (jlong)mc_perf_now_us();
}

JNIEXPORT jboolean JNICALL Java_ng_kithso_core_CoreTesting_enabled(JNIEnv *env, jobject self) {
  (void)env;
  (void)self;
  return mc_perf_enabled() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CoreTesting_setEnabled(JNIEnv *env, jobject self, jboolean on) {
  (void)env;
  (void)self;
  mc_perf_set_enabled(on);
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CoreTesting_setRefreshHz(JNIEnv *env, jobject self, jdouble hz) {
  (void)env;
  (void)self;
  mc_perf_set_refresh_hz(hz);
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CoreTesting_mark(JNIEnv *env, jobject self, jstring name) {
  (void)self;
  int ok = 1;
  const char *c = jni_chars(env, name, &ok);
  if (ok) mc_perf_mark(c);
  jni_release(env, name, c);
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CoreTesting_markAt(JNIEnv *env, jobject self, jstring name, jlong at_us) {
  (void)self;
  int ok = 1;
  const char *c = jni_chars(env, name, &ok);
  if (ok) mc_perf_mark_at(c, (uint64_t)at_us);
  jni_release(env, name, c);
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CoreTesting_note(JNIEnv *env, jobject self, jstring key, jstring value) {
  (void)self;
  int ok = 1;
  const char *k = jni_chars(env, key, &ok), *v = ok ? jni_chars(env, value, &ok) : NULL;
  if (ok) mc_perf_note(k, v);
  jni_release(env, key, k);
  jni_release(env, value, v);
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CoreTesting_frame(JNIEnv *env, jobject self, jstring series) {
  (void)self;
  int ok = 1;
  const char *c = jni_chars(env, series, &ok);
  if (ok) mc_perf_frame(c, 0);
  jni_release(env, series, c);
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CoreTesting_frameBreak(JNIEnv *env, jobject self, jstring series) {
  (void)self;
  int ok = 1;
  const char *c = jni_chars(env, series, &ok);
  if (ok) mc_perf_frame_break(c);
  jni_release(env, series, c);
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CoreTesting_autoBegin(JNIEnv *env, jobject self) {
  (void)env;
  (void)self;
  mc_perf_auto_begin();
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CoreTesting_autoEnd(JNIEnv *env, jobject self) {
  (void)env;
  (void)self;
  mc_perf_auto_end();
}

JNIEXPORT jboolean JNICALL Java_ng_kithso_core_CoreTesting_autoRunning(JNIEnv *env, jobject self) {
  (void)env;
  (void)self;
  return mc_perf_auto_running() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CoreTesting_viewBegin(JNIEnv *env, jobject self, jstring view) {
  (void)self;
  int ok = 1;
  const char *c = jni_chars(env, view, &ok);
  if (ok) mc_perf_view_begin(c);
  jni_release(env, view, c);
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CoreTesting_viewEnd(JNIEnv *env, jobject self) {
  (void)env;
  (void)self;
  mc_perf_view_end();
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CoreTesting_clear(JNIEnv *env, jobject self) {
  (void)env;
  (void)self;
  mc_perf_clear();
}

JNIEXPORT jstring JNICALL Java_ng_kithso_core_CoreTesting_report(JNIEnv *env, jobject self) {
  (void)self;
  char *report = mc_perf_report();
  jstring out = new_string(env, report);
  mc_free(report);
  return out;
}

JNIEXPORT jint JNICALL Java_ng_kithso_core_CoreTesting_presetTracks(JNIEnv *env, jobject self, jint preset) {
  (void)env;
  (void)self;
  return mc_stress_preset_tracks((mc_stress_preset)preset);
}

JNIEXPORT jstring JNICALL Java_ng_kithso_core_CoreTesting_stressDir(JNIEnv *env, jobject self, jstring config_dir,
                                                                   jint preset) {
  (void)self;
  int ok = 1;
  const char *c = jni_chars(env, config_dir, &ok);
  if (!ok) return NULL;
  char *dir = mc_stress_dir(c, (mc_stress_preset)preset);
  jni_release(env, config_dir, c);
  jstring out = new_string(env, dir);
  mc_free(dir);
  return out;
}

JNIEXPORT jboolean JNICALL Java_ng_kithso_core_CoreTesting_isComplete(JNIEnv *env, jobject self, jstring dir) {
  (void)self;
  int ok = 1;
  const char *c = jni_chars(env, dir, &ok);
  if (!ok) return JNI_FALSE;
  int yes = mc_stress_is_complete(c);
  jni_release(env, dir, c);
  return yes ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL Java_ng_kithso_core_CoreTesting_count(JNIEnv *env, jobject self, jstring config_dir) {
  (void)self;
  int ok = 1;
  const char *c = jni_chars(env, config_dir, &ok);
  if (!ok) return 0;
  int n = mc_stress_count(c);
  jni_release(env, config_dir, c);
  return n;
}

JNIEXPORT jlong JNICALL Java_ng_kithso_core_CoreTesting_start(JNIEnv *env, jobject self, jstring dir, jint preset) {
  (void)self;
  int ok = 1;
  const char *c = jni_chars(env, dir, &ok);
  if (!ok) return 0;
  mc_stress_job *job = NULL;
  mc_status st = mc_stress_start(c, (mc_stress_preset)preset, &job);
  jni_release(env, dir, c);
  if (st != MC_OK) {
    throw_status(env, st);
    return 0;
  }
  return (jlong)(intptr_t)job;
}

JNIEXPORT jint JNICALL Java_ng_kithso_core_CoreTesting_jobPercent(JNIEnv *env, jobject self, jlong job) {
  (void)env;
  (void)self;
  return mc_stress_job_percent((mc_stress_job *)(intptr_t)job);
}

JNIEXPORT jboolean JNICALL Java_ng_kithso_core_CoreTesting_jobDone(JNIEnv *env, jobject self, jlong job) {
  (void)env;
  (void)self;
  return mc_stress_job_done((mc_stress_job *)(intptr_t)job, NULL) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL Java_ng_kithso_core_CoreTesting_jobStatus(JNIEnv *env, jobject self, jlong job) {
  (void)env;
  (void)self;
  mc_status st = MC_OK;
  mc_stress_job_done((mc_stress_job *)(intptr_t)job, &st);
  return (jint)st;
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CoreTesting_jobCancel(JNIEnv *env, jobject self, jlong job) {
  (void)env;
  (void)self;
  mc_stress_job_cancel((mc_stress_job *)(intptr_t)job);
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CoreTesting_jobFree(JNIEnv *env, jobject self, jlong job) {
  (void)env;
  (void)self;
  mc_stress_job_free((mc_stress_job *)(intptr_t)job);
}

JNIEXPORT jint JNICALL Java_ng_kithso_core_CoreTesting_deleteAll(JNIEnv *env, jobject self, jstring config_dir,
                                                                jstring open_dir) {
  (void)self;
  int ok = 1;
  const char *c = jni_chars(env, config_dir, &ok), *o = ok ? jni_chars(env, open_dir, &ok) : NULL;
  int deleted = 0;
  mc_status st = ok ? mc_stress_delete_all(c, o, &deleted) : MC_ERR_NOMEM;
  jni_release(env, config_dir, c);
  jni_release(env, open_dir, o);
  if (st != MC_OK) throw_status(env, st);
  return deleted;
}

JNIEXPORT jstring JNICALL Java_ng_kithso_core_CoreLibrary_nativeBiggestAlbum(JNIEnv *env, jclass cls, jlong handle) {
  (void)cls;
  char uid[MC_UID_STRLEN];
  return mc_library_biggest_album(jni_lib_of(handle), uid) == MC_OK ? new_string(env, uid) : NULL;
}

JNIEXPORT jboolean JNICALL Java_ng_kithso_core_CoreLibrary_nativeIsStress(JNIEnv *env, jclass cls, jlong handle) {
  (void)env;
  (void)cls;
  return mc_library_is_stress(jni_lib_of(handle)) ? JNI_TRUE : JNI_FALSE;
}

/* JNI bindings for ng.kithso.core.CoreTesting: the profiler (kithcore/perf.h) and stress test
 * libraries (kithcore/stress.h). Failures throw KithCoreException. */
#include <jni.h>
#include <stdint.h>

#include "kithcore/kithcore.h"

#include "jni_bridge.h"

JNI_OBJ_FN_BARE(jlong, CoreTesting, nowUs) {
  return (jlong)mc_perf_now_us();
}

JNI_OBJ_FN_BARE(jboolean, CoreTesting, enabled) {
  return JNI_BOOL(mc_perf_enabled());
}

JNI_OBJ_FN(void, CoreTesting, setEnabled, jboolean on) {
  mc_perf_set_enabled(on);
}

JNI_OBJ_FN(void, CoreTesting, setRefreshHz, jdouble hz) {
  mc_perf_set_refresh_hz(hz);
}

JNI_OBJ_FN(void, CoreTesting, mark, jstring name) {
  JNI_STR(c, name);
  mc_perf_mark(c);
}

JNI_OBJ_FN(void, CoreTesting, markAt, jstring name, jlong at_us) {
  JNI_STR(c, name);
  mc_perf_mark_at(c, (uint64_t)at_us);
}

JNI_OBJ_FN(void, CoreTesting, note, jstring key, jstring value) {
  JNI_STR(k, key);
  JNI_STR(v, value);
  mc_perf_note(k, v);
}

JNI_OBJ_FN(void, CoreTesting, frame, jstring series) {
  JNI_STR(c, series);
  mc_perf_frame(c, 0);
}

JNI_OBJ_FN(void, CoreTesting, frameBreak, jstring series) {
  JNI_STR(c, series);
  mc_perf_frame_break(c);
}

JNI_OBJ_FN_BARE(void, CoreTesting, autoBegin) {
  mc_perf_auto_begin();
}

JNI_OBJ_FN_BARE(void, CoreTesting, autoEnd) {
  mc_perf_auto_end();
}

JNI_OBJ_FN_BARE(jboolean, CoreTesting, autoRunning) {
  return JNI_BOOL(mc_perf_auto_running());
}

JNI_OBJ_FN(void, CoreTesting, viewBegin, jstring view) {
  JNI_STR(c, view);
  mc_perf_view_begin(c);
}

JNI_OBJ_FN_BARE(void, CoreTesting, viewEnd) {
  mc_perf_view_end();
}

JNI_OBJ_FN_BARE(void, CoreTesting, clear) {
  mc_perf_clear();
}

JNI_OBJ_FN_BARE(jstring, CoreTesting, report) {
  char *report = mc_perf_report();
  jstring out = new_string(env, report);
  mc_free(report);
  return out;
}

JNI_OBJ_FN(jint, CoreTesting, presetTracks, jint preset) {
  return mc_stress_preset_tracks((mc_stress_preset)preset);
}

JNI_OBJ_FN(jstring, CoreTesting, stressDir, jstring config_dir, jint preset) {
  JNI_STR(c, config_dir, NULL);
  char *dir = mc_stress_dir(c, (mc_stress_preset)preset);
  jstring out = new_string(env, dir);
  mc_free(dir);
  return out;
}

JNI_OBJ_FN(jboolean, CoreTesting, isComplete, jstring dir) {
  JNI_STR(c, dir, JNI_FALSE);
  return JNI_BOOL(mc_stress_is_complete(c));
}

JNI_OBJ_FN(jint, CoreTesting, count, jstring config_dir) {
  JNI_STR(c, config_dir, 0);
  return mc_stress_count(c);
}

JNI_OBJ_FN(jlong, CoreTesting, start, jstring dir, jint preset) {
  JNI_STR(c, dir, 0);
  mc_stress_job *job = NULL;
  JNI_CHECK(mc_stress_start(c, (mc_stress_preset)preset, &job), 0);
  return JNI_HANDLE(job);
}

JNI_OBJ_FN(jint, CoreTesting, jobPercent, jlong job) {
  return mc_stress_job_percent(JNI_PTR(mc_stress_job, job));
}

JNI_OBJ_FN(jboolean, CoreTesting, jobDone, jlong job) {
  return JNI_BOOL(mc_stress_job_done(JNI_PTR(mc_stress_job, job), NULL));
}

JNI_OBJ_FN(jint, CoreTesting, jobStatus, jlong job) {
  mc_status st = MC_OK;
  mc_stress_job_done(JNI_PTR(mc_stress_job, job), &st);
  return (jint)st;
}

JNI_OBJ_FN(void, CoreTesting, jobCancel, jlong job) {
  mc_stress_job_cancel(JNI_PTR(mc_stress_job, job));
}

JNI_OBJ_FN(void, CoreTesting, jobFree, jlong job) {
  mc_stress_job_free(JNI_PTR(mc_stress_job, job));
}

JNI_OBJ_FN(jint, CoreTesting, deleteAll, jstring config_dir, jstring open_dir) {
  int ok = 1;
  const char *c = jni_chars(env, config_dir, &ok), *o = ok ? jni_chars(env, open_dir, &ok) : NULL;
  int deleted = 0;
  mc_status st = ok ? mc_stress_delete_all(c, o, &deleted) : MC_ERR_NOMEM;
  jni_release(env, config_dir, c);
  jni_release(env, open_dir, o);
  jni_check(env, st);
  return deleted;
}

JNI_FN(jstring, CoreLibrary, nativeBiggestAlbum, jlong handle) {
  char uid[MC_UID_STRLEN];
  return mc_library_biggest_album(jni_lib_of(handle), uid) == MC_OK ? new_string(env, uid) : NULL;
}

JNI_FN(jboolean, CoreLibrary, nativeIsStress, jlong handle) {
  return JNI_BOOL(mc_library_is_stress(jni_lib_of(handle)));
}

/* The demo library (kithcore/demo.h), for debug builds' store_demo extra. */

JNI_OBJ_FN(jint, CoreTesting, demoCreate, jstring dir, jstring staging_dir) {
  JNI_STR(d, dir, MC_ERR_NOMEM);
  JNI_STR(s, staging_dir, MC_ERR_NOMEM);
  return mc_demo_create(d, s);
}

JNI_OBJ_FN(jboolean, CoreTesting, isDemoComplete, jstring dir) {
  JNI_STR(c, dir, JNI_FALSE);
  return JNI_BOOL(mc_demo_is_complete(c));
}

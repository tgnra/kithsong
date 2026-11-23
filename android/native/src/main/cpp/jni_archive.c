/* Exporting the library as a zip and restoring one (LibraryArchive.kt, kithcore/library_archive.h). */
#include <errno.h>
#include <unistd.h>

#include "kithcore/library_archive.h"

#include "jni_bridge.h"

typedef struct archive_call {
  JNIEnv *env;
  jobject listener;
  jmethodID progress;
  int fd;
} archive_call;

static int write_fd(void *user, const void *buf, size_t len) {
  archive_call *a = user;
  const char *p = buf;
  while (len > 0) {
    ssize_t n = write(a->fd, p, len);
    if (n < 0) {
      if (errno == EINTR) continue;
      return errno ? errno : EIO;
    }
    p += n;
    len -= (size_t)n;
  }
  return 0;
}

static int report(void *user, int64_t done, int64_t total) {
  archive_call *a = user;
  if (!a->listener) return 0;
  jboolean stop = (*a->env)->CallBooleanMethod(a->env, a->listener, a->progress, (jlong)done, (jlong)total);
  if ((*a->env)->ExceptionCheck(a->env)) {
    (*a->env)->ExceptionClear(a->env);
    return 0;
  }
  return stop == JNI_TRUE;
}

static int begin(JNIEnv *env, jobject listener, archive_call *a) {
  a->env = env;
  a->listener = listener;
  if (!listener) return 1;
  jclass type = (*env)->GetObjectClass(env, listener);
  a->progress = (*env)->GetMethodID(env, type, "onProgress", "(JJ)Z");
  (*env)->DeleteLocalRef(env, type);
  return a->progress != NULL; /* else with an exception pending */
}

/* { status, problem } */
static jintArray outcome(JNIEnv *env, mc_status st, mc_archive_problem problem) {
  jint values[2] = {(jint)st, (jint)problem};
  return jni_ints(env, values, 2);
}

/* Writes to fd, which stays the caller's to close. */
JNI_FN(jintArray, LibraryArchive, nativeExport, jlong handle, jstring temp_dir, jint fd, jobject listener) {
  JNI_STR(temp, temp_dir, NULL);
  archive_call a = {0};
  if (!begin(env, listener, &a)) return NULL;
  a.fd = fd;
  mc_archive_problem problem = MC_ARCHIVE_OK;
  mc_status st = mc_library_export(jni_lib_of(handle), temp, write_fd, report, &a, &problem);
  return outcome(env, st, problem);
}

JNI_FN(jintArray, LibraryArchive, nativeRestore, jlong handle, jstring zip_path, jstring temp_dir, jobject listener) {
  JNI_STR(zip, zip_path, NULL);
  JNI_STR(temp, temp_dir, NULL);
  archive_call a = {0};
  if (!begin(env, listener, &a)) return NULL;
  mc_archive_problem problem = MC_ARCHIVE_OK;
  mc_status st = mc_library_restore(jni_lib_of(handle), zip, temp, report, &a, &problem);
  return outcome(env, st, problem);
}

/* { files, bytes } for an export, null for any other file. */
JNI_FN(jlongArray, LibraryArchive, nativeInfo, jstring zip_path) {
  JNI_STR(zip, zip_path, NULL);
  mc_archive_info info;
  if (mc_library_archive_info(zip, &info) != MC_OK) return NULL;
  jlong values[2] = {info.files, info.bytes};
  jlongArray out = (*env)->NewLongArray(env, 2);
  if (out) (*env)->SetLongArrayRegion(env, out, 0, 2, values);
  return out;
}

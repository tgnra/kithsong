/* JNI bindings for ng.kithso.core.EditNatives: editing tags, an album's flags and cover and a
 * single's mix, and searching for another release (musiccore/edit.h), on an open CoreLibrary's
 * handle. Kept apart from
 * jni_bridge.c; as thin as it is: marshal arguments, call musiccore, convert results. Failures
 * throw ng.kithso.core.KithCoreException, with the status to tell them apart by.
 *
 * What goes into a tag comes as a byte[] of UTF-8, not a jstring: what a jstring gives is
 * modified UTF-8, which needn't be what belongs in a file's tags where a title has an emoji. */
#include <jni.h>
#include <stdint.h>
#include <stdlib.h>

#include "kithcore/edit.h"
#include "kithcore/kithcore.h"

#include "jni_bridge.h"

/* bytes as a C string, malloc'd, or NULL with an exception pending. */
static char *text_of(JNIEnv *env, jbyteArray bytes) {
  jsize n = (*env)->GetArrayLength(env, bytes);
  char *out = malloc((size_t)n + 1);
  if (!out) {
    throw_status(env, MC_ERR_NOMEM);
    return NULL;
  }
  (*env)->GetByteArrayRegion(env, bytes, 0, n, (jbyte *)out);
  out[n] = '\0';
  return out;
}

/* A Java string for s, "" for NULL: the tag classes' strings are never null. */
static jstring tag_string(JNIEnv *env, const char *s) {
  return (*env)->NewStringUTF(env, s ? s : "");
}

/* An AlbumTags of t, or NULL with an exception pending. */
static jobject album_tags_object(JNIEnv *env, const mc_album_tags *t) {
  jclass cls = (*env)->FindClass(env, "ng/kithso/core/AlbumTags");
  jmethodID ctor = cls ? (*env)->GetMethodID(env, cls, "<init>",
                                             "(Ljava/lang/String;Ljava/lang/String;IILjava/lang/String;)V")
                       : NULL;
  if (!ctor) return NULL;
  jstring title = tag_string(env, t->title), artist = tag_string(env, t->artist), mb = tag_string(env, t->mb_albumid);
  if (!title || !artist || !mb) return NULL;
  return (*env)->NewObject(env, cls, ctor, title, artist, (jint)t->year, (jint)t->original_year, mb);
}

JNIEXPORT jobject JNICALL Java_ng_kithso_core_EditNatives_nativeAlbumTags(JNIEnv *env, jclass cls, jlong handle,
                                                                         jstring uid) {
  (void)cls;
  int ok = 1;
  const char *cuid = jni_chars(env, uid, &ok);
  if (!ok) return NULL;
  mc_album_tags t = {0};
  mc_status st = mc_album_tags_get(jni_lib_of(handle), cuid, &t);
  jni_release(env, uid, cuid);
  if (st != MC_OK) {
    throw_status(env, st);
    return NULL;
  }
  jobject out = album_tags_object(env, &t);
  mc_album_tags_clear(&t);
  return out;
}

JNIEXPORT void JNICALL Java_ng_kithso_core_EditNatives_nativeSetAlbumTags(JNIEnv *env, jclass cls, jlong handle,
                                                                         jstring uid, jbyteArray title,
                                                                         jbyteArray artist, jint year,
                                                                         jint original_year, jstring mb_albumid) {
  (void)cls;
  int ok = 1;
  const char *cuid = jni_chars(env, uid, &ok), *cmb = jni_chars(env, mb_albumid, &ok);
  char *ctitle = ok ? text_of(env, title) : NULL, *cartist = ctitle ? text_of(env, artist) : NULL;
  if (cartist) {
    mc_album_tags t = {ctitle, cartist, year, original_year, (char *)cmb};
    mc_status st = mc_album_tags_set(jni_lib_of(handle), cuid, &t);
    if (st != MC_OK) throw_status(env, st);
  }
  free(ctitle);
  free(cartist);
  jni_release(env, mb_albumid, cmb);
  jni_release(env, uid, cuid);
}

JNIEXPORT jobject JNICALL Java_ng_kithso_core_EditNatives_nativeArtistTags(JNIEnv *env, jclass cls, jlong handle,
                                                                          jstring uid) {
  (void)cls;
  int ok = 1;
  const char *cuid = jni_chars(env, uid, &ok);
  if (!ok) return NULL;
  mc_artist_tags t = {0};
  mc_status st = mc_artist_tags_get(jni_lib_of(handle), cuid, &t);
  jni_release(env, uid, cuid);
  if (st != MC_OK) {
    throw_status(env, st);
    return NULL;
  }
  jobject out = NULL;
  jclass tags = (*env)->FindClass(env, "ng/kithso/core/ArtistTags");
  jmethodID ctor = tags ? (*env)->GetMethodID(env, tags, "<init>", "(Ljava/lang/String;Ljava/lang/String;)V") : NULL;
  jstring name = ctor ? tag_string(env, t.name) : NULL, sort_name = name ? tag_string(env, t.sort_name) : NULL;
  if (sort_name) out = (*env)->NewObject(env, tags, ctor, name, sort_name);
  mc_artist_tags_clear(&t);
  return out;
}

JNIEXPORT void JNICALL Java_ng_kithso_core_EditNatives_nativeSetArtistTags(JNIEnv *env, jclass cls, jlong handle,
                                                                          jstring uid, jbyteArray name,
                                                                          jbyteArray sort_name) {
  (void)cls;
  int ok = 1;
  const char *cuid = jni_chars(env, uid, &ok);
  char *cname = ok ? text_of(env, name) : NULL, *csort = cname ? text_of(env, sort_name) : NULL;
  if (csort) {
    mc_artist_tags t = {cname, csort};
    mc_status st = mc_artist_tags_set(jni_lib_of(handle), cuid, &t);
    if (st != MC_OK) throw_status(env, st);
  }
  free(cname);
  free(csort);
  jni_release(env, uid, cuid);
}

JNIEXPORT jobject JNICALL Java_ng_kithso_core_EditNatives_nativeTrackTags(JNIEnv *env, jclass cls, jlong handle,
                                                                         jstring uid) {
  (void)cls;
  int ok = 1;
  const char *cuid = jni_chars(env, uid, &ok);
  if (!ok) return NULL;
  mc_track_tags t = {0};
  mc_status st = mc_track_tags_get(jni_lib_of(handle), cuid, &t);
  jni_release(env, uid, cuid);
  if (st != MC_OK) {
    throw_status(env, st);
    return NULL;
  }
  jobject out = NULL;
  jclass tags = (*env)->FindClass(env, "ng/kithso/core/TrackTags");
  jmethodID ctor = tags ? (*env)->GetMethodID(env, tags, "<init>",
                                              "(Ljava/lang/String;Ljava/lang/String;IIILjava/lang/String;)V")
                        : NULL;
  jstring title = ctor ? tag_string(env, t.title) : NULL, artist = title ? tag_string(env, t.artist) : NULL;
  jstring genre = artist ? tag_string(env, t.genre) : NULL;
  if (genre) out = (*env)->NewObject(env, tags, ctor, title, artist, (jint)t.track_no, (jint)t.disc_no, (jint)t.year, genre);
  mc_track_tags_clear(&t);
  return out;
}

JNIEXPORT void JNICALL Java_ng_kithso_core_EditNatives_nativeSetTrackTags(JNIEnv *env, jclass cls, jlong handle,
                                                                         jstring uid, jbyteArray title,
                                                                         jbyteArray artist, jint track_no, jint disc_no,
                                                                         jint year, jbyteArray genre) {
  (void)cls;
  int ok = 1;
  const char *cuid = jni_chars(env, uid, &ok);
  char *ctitle = ok ? text_of(env, title) : NULL, *cartist = ctitle ? text_of(env, artist) : NULL;
  char *cgenre = cartist ? text_of(env, genre) : NULL;
  if (cgenre) {
    mc_track_tags t = {ctitle, cartist, track_no, disc_no, year, cgenre};
    mc_status st = mc_track_tags_set(jni_lib_of(handle), cuid, &t);
    if (st != MC_OK) throw_status(env, st);
  }
  free(ctitle);
  free(cartist);
  free(cgenre);
  jni_release(env, uid, cuid);
}

/* The album's flags: {no_scrobble, mix, art_chosen}. */
JNIEXPORT jintArray JNICALL Java_ng_kithso_core_EditNatives_nativeAlbumFlags(JNIEnv *env, jclass cls, jlong handle,
                                                                            jstring uid) {
  (void)cls;
  int ok = 1;
  const char *cuid = jni_chars(env, uid, &ok);
  if (!ok) return NULL;
  mc_album_flags flags = {0};
  mc_status st = mc_album_flags_get(jni_lib_of(handle), cuid, &flags);
  jni_release(env, uid, cuid);
  if (st != MC_OK) {
    throw_status(env, st);
    return NULL;
  }
  jint values[] = {flags.no_scrobble, flags.mix, flags.art_chosen};
  return jni_ints(env, values, 3);
}

JNIEXPORT void JNICALL Java_ng_kithso_core_EditNatives_nativeSetAlbumFlags(JNIEnv *env, jclass cls, jlong handle,
                                                                          jstring uid, jboolean no_scrobble,
                                                                          jboolean mix) {
  (void)cls;
  int ok = 1;
  const char *cuid = jni_chars(env, uid, &ok);
  if (!ok) return;
  mc_album_flags flags = {no_scrobble ? 1 : 0, mix ? 1 : 0};
  mc_status st = mc_album_flags_set(jni_lib_of(handle), cuid, &flags);
  jni_release(env, uid, cuid);
  if (st != MC_OK) throw_status(env, st);
}

JNIEXPORT void JNICALL Java_ng_kithso_core_EditNatives_nativeResetAlbumArt(JNIEnv *env, jclass cls, jlong handle,
                                                                          jstring uid) {
  (void)cls;
  int ok = 1;
  const char *cuid = jni_chars(env, uid, &ok);
  if (!ok) return;
  mc_status st = mc_album_art_reset(jni_lib_of(handle), cuid);
  jni_release(env, uid, cuid);
  if (st != MC_OK) throw_status(env, st);
}

/* The other artist called name, or null. */
JNIEXPORT jstring JNICALL Java_ng_kithso_core_EditNatives_nativeArtistSameName(JNIEnv *env, jclass cls, jlong handle,
                                                                              jstring uid, jbyteArray name) {
  (void)cls;
  int ok = 1;
  const char *cuid = jni_chars(env, uid, &ok);
  char *cname = ok ? text_of(env, name) : NULL;
  jstring out = NULL;
  if (cname) {
    char other[MC_UID_STRLEN];
    mc_status st = mc_artist_same_name(jni_lib_of(handle), cuid, cname, other);
    if (st == MC_OK) out = (*env)->NewStringUTF(env, other);
    else if (st != MC_ERR_NOT_FOUND) throw_status(env, st);
  }
  free(cname);
  jni_release(env, uid, cuid);
  return out;
}

JNIEXPORT void JNICALL Java_ng_kithso_core_EditNatives_nativeMergeArtists(JNIEnv *env, jclass cls, jlong handle,
                                                                         jstring uid, jstring other) {
  (void)cls;
  int ok = 1;
  const char *cuid = jni_chars(env, uid, &ok);
  const char *cother = ok ? jni_chars(env, other, &ok) : NULL;
  if (ok) {
    mc_status st = mc_artist_merge(jni_lib_of(handle), cuid, cother);
    if (st != MC_OK) throw_status(env, st);
  }
  if (cother) jni_release(env, other, cother);
  jni_release(env, uid, cuid);
}

/* A single's mix as {can_set, mix}. */
JNIEXPORT jintArray JNICALL Java_ng_kithso_core_EditNatives_nativeTrackMix(JNIEnv *env, jclass cls, jlong handle,
                                                                          jstring uid) {
  (void)cls;
  int ok = 1;
  const char *cuid = jni_chars(env, uid, &ok);
  if (!ok) return NULL;
  mc_track_mix mix = {0};
  mc_status st = mc_track_mix_get(jni_lib_of(handle), cuid, &mix);
  jni_release(env, uid, cuid);
  if (st != MC_OK) {
    throw_status(env, st);
    return NULL;
  }
  jint values[] = {mix.can_set, mix.mix};
  return jni_ints(env, values, 2);
}

JNIEXPORT void JNICALL Java_ng_kithso_core_EditNatives_nativeSetTrackMix(JNIEnv *env, jclass cls, jlong handle,
                                                                        jstring uid, jboolean mix) {
  (void)cls;
  int ok = 1;
  const char *cuid = jni_chars(env, uid, &ok);
  if (!ok) return;
  mc_status st = mc_track_mix_set(jni_lib_of(handle), cuid, mix ? 1 : 0);
  jni_release(env, uid, cuid);
  if (st != MC_OK) throw_status(env, st);
}

/* The album's cover here, null if it has none; throws MC_ERR_NOT_FOUND if it has no file here. */
JNIEXPORT jstring JNICALL Java_ng_kithso_core_EditNatives_nativeAlbumArt(JNIEnv *env, jclass cls, jlong handle,
                                                                        jstring uid) {
  (void)cls;
  int ok = 1;
  const char *cuid = jni_chars(env, uid, &ok);
  if (!ok) return NULL;
  char *path = NULL;
  mc_status st = mc_album_art_get(jni_lib_of(handle), cuid, &path);
  jni_release(env, uid, cuid);
  if (st != MC_OK) {
    throw_status(env, st);
    return NULL;
  }
  jstring out = new_string(env, path);
  mc_free(path);
  return out;
}

/* image's bytes, malloc'd, or NULL with an exception pending. An empty one is still a pointer. */
static void *image_of(JNIEnv *env, jbyteArray image, size_t *len) {
  jsize n = (*env)->GetArrayLength(env, image);
  void *out = malloc(n ? (size_t)n : 1);
  if (!out) {
    throw_status(env, MC_ERR_NOMEM);
    return NULL;
  }
  (*env)->GetByteArrayRegion(env, image, 0, n, (jbyte *)out);
  *len = (size_t)n;
  return out;
}

JNIEXPORT void JNICALL Java_ng_kithso_core_EditNatives_nativeCheckAlbumArt(JNIEnv *env, jclass cls, jbyteArray image) {
  (void)cls;
  size_t len = 0;
  void *bytes = image_of(env, image, &len);
  if (!bytes) return;
  mc_status st = mc_album_art_check(bytes, len);
  free(bytes);
  if (st != MC_OK) throw_status(env, st);
}

JNIEXPORT void JNICALL Java_ng_kithso_core_EditNatives_nativeSetAlbumArt(JNIEnv *env, jclass cls, jlong handle,
                                                                        jstring uid, jbyteArray image) {
  (void)cls;
  int ok = 1;
  const char *cuid = jni_chars(env, uid, &ok);
  if (!ok) return;
  size_t len = 0;
  void *bytes = image_of(env, image, &len);
  if (bytes) {
    mc_status st = mc_album_art_set(jni_lib_of(handle), cuid, bytes, len);
    if (st != MC_OK) throw_status(env, st);
  }
  free(bytes);
  jni_release(env, uid, cuid);
}

/* The releases found, best fit first, as a ReleaseInfo[]. Waits for MusicBrainz. */
JNIEXPORT jobjectArray JNICALL Java_ng_kithso_core_EditNatives_nativeReleaseSearch(JNIEnv *env, jclass cls, jlong handle,
                                                                                 jstring uid, jbyteArray artist,
                                                                                 jbyteArray title) {
  (void)cls;
  int ok = 1;
  const char *cuid = jni_chars(env, uid, &ok);
  char *cartist = ok ? text_of(env, artist) : NULL, *ctitle = cartist ? text_of(env, title) : NULL;
  mc_release_list *list = NULL;
  mc_status st = ctitle ? mc_release_search(jni_lib_of(handle), cuid, cartist, ctitle, &list) : MC_OK;
  int failed = !ctitle; /* an exception is pending */
  free(cartist);
  free(ctitle);
  jni_release(env, uid, cuid);
  if (failed) return NULL;
  if (st != MC_OK) {
    throw_status(env, st);
    return NULL;
  }
  jobjectArray out = NULL;
  jclass info = (*env)->FindClass(env, "ng/kithso/core/ReleaseInfo");
  jmethodID ctor = info ? (*env)->GetMethodID(env, info, "<init>",
                                              "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;ILjava/lang/String;"
                                              "Ljava/lang/String;Ljava/lang/String;ID)V")
                        : NULL;
  if (ctor) out = (*env)->NewObjectArray(env, list->count, info, NULL);
  for (int i = 0; out && i < list->count; i++) {
    const mc_release_info *r = &list->items[i];
    jstring id = new_string(env, r->mb_albumid), rel_title = tag_string(env, r->title);
    jstring rel_artist = tag_string(env, r->artist), country = new_string(env, r->country);
    jstring label = new_string(env, r->label), format = new_string(env, r->format);
    jobject item = (*env)->NewObject(env, info, ctor, id, rel_title, rel_artist, (jint)r->year, country, label, format,
                                     (jint)r->track_count, (jdouble)r->score);
    if (!item) {
      out = NULL;
      break;
    }
    (*env)->SetObjectArrayElement(env, out, i, item);
    (*env)->DeleteLocalRef(env, item);
    jstring strings[] = {id, rel_title, rel_artist, country, label, format};
    for (int k = 0; k < 6; k++)
      if (strings[k]) (*env)->DeleteLocalRef(env, strings[k]);
  }
  mc_release_list_free(list);
  return out;
}

/* What the album's tags would be on a release, as an AlbumTags. Waits for MusicBrainz. */
JNIEXPORT jobject JNICALL Java_ng_kithso_core_EditNatives_nativeReleaseTags(JNIEnv *env, jclass cls, jlong handle,
                                                                           jstring uid, jstring mb_albumid) {
  (void)cls;
  int ok = 1;
  const char *cuid = jni_chars(env, uid, &ok), *cmb = jni_chars(env, mb_albumid, &ok);
  mc_album_tags t = {0};
  mc_status st = ok ? mc_release_tags(jni_lib_of(handle), cuid, cmb, &t) : MC_OK;
  jni_release(env, mb_albumid, cmb);
  jni_release(env, uid, cuid);
  if (!ok) return NULL;
  if (st != MC_OK) {
    throw_status(env, st);
    return NULL;
  }
  jobject out = album_tags_object(env, &t);
  mc_album_tags_clear(&t);
  return out;
}

/* Why, in words (mc_status_str), for what the user is told. */
JNIEXPORT jstring JNICALL Java_ng_kithso_core_EditNatives_nativeStatusText(JNIEnv *env, jclass cls, jint status) {
  (void)cls;
  return tag_string(env, mc_status_str((mc_status)status));
}

/* mc_edit_set_watch() on an EditWatch. The core keeps the pointer, so it's kept here until the
 * watch ends. */
typedef struct jni_edit_watch {
  mc_edit_watch watch;
  JavaVM *vm;
  jobject object; /* global ref */
  jmethodID progress, cancelled;
} jni_edit_watch;

/* On the thread of the edit, the JVM's, which needn't be the one that started the watch. */
static JNIEnv *watch_env(jni_edit_watch *w) {
  JNIEnv *env = NULL;
  return (*w->vm)->GetEnv(w->vm, (void **)&env, JNI_VERSION_1_6) == JNI_OK ? env : NULL;
}

static void watch_progress(void *user, int done, int total) {
  jni_edit_watch *w = user;
  JNIEnv *env = watch_env(w);
  if (!env) return;
  (*env)->CallVoidMethod(env, w->object, w->progress, (jint)done, (jint)total);
  if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
}

static int watch_cancelled(void *user) {
  jni_edit_watch *w = user;
  JNIEnv *env = watch_env(w);
  if (!env) return 0;
  jboolean stop = (*env)->CallBooleanMethod(env, w->object, w->cancelled);
  if ((*env)->ExceptionCheck(env)) {
    (*env)->ExceptionClear(env);
    return 0;
  }
  return stop == JNI_TRUE;
}

/* Returns what nativeEndWatch takes. */
JNIEXPORT jlong JNICALL Java_ng_kithso_core_EditNatives_nativeStartWatch(JNIEnv *env, jclass cls, jlong handle,
                                                                        jobject watch) {
  (void)cls;
  jclass type = (*env)->GetObjectClass(env, watch);
  jmethodID progress = (*env)->GetMethodID(env, type, "progress", "(II)V");
  jmethodID cancelled = progress ? (*env)->GetMethodID(env, type, "cancelled", "()Z") : NULL;
  if (!cancelled) return 0; /* with an exception pending */
  jni_edit_watch *w = calloc(1, sizeof *w);
  if (!w) {
    throw_status(env, MC_ERR_NOMEM);
    return 0;
  }
  (*env)->GetJavaVM(env, &w->vm);
  w->object = (*env)->NewGlobalRef(env, watch);
  w->progress = progress;
  w->cancelled = cancelled;
  w->watch.progress = watch_progress;
  w->watch.cancelled = watch_cancelled;
  w->watch.user = w;
  mc_edit_set_watch(jni_lib_of(handle), &w->watch);
  return (jlong)(intptr_t)w;
}

JNIEXPORT void JNICALL Java_ng_kithso_core_EditNatives_nativeEndWatch(JNIEnv *env, jclass cls, jlong handle,
                                                                      jlong watch) {
  (void)cls;
  jni_edit_watch *w = (jni_edit_watch *)(intptr_t)watch;
  mc_edit_set_watch(jni_lib_of(handle), NULL);
  (*env)->DeleteGlobalRef(env, w->object);
  free(w);
}

/* JNI bindings for ng.kithso.core.EditNatives: editing tags, an album's flags and cover and a
 * single's mix, and searching for another release (kithcore/edit.h), on an open CoreLibrary's
 * handle. Failures throw ng.kithso.core.KithCoreException, with the status to tell them apart.
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
  jclass cls;
  jmethodID ctor = jni_ctor(env, JNI_CLASS("AlbumTags"), "(" J_STRING J_STRING "II" J_STRING ")V", &cls);
  if (!ctor) return NULL;
  jstring title = tag_string(env, t->title), artist = tag_string(env, t->artist), mb = tag_string(env, t->mb_albumid);
  if (!title || !artist || !mb) return NULL;
  return (*env)->NewObject(env, cls, ctor, title, artist, (jint)t->year, (jint)t->original_year, mb);
}

JNI_FN(jobject, EditNatives, nativeAlbumTags, jlong handle, jstring uid) {
  JNI_STR(cuid, uid, NULL);
  mc_album_tags t = {0};
  JNI_CHECK(mc_album_tags_get(jni_lib_of(handle), cuid, &t), NULL);
  jobject out = album_tags_object(env, &t);
  mc_album_tags_clear(&t);
  return out;
}

JNI_FN(void, EditNatives, nativeSetAlbumTags, jlong handle, jstring uid, jbyteArray title, jbyteArray artist,
       jint year, jint original_year, jstring mb_albumid) {
  JNI_STR(cuid, uid);
  JNI_STR(cmb, mb_albumid);
  char *ctitle = text_of(env, title), *cartist = ctitle ? text_of(env, artist) : NULL;
  if (cartist) {
    mc_album_tags t = {ctitle, cartist, year, original_year, (char *)cmb};
    jni_check(env, mc_album_tags_set(jni_lib_of(handle), cuid, &t));
  }
  free(ctitle);
  free(cartist);
}

JNI_FN(jobject, EditNatives, nativeArtistTags, jlong handle, jstring uid) {
  JNI_STR(cuid, uid, NULL);
  mc_artist_tags t = {0};
  JNI_CHECK(mc_artist_tags_get(jni_lib_of(handle), cuid, &t), NULL);
  jobject out = NULL;
  jclass tags;
  jmethodID ctor = jni_ctor(env, JNI_CLASS("ArtistTags"), "(" J_STRING J_STRING ")V", &tags);
  jstring name = ctor ? tag_string(env, t.name) : NULL, sort_name = name ? tag_string(env, t.sort_name) : NULL;
  if (sort_name) out = (*env)->NewObject(env, tags, ctor, name, sort_name);
  mc_artist_tags_clear(&t);
  return out;
}

JNI_FN(void, EditNatives, nativeSetArtistTags, jlong handle, jstring uid, jbyteArray name, jbyteArray sort_name) {
  JNI_STR(cuid, uid);
  char *cname = text_of(env, name), *csort = cname ? text_of(env, sort_name) : NULL;
  if (csort) {
    mc_artist_tags t = {cname, csort};
    jni_check(env, mc_artist_tags_set(jni_lib_of(handle), cuid, &t));
  }
  free(cname);
  free(csort);
}

JNI_FN(jobject, EditNatives, nativeTrackTags, jlong handle, jstring uid) {
  JNI_STR(cuid, uid, NULL);
  mc_track_tags t = {0};
  JNI_CHECK(mc_track_tags_get(jni_lib_of(handle), cuid, &t), NULL);
  jobject out = NULL;
  jclass tags;
  jmethodID ctor = jni_ctor(env, JNI_CLASS("TrackTags"), "(" J_STRING J_STRING "III" J_STRING ")V", &tags);
  jstring title = ctor ? tag_string(env, t.title) : NULL, artist = title ? tag_string(env, t.artist) : NULL;
  jstring genre = artist ? tag_string(env, t.genre) : NULL;
  if (genre) out = (*env)->NewObject(env, tags, ctor, title, artist, (jint)t.track_no, (jint)t.disc_no, (jint)t.year, genre);
  mc_track_tags_clear(&t);
  return out;
}

JNI_FN(void, EditNatives, nativeSetTrackTags, jlong handle, jstring uid, jbyteArray title, jbyteArray artist,
       jint track_no, jint disc_no, jint year, jbyteArray genre) {
  JNI_STR(cuid, uid);
  char *ctitle = text_of(env, title), *cartist = ctitle ? text_of(env, artist) : NULL;
  char *cgenre = cartist ? text_of(env, genre) : NULL;
  if (cgenre) {
    mc_track_tags t = {ctitle, cartist, track_no, disc_no, year, cgenre};
    jni_check(env, mc_track_tags_set(jni_lib_of(handle), cuid, &t));
  }
  free(ctitle);
  free(cartist);
  free(cgenre);
}

/* The album's flags: {no_scrobble, mix, art_chosen}. */
JNI_FN(jintArray, EditNatives, nativeAlbumFlags, jlong handle, jstring uid) {
  JNI_STR(cuid, uid, NULL);
  mc_album_flags flags = {0};
  JNI_CHECK(mc_album_flags_get(jni_lib_of(handle), cuid, &flags), NULL);
  jint values[] = {flags.no_scrobble, flags.mix, flags.art_chosen};
  return jni_ints(env, values, 3);
}

JNI_FN(void, EditNatives, nativeSetAlbumFlags, jlong handle, jstring uid, jboolean no_scrobble, jboolean mix) {
  JNI_STR(cuid, uid);
  mc_album_flags flags = {no_scrobble ? 1 : 0, mix ? 1 : 0};
  JNI_CHECK(mc_album_flags_set(jni_lib_of(handle), cuid, &flags));
}

JNI_FN(void, EditNatives, nativeResetAlbumArt, jlong handle, jstring uid) {
  JNI_STR(cuid, uid);
  JNI_CHECK(mc_album_art_reset(jni_lib_of(handle), cuid));
}

/* The other artist called name, or null. */
JNI_FN(jstring, EditNatives, nativeArtistSameName, jlong handle, jstring uid, jbyteArray name) {
  JNI_STR(cuid, uid, NULL);
  char *cname = text_of(env, name);
  jstring out = NULL;
  if (cname) {
    char other[MC_UID_STRLEN];
    mc_status st = mc_artist_same_name(jni_lib_of(handle), cuid, cname, other);
    if (st == MC_OK) out = (*env)->NewStringUTF(env, other);
    else if (st != MC_ERR_NOT_FOUND) throw_status(env, st);
  }
  free(cname);
  return out;
}

JNI_FN(void, EditNatives, nativeMergeArtists, jlong handle, jstring uid, jstring other) {
  JNI_STR(cuid, uid);
  JNI_STR(cother, other);
  JNI_CHECK(mc_artist_merge(jni_lib_of(handle), cuid, cother));
}

/* A single's mix as {can_set, mix}. */
JNI_FN(jintArray, EditNatives, nativeTrackMix, jlong handle, jstring uid) {
  JNI_STR(cuid, uid, NULL);
  mc_track_mix mix = {0};
  JNI_CHECK(mc_track_mix_get(jni_lib_of(handle), cuid, &mix), NULL);
  jint values[] = {mix.can_set, mix.mix};
  return jni_ints(env, values, 2);
}

JNI_FN(void, EditNatives, nativeSetTrackMix, jlong handle, jstring uid, jboolean mix) {
  JNI_STR(cuid, uid);
  JNI_CHECK(mc_track_mix_set(jni_lib_of(handle), cuid, mix ? 1 : 0));
}

/* The album's cover here, null if it has none; throws MC_ERR_NOT_FOUND if it has no file here. */
JNI_FN(jstring, EditNatives, nativeAlbumArt, jlong handle, jstring uid) {
  JNI_STR(cuid, uid, NULL);
  char *path = NULL;
  JNI_CHECK(mc_album_art_get(jni_lib_of(handle), cuid, &path), NULL);
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

JNI_FN(void, EditNatives, nativeCheckAlbumArt, jbyteArray image) {
  size_t len = 0;
  void *bytes = image_of(env, image, &len);
  if (!bytes) return;
  mc_status st = mc_album_art_check(bytes, len);
  free(bytes);
  jni_check(env, st);
}

JNI_FN(void, EditNatives, nativeSetAlbumArt, jlong handle, jstring uid, jbyteArray image) {
  JNI_STR(cuid, uid);
  size_t len = 0;
  void *bytes = image_of(env, image, &len);
  if (bytes) jni_check(env, mc_album_art_set(jni_lib_of(handle), cuid, bytes, len));
  free(bytes);
}

static jobject new_release(JNIEnv *env, jclass cls, jmethodID ctor, const void *item) {
  const mc_release_info *r = item;
  jstring id = new_string(env, r->mb_albumid), title = tag_string(env, r->title);
  jstring artist = tag_string(env, r->artist), country = new_string(env, r->country);
  jstring label = new_string(env, r->label), format = new_string(env, r->format);
  jobject out = (*env)->NewObject(env, cls, ctor, id, title, artist, (jint)r->year, country, label, format,
                                  (jint)r->track_count, (jdouble)r->score);
  JNI_DELETE(id, title, artist, country, label, format);
  return out;
}

/* The releases found, best fit first, as a ReleaseInfo[]. Waits for MusicBrainz. */
JNI_FN(jobjectArray, EditNatives, nativeReleaseSearch, jlong handle, jstring uid, jbyteArray artist,
       jbyteArray title) {
  JNI_STR(cuid, uid, NULL);
  char *cartist = text_of(env, artist), *ctitle = cartist ? text_of(env, title) : NULL;
  mc_release_list *list = NULL;
  mc_status st = ctitle ? mc_release_search(jni_lib_of(handle), cuid, cartist, ctitle, &list) : MC_OK;
  int failed = !ctitle; /* an exception is pending */
  free(cartist);
  free(ctitle);
  if (failed) return NULL;
  JNI_CHECK(st, NULL);
  jclass info;
  jmethodID ctor = jni_ctor(env, JNI_CLASS("ReleaseInfo"),
                            "(" J_STRING J_STRING J_STRING "I" J_STRING J_STRING J_STRING "ID)V", &info);
  jobjectArray out = ctor ? JNI_OBJECT_ARRAY(info, ctor, list->items, list->count, new_release) : NULL;
  mc_release_list_free(list);
  return out;
}

/* What the album's tags would be on a release, as an AlbumTags. Waits for MusicBrainz. */
JNI_FN(jobject, EditNatives, nativeReleaseTags, jlong handle, jstring uid, jstring mb_albumid) {
  JNI_STR(cuid, uid, NULL);
  JNI_STR(cmb, mb_albumid, NULL);
  mc_album_tags t = {0};
  JNI_CHECK(mc_release_tags(jni_lib_of(handle), cuid, cmb, &t), NULL);
  jobject out = album_tags_object(env, &t);
  mc_album_tags_clear(&t);
  return out;
}

/* Why, in words (mc_status_str), for what the user is told. */
JNI_FN(jstring, EditNatives, nativeStatusText, jint status) {
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
JNI_FN(jlong, EditNatives, nativeStartWatch, jlong handle, jobject watch) {
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
  return JNI_HANDLE(w);
}

JNI_FN(void, EditNatives, nativeEndWatch, jlong handle, jlong watch) {
  jni_edit_watch *w = JNI_PTR(jni_edit_watch, watch);
  mc_edit_set_watch(jni_lib_of(handle), NULL);
  (*env)->DeleteGlobalRef(env, w->object);
  free(w);
}

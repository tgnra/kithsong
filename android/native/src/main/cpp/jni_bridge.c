/* JNI bindings for ng.kithso.core.NativeCore, CoreLibrary, CoreImport, CorePlayer and CoreSync
 * (CoreScrobbler's are in jni_scrobbler.c). Keep this thin: marshal arguments, call kithcore,
 * convert results. Failures throw ng.kithso.core.KithCoreException. */
#include <android/log.h>
#include <jni.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kithcore/kithcore.h"

#include "jni_bridge.h"

static JavaVM *g_vm;

/* The core's diagnostic lines, to logcat rather than stderr, which Android drops. */
static void log_line(void *user, const char *line) {
  (void)user;
  __android_log_write(ANDROID_LOG_INFO, "kithcore", line);
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
  (void)reserved;
  g_vm = vm;
  /* Before NativeCore.init() (mc_init), which loading the library precedes. */
  mc_log_set_handler(log_line, NULL);
  return JNI_VERSION_1_6;
}

mc_status throw_status(JNIEnv *env, mc_status st) {
  jclass cls = (*env)->FindClass(env, "ng/kithso/core/KithCoreException");
  if (!cls) return st; /* NoClassDefFoundError is already pending */
  jmethodID ctor = (*env)->GetMethodID(env, cls, "<init>", "(I)V");
  jobject ex = ctor ? (*env)->NewObject(env, cls, ctor, (jint)st) : NULL;
  if (ex) (*env)->Throw(env, (jthrowable)ex);
  return st;
}

jstring new_string(JNIEnv *env, const char *s) {
  return s ? (*env)->NewStringUTF(env, s) : NULL;
}

jmethodID jni_ctor(JNIEnv *env, const char *class_name, const char *sig, jclass *cls) {
  *cls = (*env)->FindClass(env, class_name);
  return *cls ? (*env)->GetMethodID(env, *cls, "<init>", sig) : NULL;
}

jobjectArray jni_object_array(JNIEnv *env, jclass cls, jmethodID ctor, const void *items, size_t size, int n,
                              jni_make_fn make) {
  jobjectArray out = (*env)->NewObjectArray(env, n, cls, NULL);
  for (int i = 0; out && i < n; i++) {
    jobject item = make(env, cls, ctor, (const char *)items + (size_t)i * size);
    if (!item) return NULL;
    (*env)->SetObjectArrayElement(env, out, i, item);
    (*env)->DeleteLocalRef(env, item);
  }
  return out;
}

jstring jni_art(JNIEnv *env, jlong handle, jstring uid, jstring cache_dir, jni_art_fn art) {
  JNI_STR(cuid, uid, NULL);
  JNI_STR(cdir, cache_dir, NULL);
  char *path = NULL;
  JNI_CHECK(art(jni_lib_of(handle), cuid, cdir, &path), NULL);
  jstring out = new_string(env, path);
  mc_free(path);
  return out;
}

JNI_FN_BARE(jstring, NativeCore, version) {
  return (*env)->NewStringUTF(env, mc_version());
}

JNI_FN_BARE(jint, NativeCore, init) {
  return mc_init();
}

/* The core's language for its own text, from a BCP 47 tag such as "fr-CA". */
JNI_FN(void, NativeCore, setLocale, jstring tag) {
  JNI_STR(ctag, tag);
  mc_strings_set_locale(ctag);
}

/* The number of albums, or a negative mc_status on failure. */
JNI_FN(jlong, NativeCore, albumCount, jstring path) {
  JNI_STR(cpath, path, MC_ERR_NOMEM);
  mc_library *lib = NULL;
  int64_t count = 0;
  mc_status st = mc_library_open(cpath, &lib);
  if (st == MC_OK) st = mc_library_count(lib, MC_ENTITY_ALBUMS, &count);
  mc_library_close(lib);
  return st == MC_OK ? (jlong)count : (jlong)st;
}

/* The cover file in an album folder (mc_coverart_find()), or null. */
JNI_FN(jstring, NativeCore, coverFile, jstring dir) {
  JNI_STR(cdir, dir, NULL);
  char *path = mc_coverart_find(cdir);
  jstring out = new_string(env, path);
  mc_free(path);
  return out;
}

/* --- CoreLibrary ------------------------------------------------------------------- */

JNI_FN(jlong, CoreLibrary, nativeOpen, jstring dir) {
  JNI_STR(cdir, dir, 0);
  mc_library *lib = NULL;
  JNI_CHECK(mc_library_open(cdir, &lib), 0);
  return JNI_HANDLE(lib);
}

JNI_FN(void, CoreLibrary, nativeClose, jlong handle) {
  mc_library_close(jni_lib_of(handle));
}

JNI_FN(jlong, CoreLibrary, nativeCount, jlong handle, jint entity) {
  int64_t count = 0;
  JNI_CHECK(mc_library_count(jni_lib_of(handle), (mc_entity)entity, &count), 0);
  return (jlong)count;
}

static jobject new_album(JNIEnv *env, jclass cls, jmethodID ctor, const void *item) {
  const mc_album_info *a = item;
  jstring uid = new_string(env, a->uid);
  jstring title = new_string(env, a->title ? a->title : "");
  jstring artist = new_string(env, a->artist);
  jstring folder = new_string(env, a->folder);
  jstring artist_uid = new_string(env, a->artist_uid[0] ? a->artist_uid : NULL);
  jobject out = (*env)->NewObject(env, cls, ctor, uid, title, artist, (jint)a->year, JNI_BOOL(a->is_singles),
                                  JNI_BOOL(a->has_art), folder, artist_uid, JNI_BOOL(a->mix));
  JNI_DELETE(uid, title, artist, folder, artist_uid);
  return out;
}

jobjectArray album_array(JNIEnv *env, mc_status st, mc_album_list *list) {
  JNI_CHECK(st, NULL);
  jclass info;
  jmethodID ctor = jni_ctor(env, JNI_CLASS("AlbumInfo"),
                            "(" J_STRING J_STRING J_STRING "IZZ" J_STRING J_STRING "Z)V", &info);
  jobjectArray out = ctor ? JNI_OBJECT_ARRAY(info, ctor, list->items, list->count, new_album) : NULL;
  mc_album_list_free(list);
  return out;
}

JNI_FN(jobjectArray, CoreLibrary, nativeListAlbums, jlong handle) {
  mc_album_list *list = NULL;
  mc_status st = mc_library_list_albums(jni_lib_of(handle), &list);
  return album_array(env, st, list);
}

/* What a smart album group with rule would hold, without saving it. */
JNI_FN(jobjectArray, CoreLibrary, nativePreviewSmartAlbums, jlong handle, jstring rule) {
  JNI_STR(crule, rule, NULL);
  mc_album_list *list = NULL;
  mc_status st = mc_smart_preview_albums(jni_lib_of(handle), crule, &list);
  return album_array(env, st, list);
}

/* A play log row, as CorePlayer.takeEnded() gave it. context_uid may be null. */
JNI_FN(void, CoreLibrary, nativeLogPlay, jlong handle, jstring track_uid, jlong started_ms, jlong listened_ms,
       jint end, jint context, jstring context_uid) {
  mc_play_event event;
  memset(&event, 0, sizeof event);
  event.started_ms = started_ms;
  event.listened_ms = listened_ms;
  event.end = (mc_play_end)end;
  event.context = (mc_play_context)context;
  JNI_STR(ctrack, track_uid);
  snprintf(event.track_uid, sizeof event.track_uid, "%s", ctrack);
  JNI_STR(cuid, context_uid);
  if (cuid) snprintf(event.context_uid, sizeof event.context_uid, "%s", cuid);
  JNI_CHECK(mc_library_log_play(jni_lib_of(handle), &event));
}

JNI_FN(void, CoreLibrary, nativeRecordPlay, jlong handle, jstring uid) {
  JNI_STR(cuid, uid);
  JNI_CHECK(mc_library_record_play(jni_lib_of(handle), cuid));
}

/* The image to show for a playlist (see mc_playlist_art()), or NULL for a placeholder. */
JNI_FN(jstring, CoreLibrary, nativePlaylistArt, jlong handle, jstring uid, jstring cache_dir) {
  return jni_art(env, handle, uid, cache_dir, mc_playlist_art);
}

/* Deletes cached pictures of playlists that have gone (see mc_playlist_art_prune()). */
JNI_FN(void, CoreLibrary, nativePlaylistArtPrune, jlong handle, jstring cache_dir) {
  JNI_STR(cdir, cache_dir);
  JNI_CHECK(mc_playlist_art_prune(jni_lib_of(handle), cdir));
}

static jobject new_track(JNIEnv *env, jclass cls, jmethodID ctor, const void *item) {
  const mc_track_info *t = item;
  jstring uid = new_string(env, t->uid);
  jstring title = new_string(env, t->title ? t->title : "");
  jstring path = new_string(env, t->path);
  jstring artist = new_string(env, t->artist);
  jstring album = new_string(env, t->album);
  jstring cover_dir = new_string(env, t->cover_dir);
  jstring artist_uid = new_string(env, t->artist_uid[0] ? t->artist_uid : NULL);
  jstring album_uid = new_string(env, t->album_uid[0] ? t->album_uid : NULL);
  jobject out = (*env)->NewObject(env, cls, ctor, uid, title, (jint)t->track_no, (jint)t->disc_no,
                                  (jint)t->duration_ms, path, artist, album, cover_dir, artist_uid, (jint)t->year,
                                  album_uid);
  JNI_DELETE(uid, title, path, artist, album, cover_dir, artist_uid, album_uid);
  return out;
}

jobjectArray track_array(JNIEnv *env, mc_status st, mc_track_list *list) {
  JNI_CHECK(st, NULL);
  jclass info;
  jmethodID ctor = jni_ctor(env, JNI_CLASS("TrackInfo"),
                            "(" J_STRING J_STRING "III" J_STRING J_STRING J_STRING J_STRING J_STRING "I" J_STRING ")V",
                            &info);
  jobjectArray out = ctor ? JNI_OBJECT_ARRAY(info, ctor, list->items, list->count, new_track) : NULL;
  mc_track_list_free(list);
  return out;
}

/* The tracks of an album (kind 0), a playlist (1) or a smart playlist (2). */
JNI_FN(jobjectArray, CoreLibrary, nativeListTracks, jlong handle, jint kind, jstring uid) {
  static mc_status (*const lists[])(mc_library *, const char *, mc_track_list **) = {
    mc_library_list_album_tracks,
    mc_library_list_playlist_tracks,
    mc_library_list_smart_tracks,
  };
  if (kind < 0 || kind >= (jint)(sizeof lists / sizeof *lists)) {
    throw_status(env, MC_ERR_INVALID_ARG);
    return NULL;
  }
  JNI_STR(cuid, uid, NULL);
  mc_track_list *list = NULL;
  mc_status st = lists[kind](jni_lib_of(handle), cuid, &list);
  return track_array(env, st, list);
}

static jobject new_playlist(JNIEnv *env, jclass cls, jmethodID ctor, const void *item) {
  const mc_playlist_info *p = item;
  jstring uid = new_string(env, p->uid);
  jstring name = new_string(env, p->name ? p->name : "");
  jobject out = (*env)->NewObject(env, cls, ctor, uid, name, (jint)p->track_count, (jint)p->playable_count);
  JNI_DELETE(uid, name);
  return out;
}

/* flags are mc_library_list_playlists()'s (MC_LIST_PLAYABLE). */
JNI_FN(jobjectArray, CoreLibrary, nativeListPlaylists, jlong handle, jint flags) {
  jclass info;
  jmethodID ctor = jni_ctor(env, JNI_CLASS("PlaylistInfo"), "(" J_STRING J_STRING "II)V", &info);
  if (!ctor) return NULL;
  mc_playlist_list *list = NULL;
  JNI_CHECK(mc_library_list_playlists(jni_lib_of(handle), (unsigned)flags, &list), NULL);
  jobjectArray out = JNI_OBJECT_ARRAY(info, ctor, list->items, list->count, new_playlist);
  mc_playlist_list_free(list);
  return out;
}

static jobject new_smart(JNIEnv *env, jclass cls, jmethodID ctor, const void *item) {
  const mc_smart_info *s = item;
  jstring uid = new_string(env, s->uid);
  jstring name = new_string(env, s->name ? s->name : "");
  jstring rule = new_string(env, s->rule ? s->rule : "");
  jobject out = (*env)->NewObject(env, cls, ctor, uid, name, JNI_BOOL(s->subject == MC_SMART_ALBUMS), rule,
                                  JNI_BOOL(s->random));
  JNI_DELETE(uid, name, rule);
  return out;
}

/* flags are mc_library_list_smart()'s (MC_LIST_PLAYABLE). */
JNI_FN(jobjectArray, CoreLibrary, nativeListSmart, jlong handle, jint flags) {
  jclass info;
  jmethodID ctor = jni_ctor(env, JNI_CLASS("SmartInfo"), "(" J_STRING J_STRING "Z" J_STRING "Z)V", &info);
  if (!ctor) return NULL;
  mc_smart_list *list = NULL;
  JNI_CHECK(mc_library_list_smart(jni_lib_of(handle), (unsigned)flags, &list), NULL);
  jobjectArray out = JNI_OBJECT_ARRAY(info, ctor, list->items, list->count, new_smart);
  mc_smart_list_free(list);
  return out;
}

/* A NamedInfo of an album group. */
static jobject new_album_group(JNIEnv *env, jclass cls, jmethodID ctor, const void *item) {
  const mc_album_group_info *g = item;
  jstring uid = new_string(env, g->uid);
  jstring name = new_string(env, g->name ? g->name : "");
  jobject out = (*env)->NewObject(env, cls, ctor, uid, name, (jint)g->album_count, (jint)g->playable_count);
  JNI_DELETE(uid, name);
  return out;
}

static jobject new_artist(JNIEnv *env, jclass cls, jmethodID ctor, const void *item) {
  const mc_artist_info *a = item;
  const char *cname = a->name ? a->name : "";
  jstring uid = new_string(env, a->uid);
  jstring name = new_string(env, cname);
  jstring sort = new_string(env, a->sort_name ? a->sort_name : cname);
  jstring cover = new_string(env, a->cover_dir);
  jobject out =
    (*env)->NewObject(env, cls, ctor, uid, name, (jint)a->album_count, (jint)a->track_count, sort, cover);
  JNI_DELETE(uid, name, sort, cover);
  return out;
}

/* An ArtistInfo[] of list's artists, or NULL with an exception pending. Frees list, which may
 * be NULL when it's empty. */
static jobjectArray artist_array(JNIEnv *env, mc_artist_list *list) {
  jclass info;
  jmethodID ctor =
    jni_ctor(env, JNI_CLASS("ArtistInfo"), "(" J_STRING J_STRING "II" J_STRING J_STRING ")V", &info);
  jobjectArray out =
    ctor ? JNI_OBJECT_ARRAY(info, ctor, list ? list->items : NULL, list ? list->count : 0, new_artist) : NULL;
  mc_artist_list_free(list);
  return out;
}

/* Every artist (mc_library_list_artists()), as ArtistInfo[] with no track counts. */
JNI_FN(jobjectArray, CoreLibrary, nativeListArtists, jlong handle) {
  mc_artist_list *list = NULL;
  JNI_CHECK(mc_library_list_artists(jni_lib_of(handle), &list), NULL);
  return artist_array(env, list);
}

/* The artists with something here (mc_library_list_artists_here()), as ArtistInfo[]. */
JNI_FN(jobjectArray, CoreLibrary, nativeListArtistsHere, jlong handle) {
  mc_artist_list *list = NULL;
  JNI_CHECK(mc_library_list_artists_here(jni_lib_of(handle), &list), NULL);
  return artist_array(env, list);
}

JNI_FN(jobjectArray, CoreLibrary, nativeListAlbumGroups, jlong handle, jint flags) {
  mc_album_group_list *list = NULL;
  JNI_CHECK(mc_library_list_album_groups(jni_lib_of(handle), (unsigned)flags, &list), NULL);
  jclass info;
  jmethodID ctor = jni_ctor(env, JNI_CLASS("NamedInfo"), "(" J_STRING J_STRING "II)V", &info);
  /* An empty list may be NULL. */
  jobjectArray out =
    ctor ? JNI_OBJECT_ARRAY(info, ctor, list ? list->items : NULL, list ? list->count : 0, new_album_group)
         : NULL;
  mc_album_group_list_free(list);
  return out;
}

/* --- CoreImport -------------------------------------------------------------------- */

typedef struct jni_import {
  mc_import *imp;
  jobject listener; /* global ref to the CoreImport, whose onEvent(int, int) gets the events */
  jmethodID on_event;
} jni_import;

/* On the import thread, which the JVM doesn't know: attach it for the call. */
static void on_import_event(void *user, mc_import_event event, int arg) {
  jni_import *p = user;
  JNIEnv *env = NULL;
  int attached = 0;
  if ((*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
    if ((*g_vm)->AttachCurrentThread(g_vm, &env, NULL) != JNI_OK) return;
    attached = 1;
  }
  (*env)->CallVoidMethod(env, p->listener, p->on_event, (jint)event, (jint)arg);
  if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
  if (attached) (*g_vm)->DetachCurrentThread(g_vm);
}

static mc_import *import_of(jlong handle) {
  return JNI_PTR(jni_import, handle)->imp;
}

/* staging_dir may be null: then zips are extracted in the system's temporary folder. */
JNI_FN(jlong, CoreImport, nativeStart, jlong library, jobjectArray paths, jboolean fetch_art,
       jboolean skip_tagging, jstring staging_dir, jobject listener) {
  jni_import *p = calloc(1, sizeof *p);
  if (!p) {
    throw_status(env, MC_ERR_NOMEM);
    return 0;
  }
  p->on_event = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, listener), "onEvent", "(II)V");
  p->listener = p->on_event ? (*env)->NewGlobalRef(env, listener) : NULL;
  if (!p->listener) {
    free(p);
    return 0;
  }

  jsize count = (*env)->GetArrayLength(env, paths);
  const char **cpaths = calloc(count ? (size_t)count : 1, sizeof *cpaths);
  jstring *jpaths = calloc(count ? (size_t)count : 1, sizeof *jpaths);
  mc_status st = cpaths && jpaths ? MC_OK : MC_ERR_NOMEM;
  jsize got = 0;
  for (; st == MC_OK && got < count; got++) {
    jpaths[got] = (*env)->GetObjectArrayElement(env, paths, got);
    cpaths[got] = jpaths[got] ? (*env)->GetStringUTFChars(env, jpaths[got], NULL) : NULL;
    if (!cpaths[got]) st = MC_ERR_INVALID_ARG;
  }
  const char *cstaging = staging_dir ? (*env)->GetStringUTFChars(env, staging_dir, NULL) : NULL;
  if (staging_dir && !cstaging) st = MC_ERR_NOMEM;
  /* mc_import_start copies the paths and the staging folder. */
  mc_import_options opts = {
      .threshold = 0,
      .fetch_art = fetch_art ? 1 : 0,
      .skip_tagging = skip_tagging ? 1 : 0,
      .staging_dir = cstaging,
  };
  if (st == MC_OK) {
    st = mc_import_start(jni_lib_of(library), cpaths, count, &opts, on_import_event, p, &p->imp);
  }
  if (cstaging) (*env)->ReleaseStringUTFChars(env, staging_dir, cstaging);
  for (jsize i = 0; i < got; i++) {
    if (cpaths[i]) (*env)->ReleaseStringUTFChars(env, jpaths[i], cpaths[i]);
    if (jpaths[i]) (*env)->DeleteLocalRef(env, jpaths[i]);
  }
  free(cpaths);
  free(jpaths);
  if (st != MC_OK) {
    (*env)->DeleteGlobalRef(env, p->listener);
    free(p);
    if (!(*env)->ExceptionCheck(env)) throw_status(env, st);
    return 0;
  }
  return JNI_HANDLE(p);
}

JNI_FN(void, CoreImport, nativeFree, jlong handle) {
  jni_import *p = JNI_PTR(jni_import, handle);
  if (!p) return;
  mc_import_free(p->imp); /* waits for the import thread, so no more events arrive */
  (*env)->DeleteGlobalRef(env, p->listener);
  free(p);
}

JNI_FN(jint, CoreImport, nativeItemCount, jlong handle) {
  return mc_import_item_count(import_of(handle));
}

/* The item's mc_import_state. */
JNI_FN(jint, CoreImport, nativeItemState, jlong handle, jint item) {
  mc_import_item *it = NULL;
  JNI_CHECK(mc_import_item_get(import_of(handle), item, &it), 0);
  jint state = (jint)it->state;
  mc_import_item_free(it);
  return state;
}

/* What to call the item: "Artist – Album" (the core's track_byline) from its tags, else its
 * folder. */
JNI_FN(jstring, CoreImport, nativeItemName, jlong handle, jint item) {
  mc_import_item *it = NULL;
  JNI_CHECK(mc_import_item_get(import_of(handle), item, &it), NULL);
  jstring name;
  if (it->artist && it->album) {
    char *text = mc_format_alloc(mc_str(MC_STR_TRACK_BYLINE), MC_S(it->artist), MC_S(it->album));
    name = new_string(env, text ? text : it->album);
    mc_free(text);
  } else {
    name = new_string(env, it->album ? it->album : it->folder ? it->folder : "");
  }
  mc_import_item_free(it);
  return name;
}

/* Why the item failed or needs review, or null. */
JNI_FN(jstring, CoreImport, nativeItemError, jlong handle, jint item) {
  mc_import_item *it = NULL;
  JNI_CHECK(mc_import_item_get(import_of(handle), item, &it), NULL);
  jstring error = new_string(env, it->error);
  mc_import_item_free(it);
  return error;
}

/* Fills out[i] with s, or "" for NULL. Returns 0 if out of memory. */
static int put_string(JNIEnv *env, jobjectArray out, jsize i, const char *s) {
  jstring j = (*env)->NewStringUTF(env, s ? s : "");
  if (!j) return 0;
  (*env)->SetObjectArrayElement(env, out, i, j);
  (*env)->DeleteLocalRef(env, j);
  return 1;
}

static int put_int(JNIEnv *env, jobjectArray out, jsize i, long long v) {
  char num[32];
  snprintf(num, sizeof num, "%lld", v);
  return put_string(env, out, i, num);
}

/* A String[n], or NULL with an exception pending. */
static jobjectArray new_strings(JNIEnv *env, jsize n) {
  jclass string_class = (*env)->FindClass(env, "java/lang/String");
  return string_class ? (*env)->NewObjectArray(env, n, string_class, NULL) : NULL;
}

/* A snapshot of the item as strings, in CoreImport.ItemInfo's order: kind, state, file count,
 * artist, album, folder, error, searched wider, outcome, chosen, match count, removed count. */
JNI_FN(jobjectArray, CoreImport, nativeItemInfo, jlong handle, jint item) {
  mc_import_item *it = NULL;
  JNI_CHECK(mc_import_item_get(import_of(handle), item, &it), NULL);
  jobjectArray out = new_strings(env, 12);
  if (out && !(put_int(env, out, 0, it->kind) && put_int(env, out, 1, it->state) &&
               put_int(env, out, 2, it->file_count) && put_string(env, out, 3, it->artist) &&
               put_string(env, out, 4, it->album) && put_string(env, out, 5, it->folder) &&
               put_string(env, out, 6, it->error) && put_int(env, out, 7, it->searched_wider) &&
               put_int(env, out, 8, it->outcome) && put_int(env, out, 9, it->chosen) &&
               put_int(env, out, 10, it->match_count) &&
               put_int(env, out, 11, it->removed_count)))
    out = NULL;
  mc_import_item_free(it);
  return out;
}

#define MATCH_FIELDS 13

/* The item's candidates, best first, flattened MATCH_FIELDS strings each, in
 * CoreImport.Match's order: album, album artist, release, year, confidence in thousandths,
 * track count, format, country, label, whether it's a single, a single's track title, release
 * type and catalogue number. */
JNI_FN(jobjectArray, CoreImport, nativeItemMatches, jlong handle, jint item) {
  mc_import_item *it = NULL;
  JNI_CHECK(mc_import_item_get(import_of(handle), item, &it), NULL);
  jobjectArray out = new_strings(env, it->match_count * MATCH_FIELDS);
  for (int i = 0; out && i < it->match_count; i++) {
    const mc_import_match *m = &it->matches[i];
    jsize at = i * MATCH_FIELDS;
    if (!(put_string(env, out, at, m->album) && put_string(env, out, at + 1, m->album_artist) &&
          put_string(env, out, at + 2, m->release) && put_int(env, out, at + 3, m->year) &&
          put_int(env, out, at + 4, (long long)(m->confidence * 1000 + 0.5)) &&
          put_int(env, out, at + 5, m->track_count) && put_string(env, out, at + 6, m->format) &&
          put_string(env, out, at + 7, m->country) && put_string(env, out, at + 8, m->label) &&
          put_int(env, out, at + 9, m->is_single) &&
          put_string(env, out, at + 10, m->is_single && m->track_count ? m->tracks[0].title : NULL) &&
          put_string(env, out, at + 11, m->release_type) && put_string(env, out, at + 12, m->catalog)))
      out = NULL;
  }
  mc_import_item_free(it);
  return out;
}

/* The calls that resolve an item needing review: 0 accept as is, 1 skip, 2 search more widely. */
JNI_FN(void, CoreImport, nativeResolve, jlong handle, jint item, jint how) {
  static mc_status (*const calls[])(mc_import *, int) = {mc_import_accept_as_is, mc_import_skip,
                                                         mc_import_search_wider};
  if (how < 0 || how >= (jint)(sizeof calls / sizeof *calls)) {
    throw_status(env, MC_ERR_INVALID_ARG);
    return;
  }
  JNI_CHECK(calls[how](import_of(handle), item));
}

JNI_FN(void, CoreImport, nativeAccept, jlong handle, jint item, jint match) {
  JNI_CHECK(mc_import_accept(import_of(handle), item, match));
}

JNI_FN(void, CoreImport, nativeSearch, jlong handle, jint item, jstring artist, jstring title) {
  JNI_STR(a, artist);
  JNI_STR(t, title);
  JNI_CHECK(mc_import_search(import_of(handle), item, a, t));
}

#define FILE_FIELDS 6

/* The item's files, or with removed its files removed from the import (mc_import_item.removed),
 * flattened FILE_FIELDS strings each, in CoreImport.ItemFile's order: path, title, artist,
 * track, disc, length in ms. */
JNI_FN(jobjectArray, CoreImport, nativeItemFiles, jlong handle, jint item, jboolean removed) {
  mc_import_item *it = NULL;
  JNI_CHECK(mc_import_item_get(import_of(handle), item, &it), NULL);
  const mc_import_file *files = removed ? it->removed : it->files;
  int count = removed ? it->removed_count : it->file_count;
  jobjectArray out = new_strings(env, count * FILE_FIELDS);
  for (int i = 0; out && i < count; i++) {
    const mc_import_file *f = &files[i];
    jsize at = i * FILE_FIELDS;
    if (!(put_string(env, out, at, f->path) && put_string(env, out, at + 1, f->title) &&
          put_string(env, out, at + 2, f->artist) && put_int(env, out, at + 3, f->track) &&
          put_int(env, out, at + 4, f->disc) && put_int(env, out, at + 5, f->duration_ms)))
      out = NULL;
  }
  mc_import_item_free(it);
  return out;
}

#define TRACK_FIELDS 7

/* The tracks of all the item's candidates, flattened TRACK_FIELDS strings each: the candidate's
 * index, then CoreImport.MatchTrack's order: title, artist, disc, number, length in ms, and the
 * index of the file on it, or -1. */
JNI_FN(jobjectArray, CoreImport, nativeItemTracks, jlong handle, jint item) {
  mc_import_item *it = NULL;
  JNI_CHECK(mc_import_item_get(import_of(handle), item, &it), NULL);
  int tracks = 0;
  for (int i = 0; i < it->match_count; i++) tracks += it->matches[i].track_count;
  jobjectArray out = new_strings(env, tracks * TRACK_FIELDS);
  jsize at = 0;
  for (int i = 0; out && i < it->match_count; i++) {
    for (int j = 0; out && j < it->matches[i].track_count; j++, at += TRACK_FIELDS) {
      const mc_import_match_track *t = &it->matches[i].tracks[j];
      if (!(put_int(env, out, at, i) && put_string(env, out, at + 1, t->title) &&
            put_string(env, out, at + 2, t->artist) && put_int(env, out, at + 3, t->disc) &&
            put_int(env, out, at + 4, t->number) && put_int(env, out, at + 5, t->length_ms) &&
            put_int(env, out, at + 6, t->file)))
        out = NULL;
    }
  }
  mc_import_item_free(it);
  return out;
}

/* Other items from the item's folder (mc_import_related). */
JNI_FN(jintArray, CoreImport, nativeRelated, jlong handle, jint item) {
  int related[64];
  int n = mc_import_related(import_of(handle), item, related, 64);
  if (n > 64) n = 64;
  return jni_ints(env, related, n);
}

JNI_FN(void, CoreImport, nativeAssign, jlong handle, jint item, jint match, jint track, jint from_item, jint file) {
  JNI_CHECK(mc_import_assign(import_of(handle), item, match, track, from_item, file));
}

JNI_FN(void, CoreImport, nativeRemove, jlong handle, jint item, jint file) {
  JNI_CHECK(mc_import_remove(import_of(handle), item, file));
}

JNI_FN(void, CoreImport, nativePutBack, jlong handle, jint item, jint removed) {
  JNI_CHECK(mc_import_put_back(import_of(handle), item, removed));
}

JNI_FN(void, CoreImport, nativeSkipTagging, jlong handle) {
  JNI_CHECK(mc_import_skip_tagging(import_of(handle)));
}

JNI_FN(void, CoreImport, nativeRetryUnreachable, jlong handle) {
  JNI_CHECK(mc_import_retry_unreachable(import_of(handle)));
}

JNI_FN(jboolean, CoreImport, nativeItemUnreachable, jlong handle, jint item) {
  mc_import_item *it = NULL;
  JNI_CHECK(mc_import_item_get(import_of(handle), item, &it), JNI_FALSE);
  jboolean unreachable = JNI_BOOL(it->unreachable);
  mc_import_item_free(it);
  return unreachable;
}

JNI_FN(jboolean, CoreImport, nativeOffline, jlong handle) {
  return JNI_BOOL(mc_import_offline(import_of(handle)));
}

/* --- CorePlayer -------------------------------------------------------------------- */

typedef struct jni_player {
  mc_player *player;
  jobject listener; /* global ref to the CorePlayer, whose onEvent(int) gets the events */
  jmethodID on_event;
} jni_player;

/* On the player's thread, which the JVM doesn't know: attach it for the call. */
static void on_player_event(void *user, mc_player_event event) {
  jni_player *p = user;
  JNIEnv *env = NULL;
  int attached = 0;
  if ((*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
    if ((*g_vm)->AttachCurrentThread(g_vm, &env, NULL) != JNI_OK) return;
    attached = 1;
  }
  (*env)->CallVoidMethod(env, p->listener, p->on_event, (jint)event);
  if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
  if (attached) (*g_vm)->DetachCurrentThread(g_vm);
}

static mc_player *player_of(jlong handle) {
  return JNI_PTR(jni_player, handle)->player;
}

mc_player *jni_player_of(jlong handle) {
  return player_of(handle);
}

JNI_FN(jlong, CorePlayer, nativeNew, jobject listener) {
  jni_player *p = calloc(1, sizeof *p);
  if (!p) {
    throw_status(env, MC_ERR_NOMEM);
    return 0;
  }
  p->on_event = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, listener), "onEvent", "(I)V");
  p->listener = p->on_event ? (*env)->NewGlobalRef(env, listener) : NULL;
  if (!p->listener) {
    free(p);
    return 0;
  }
  mc_status st = mc_player_new(on_player_event, p, &p->player);
  if (st != MC_OK) {
    (*env)->DeleteGlobalRef(env, p->listener);
    free(p);
    throw_status(env, st);
    return 0;
  }
  return JNI_HANDLE(p);
}

JNI_FN(void, CorePlayer, nativeFree, jlong handle) {
  jni_player *p = JNI_PTR(jni_player, handle);
  if (!p) return;
  mc_player_free(p->player); /* first, so no more events arrive */
  (*env)->DeleteGlobalRef(env, p->listener);
  free(p);
}

/* A String[]'s elements as C strings, for as long as it's held; none may be null. */
typedef struct string_array {
  const char **strings;
  jstring *refs;
  jsize got;
} string_array;

/* MC_ERR_INVALID_ARG for a null element, or one that couldn't be had (OutOfMemoryError is
 * pending then). Release it whatever this returns. */
static mc_status string_array_get(JNIEnv *env, jobjectArray array, jsize count, string_array *out) {
  out->strings = calloc(count ? (size_t)count : 1, sizeof *out->strings);
  out->refs = calloc(count ? (size_t)count : 1, sizeof *out->refs);
  out->got = 0;
  mc_status st = out->strings && out->refs ? MC_OK : MC_ERR_NOMEM;
  for (; st == MC_OK && out->got < count; out->got++) {
    jsize i = out->got;
    out->refs[i] = (*env)->GetObjectArrayElement(env, array, i);
    int ok = 1;
    out->strings[i] = jni_chars(env, out->refs[i], &ok);
    if (!out->strings[i]) st = MC_ERR_INVALID_ARG;
  }
  return st;
}

static void string_array_release(JNIEnv *env, string_array *a) {
  for (jsize i = 0; i < a->got; i++) {
    jni_release(env, a->refs[i], a->strings[i]);
    JNI_DELETE(a->refs[i]);
  }
  free(a->strings);
  free(a->refs);
}

/* What replaces the queue: mc_player_set_queue() and the like. */
typedef mc_status (*queue_call)(mc_player *, const char *const *, int, int, const mc_play_origin *);

/* call with these. uids go with paths, one each. */
static void set_queue(JNIEnv *env, jlong handle, jobjectArray paths, jobjectArray uids, jint context,
                      jstring context_uid, jint start, queue_call call) {
  jsize count = (*env)->GetArrayLength(env, paths);
  if ((*env)->GetArrayLength(env, uids) != count) {
    throw_status(env, MC_ERR_INVALID_ARG);
    return;
  }
  string_array cpaths, cuids;
  mc_status st = string_array_get(env, paths, count, &cpaths);
  mc_status st2 = string_array_get(env, uids, count, &cuids);
  if (st == MC_OK) st = st2;
  int ok = 1;
  const char *cctx = jni_chars(env, context_uid, &ok);
  if (!ok) st = MC_ERR_NOMEM;
  /* They copy everything. */
  if (st == MC_OK) {
    mc_play_origin origin = {cuids.strings, (mc_play_context)context, cctx};
    st = call(player_of(handle), cpaths.strings, count, start, &origin);
  }
  jni_release(env, context_uid, cctx);
  string_array_release(env, &cpaths);
  string_array_release(env, &cuids);
  if (st != MC_OK && !(*env)->ExceptionCheck(env)) throw_status(env, st);
}

JNI_FN(void, CorePlayer, nativeSetQueue, jlong handle, jobjectArray paths, jobjectArray uids, jint context,
       jstring context_uid, jint start) {
  set_queue(env, handle, paths, uids, context, context_uid, start, mc_player_set_queue);
}

JNI_FN(void, CorePlayer, nativeLoadQueue, jlong handle, jobjectArray paths, jobjectArray uids, jint context,
       jstring context_uid, jint start) {
  set_queue(env, handle, paths, uids, context, context_uid, start, mc_player_load_queue);
}

JNI_FN(void, CorePlayer, nativeRestoreQueue, jlong handle, jobjectArray paths, jobjectArray uids, jint context,
       jstring context_uid, jint start) {
  set_queue(env, handle, paths, uids, context, context_uid, start, mc_player_restore_queue);
}

/* The player calls that take no arguments: 0 toggle, 1 pause, 2 resume, 3 stop, 4 next,
 * 5 previous. */
JNI_FN(void, CorePlayer, nativeCommand, jlong handle, jint command) {
  static mc_status (*const commands[])(mc_player *) = {
    mc_player_toggle, mc_player_pause, mc_player_resume,
    mc_player_stop,   mc_player_next,  mc_player_previous,
  };
  if (command < 0 || command >= (jint)(sizeof commands / sizeof *commands)) {
    throw_status(env, MC_ERR_INVALID_ARG);
    return;
  }
  JNI_CHECK(commands[command](player_of(handle)));
}

JNI_FN(void, CorePlayer, nativeSkipTo, jlong handle, jint index) {
  JNI_CHECK(mc_player_skip_to(player_of(handle), index));
}

JNI_FN(void, CorePlayer, nativeSeekMs, jlong handle, jlong ms) {
  JNI_CHECK(mc_player_seek_ms(player_of(handle), ms));
}

JNI_FN(void, CorePlayer, nativeSetShuffle, jlong handle, jboolean on) {
  JNI_CHECK(mc_player_set_shuffle(player_of(handle), on ? 1 : 0));
}

JNI_FN(void, CorePlayer, nativeSetVolume, jlong handle, jint volume) {
  JNI_CHECK(mc_player_set_volume(player_of(handle), volume));
}

JNI_FN(void, CorePlayer, nativeSetMuted, jlong handle, jboolean muted) {
  JNI_CHECK(mc_player_set_muted(player_of(handle), muted ? 1 : 0));
}

JNI_FN(void, CorePlayer, nativeTapStart, jlong handle) {
  JNI_CHECK(mc_player_tap_start(player_of(handle)));
}

JNI_FN(void, CorePlayer, nativeTapStop, jlong handle) {
  JNI_CHECK(mc_player_tap_stop(player_of(handle)));
}

/* Fills out (interleaved 16-bit stereo) from the tap, up to its length, and rate[0] with their
 * rate; returns the frames read, or -1 with no tap started. Never waits. */
JNI_FN(jint, CorePlayer, nativeTapRead, jlong handle, jshortArray out, jintArray rate) {
  enum { MAX = 4096 };
  int16_t buf[MAX * 2];
  jsize room = (*env)->GetArrayLength(env, out) / 2;
  int frames = 0, at = 0;
  mc_status st = mc_player_tap_read(player_of(handle), buf, room < MAX ? room : MAX, &frames, &at);
  if (st != MC_OK) return -1;
  if (frames > 0) (*env)->SetShortArrayRegion(env, out, 0, frames * 2, buf);
  jint r = at;
  (*env)->SetIntArrayRegion(env, rate, 0, 1, &r);
  return frames;
}

/* The queue items at places from to from + count - 1 around the current one (mc_player_order). */
JNI_FN(jintArray, CorePlayer, nativeOrder, jlong handle, jint from, jint count) {
  if (count < 0) {
    throw_status(env, MC_ERR_INVALID_ARG);
    return NULL;
  }
  _Static_assert(sizeof(jint) == sizeof(int), "jint is int");
  int *order = malloc(count ? (size_t)count * sizeof *order : 1);
  if (!order) {
    throw_status(env, MC_ERR_NOMEM);
    return NULL;
  }
  mc_status st = mc_player_order(player_of(handle), from, count, order);
  jintArray out = st == MC_OK ? jni_ints(env, (const jint *)order, count) : NULL;
  free(order);
  jni_check(env, st);
  return out;
}

/* Queue item index's track uid; null if there's no such item, or one that isn't logged. */
JNI_FN(jstring, CorePlayer, nativeQueueUid, jlong handle, jint index) {
  char uid[MC_UID_STRLEN];
  if (mc_player_queue_uid(player_of(handle), index, uid) != MC_OK || !uid[0]) return NULL;
  return new_string(env, uid);
}

JNI_FN(void, CorePlayer, nativeQueueSetPath, jlong handle, jint index, jstring path) {
  JNI_STR(cpath, path);
  mc_player_queue_set_path(player_of(handle), index, cpath);
}

/* Fills out with state, index, count, position_ms, duration_ms, shuffle, volume, muted, up_next,
 * up_next_changes. */
JNI_FN(void, CorePlayer, nativeStatus, jlong handle, jlongArray out) {
  mc_player_status st;
  mc_player_status_get(player_of(handle), &st);
  jlong values[10] = {st.state, st.index, st.count, st.position_ms, st.duration_ms,
                      st.shuffle, st.volume, st.muted, st.up_next, st.up_next_changes};
  (*env)->SetLongArrayRegion(env, out, 0, 10, values);
}

/* Fills out with the item and start (wall clock ms) of the oldest play that counts; false if
 * there's none. */
JNI_FN(jboolean, CorePlayer, nativeTakePlayed, jlong handle, jlongArray out) {
  int index;
  int64_t started_ms;
  if (mc_player_take_played(player_of(handle), &index, &started_ms) != MC_OK) return JNI_FALSE;
  jlong values[2] = {index, started_ms};
  (*env)->SetLongArrayRegion(env, out, 0, 2, values);
  return JNI_TRUE;
}

/* out gets started, listened, end and context; strings the track uid and the context's uid
 * (null if none). */
JNI_FN(jboolean, CorePlayer, nativeTakeEnded, jlong handle, jlongArray out, jobjectArray strings) {
  mc_play_event e;
  if (mc_player_take_ended(player_of(handle), &e) != MC_OK) return JNI_FALSE;
  jlong values[4] = {e.started_ms, e.listened_ms, e.end, e.context};
  (*env)->SetLongArrayRegion(env, out, 0, 4, values);
  jstring track = (*env)->NewStringUTF(env, e.track_uid);
  jstring context = e.context_uid[0] ? (*env)->NewStringUTF(env, e.context_uid) : NULL;
  (*env)->SetObjectArrayElement(env, strings, 0, track);
  (*env)->SetObjectArrayElement(env, strings, 1, context);
  return JNI_TRUE;
}

JNI_FN(void, CorePlayer, nativeEndPlay, jlong handle) {
  JNI_CHECK(mc_player_end_play(player_of(handle)));
}

/* --- CoreSync ---------------------------------------------------------------------- */


typedef struct jni_sync {
  mc_sync *sync;
  jobject listener; /* global ref to the CoreSync, whose on* methods get the events */
  jmethodID on_peer, on_pair_code, on_pair_result, on_progress, on_sync_done;
  jmethodID on_waiting, on_transcoding, on_group_dissolved, on_version, on_outlook;
} jni_sync;

/* mc_sync_version_text() for name, as a Java string ("" when the two can sync). */
static jstring version_string(JNIEnv *env, const char *name, const mc_sync_version *v) {
  char text[512];
  mc_sync_version_text(name, v, text, sizeof text);
  return new_string(env, text);
}

/* The current core thread's JNIEnv, attaching the thread if the JVM doesn't know it, or NULL.
 * Pass *attached to detach() afterwards. */
static JNIEnv *attach(int *attached) {
  JNIEnv *env = NULL;
  *attached = 0;
  if ((*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
    if ((*g_vm)->AttachCurrentThread(g_vm, &env, NULL) != JNI_OK) return NULL;
    *attached = 1;
  }
  return env;
}

static void detach(JNIEnv *env, int attached) {
  if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
  if (attached) (*g_vm)->DetachCurrentThread(g_vm);
}

static void on_sync_peer(void *user, const char *device_uid, int online, int member) {
  jni_sync *p = user;
  int attached;
  JNIEnv *env = attach(&attached);
  if (!env) return;
  jstring uid = new_string(env, device_uid ? device_uid : "");
  (*env)->CallVoidMethod(env, p->listener, p->on_peer, uid, JNI_BOOL(online), JNI_BOOL(member));
  JNI_DELETE(uid);
  detach(env, attached);
}

static void on_sync_pair_code(void *user, const char *code, const char *requester_name,
                              int attempts_left) {
  jni_sync *p = user;
  int attached;
  JNIEnv *env = attach(&attached);
  if (!env) return;
  jstring jcode = new_string(env, code ? code : "");
  jstring name = new_string(env, requester_name ? requester_name : "");
  (*env)->CallVoidMethod(env, p->listener, p->on_pair_code, jcode, name, (jint)attempts_left);
  JNI_DELETE(jcode, name);
  detach(env, attached);
}

static void on_sync_pair_result(void *user, const mc_pair_result *r) {
  jni_sync *p = user;
  int attached;
  JNIEnv *env = attach(&attached);
  if (!env) return;
  jstring uid = new_string(env, r->device_uid ? r->device_uid : "");
  jstring name = new_string(env, r->name ? r->name : "");
  jstring archive = new_string(env, r->archive_name ? r->archive_name : "");
  jstring version = version_string(env, r->name, &r->version);
  (*env)->CallVoidMethod(env, p->listener, p->on_pair_result, uid, name, JNI_BOOL(r->initiator), (jint)r->reason,
                         (jint)r->attempts_left, (jint)r->status, JNI_BOOL(r->joined), archive,
                         (jint)r->archives, version);
  JNI_DELETE(uid, name, archive, version);
  detach(env, attached);
}

static void on_sync_waiting(void *user) {
  jni_sync *p = user;
  int attached;
  JNIEnv *env = attach(&attached);
  if (!env) return;
  (*env)->CallVoidMethod(env, p->listener, p->on_waiting);
  detach(env, attached);
}

static void on_sync_transcoding(void *user, int active) {
  jni_sync *p = user;
  int attached;
  JNIEnv *env = attach(&attached);
  if (!env) return;
  (*env)->CallVoidMethod(env, p->listener, p->on_transcoding, JNI_BOOL(active));
  detach(env, attached);
}

static void on_sync_group_dissolved(void *user, const char *device_name) {
  jni_sync *p = user;
  int attached;
  JNIEnv *env = attach(&attached);
  if (!env) return;
  jstring name = new_string(env, device_name ? device_name : "");
  (*env)->CallVoidMethod(env, p->listener, p->on_group_dissolved, name);
  JNI_DELETE(name);
  detach(env, attached);
}

static void on_sync_version(void *user, const char *device_uid) {
  jni_sync *p = user;
  int attached;
  JNIEnv *env = attach(&attached);
  if (!env) return;
  jstring uid = new_string(env, device_uid ? device_uid : "");
  (*env)->CallVoidMethod(env, p->listener, p->on_version, uid);
  JNI_DELETE(uid);
  detach(env, attached);
}

static void on_sync_outlook(void *user) {
  jni_sync *p = user;
  int attached;
  JNIEnv *env = attach(&attached);
  if (!env) return;
  (*env)->CallVoidMethod(env, p->listener, p->on_outlook);
  detach(env, attached);
}

static void on_sync_progress(void *user, const mc_sync_progress *pr) {
  jni_sync *p = user;
  int attached;
  JNIEnv *env = attach(&attached);
  if (!env) return;
  jstring uid = new_string(env, pr->device_uid ? pr->device_uid : "");
  char text[128];
  mc_sync_pace_text(pr, text, sizeof text);
  jstring pace = new_string(env, text);
  (*env)->CallVoidMethod(env, p->listener, p->on_progress, uid, (jint)pr->phase, (jlong)pr->rows_sent,
                         (jlong)pr->rows_received, (jint)pr->files_done, (jint)pr->files_total,
                         (jlong)pr->bytes_received, (jlong)pr->bytes_sent, (jint)pr->sends_done,
                         (jint)pr->sends_total, pace, JNI_BOOL(pr->user), JNI_BOOL(pr->connected));
  JNI_DELETE(uid, pace);
  detach(env, attached);
}

static void on_sync_done(void *user, const mc_sync_result *r) {
  jni_sync *p = user;
  int attached;
  JNIEnv *env = attach(&attached);
  if (!env) return;
  jstring uid = new_string(env, r->device_uid ? r->device_uid : "");
  char text[256];
  mc_sync_finished_text(r, text, sizeof text);
  jstring finished = new_string(env, text);
  (*env)->CallVoidMethod(env, p->listener, p->on_sync_done, uid, (jint)r->status, JNI_BOOL(r->library_synced),
                         JNI_BOOL(r->converged), (jlong)r->rows_sent, (jlong)r->rows_received,
                         (jint)r->files_received, (jint)r->files_missing, (jint)r->files_corrupt,
                         (jint)r->files_deleted, (jint)r->files_unknown, (jint)r->files_sent,
                         (jlong)r->bytes_received, (jlong)r->bytes_sent, (jint)r->covers_received,
                         JNI_BOOL(r->cancelled), JNI_BOOL(r->peer_cancelled), JNI_BOOL(r->user),
                         JNI_BOOL(r->connected), finished);
  JNI_DELETE(uid, finished);
  detach(env, attached);
}

static mc_sync *sync_of(jlong handle) {
  return JNI_PTR(jni_sync, handle)->sync;
}

mc_sync *jni_sync_of(jlong handle) {
  return sync_of(handle);
}

JNI_FN(jlong, CoreSync, nativeNew, jlong library, jstring config_dir, jstring device_name, jint kind,
       jobject listener) {
  jni_sync *p = calloc(1, sizeof *p);
  if (!p) {
    throw_status(env, MC_ERR_NOMEM);
    return 0;
  }
  jclass lcls = (*env)->GetObjectClass(env, listener);
  static const struct {
    const char *name, *sig;
  } methods[] = {
    {"onPeer", "(" J_STRING "ZZ)V"},
    {"onPairCode", "(" J_STRING J_STRING "I)V"},
    {"onPairResult", "(" J_STRING J_STRING "ZIIIZ" J_STRING "I" J_STRING ")V"},
    {"onProgress", "(" J_STRING "IJJIIJJII" J_STRING "ZZ)V"},
    {"onSyncDone", "(" J_STRING "IZZJJIIIIIIJJIZZZZ" J_STRING ")V"},
    {"onWaiting", "()V"},
    {"onTranscoding", "(Z)V"},
    {"onGroupDissolved", "(" J_STRING ")V"},
    {"onVersion", "(" J_STRING ")V"},
    {"onOutlook", "()V"},
  };
  jmethodID *ids[] = {&p->on_peer,        &p->on_pair_code,      &p->on_pair_result,
                      &p->on_progress,    &p->on_sync_done,      &p->on_waiting,
                      &p->on_transcoding, &p->on_group_dissolved, &p->on_version,
                      &p->on_outlook};
  int found = 1;
  for (size_t i = 0; found && i < sizeof methods / sizeof *methods; i++) {
    *ids[i] = (*env)->GetMethodID(env, lcls, methods[i].name, methods[i].sig);
    found = *ids[i] != NULL;
  }
  p->listener = found ? (*env)->NewGlobalRef(env, listener) : NULL;
  if (!p->listener) {
    free(p);
    return 0;
  }

  const char *cdir = (*env)->GetStringUTFChars(env, config_dir, NULL);
  const char *cname = cdir ? (*env)->GetStringUTFChars(env, device_name, NULL) : NULL;
  mc_status st = MC_ERR_NOMEM;
  if (cname) {
    mc_sync_events events = {
      .on_peer = on_sync_peer,
      .on_pair_code = on_sync_pair_code,
      .on_pair_result = on_sync_pair_result,
      .on_progress = on_sync_progress,
      .on_sync_done = on_sync_done,
      .on_waiting = on_sync_waiting,
      .on_problems = on_sync_waiting, /* one event: read again, both are in the same status */
      .on_transcoding = on_sync_transcoding,
      .on_group_dissolved = on_sync_group_dissolved,
      .on_version = on_sync_version,
      .on_outlook = on_sync_outlook,
    };
    /* mc_sync_new copies the strings and the events. */
    st = mc_sync_new(jni_lib_of(library), cdir, cname, (mc_device_kind)kind, &events, p, &p->sync);
  }
  if (cname) (*env)->ReleaseStringUTFChars(env, device_name, cname);
  if (cdir) (*env)->ReleaseStringUTFChars(env, config_dir, cdir);
  if (st != MC_OK) {
    (*env)->DeleteGlobalRef(env, p->listener);
    free(p);
    if (!(*env)->ExceptionCheck(env)) throw_status(env, st);
    return 0;
  }
  return JNI_HANDLE(p);
}

JNI_FN(void, CoreSync, nativeFree, jlong handle) {
  jni_sync *p = JNI_PTR(jni_sync, handle);
  if (!p) return;
  mc_sync_free(p->sync); /* stops first, so no more events arrive */
  (*env)->DeleteGlobalRef(env, p->listener);
  free(p);
}

JNI_FN(jstring, CoreSync, nativeDeviceUid, jlong handle) {
  return new_string(env, mc_sync_device_uid(sync_of(handle)));
}

JNI_FN(void, CoreSync, nativeStart, jlong handle) {
  JNI_CHECK(mc_sync_start(sync_of(handle)));
}

JNI_FN(void, CoreSync, nativeStop, jlong handle) {
  mc_sync_stop(sync_of(handle));
}

JNI_FN(jint, CoreSync, nativePort, jlong handle) {
  return mc_sync_port(sync_of(handle));
}

/* The calls that take a device uid: 0 pair request, 1 remove device, 2 sync now (the Sync
 * button), 3 background sync (rate-limited), 4 cancel. For 2 to 4, uid null is every member. */
JNI_FN(void, CoreSync, nativeWithDevice, jlong handle, jint call, jstring uid) {
  static mc_status (*const calls[])(mc_sync *, const char *) = {
    mc_sync_pair_request,
    mc_sync_remove_device,
    mc_sync_now,
    mc_sync_background,
    mc_sync_cancel,
  };
  if (call < 0 || call >= (jint)(sizeof calls / sizeof *calls)) {
    throw_status(env, MC_ERR_INVALID_ARG);
    return;
  }
  JNI_STR(cuid, uid);
  JNI_CHECK(calls[call](sync_of(handle), cuid));
}

JNI_FN(void, CoreSync, nativePairRequestAddress, jlong handle, jstring host, jint port) {
  JNI_STR(chost, host);
  JNI_CHECK(mc_sync_pair_request_address(sync_of(handle), chost, (uint16_t)port));
}

JNI_FN(void, CoreSync, nativePairEnter, jlong handle, jstring code) {
  JNI_STR(ccode, code);
  JNI_CHECK(mc_sync_pair_enter(sync_of(handle), ccode));
}

JNI_FN(void, CoreSync, nativePairCancel, jlong handle) {
  mc_sync_pair_cancel(sync_of(handle));
}

JNI_FN(void, CoreSync, nativeLeaveGroup, jlong handle) {
  JNI_CHECK(mc_sync_leave_group(sync_of(handle)));
}

JNI_FN(void, CoreSync, nativeNotifyChanged, jlong handle) {
  mc_sync_notify_changed(sync_of(handle));
}

JNI_FN(void, CoreSync, nativeSetAutomatic, jlong handle, jboolean on) {
  mc_sync_set_automatic(sync_of(handle), on != JNI_FALSE);
}

JNI_FN(void, CoreSync, nativeSetPower, jlong handle, jboolean on_power, jboolean may_transcode) {
  mc_sync_set_power(sync_of(handle), on_power != JNI_FALSE, may_transcode != JNI_FALSE);
}

/* mc_sync_get_outlook(), as a SyncOutlook with mc_sync_outlook_text(). */
JNI_FN(jobject, CoreSync, nativeOutlook, jlong handle) {
  jclass info;
  jmethodID ctor = jni_ctor(env, JNI_CLASS("SyncOutlook"), "(ZIJIJI" J_STRING ")V", &info);
  if (!ctor) return NULL;
  mc_sync_outlook o;
  JNI_CHECK(mc_sync_get_outlook(sync_of(handle), &o), NULL);
  char text[128];
  mc_sync_outlook_text(&o, text, sizeof text);
  jstring jtext = new_string(env, text);
  jobject out = (*env)->NewObject(env, info, ctor, JNI_BOOL(o.chosen), (jint)o.receive_tracks,
                                  (jlong)o.receive_bytes, (jint)o.send_tracks, (jlong)o.send_bytes,
                                  (jint)o.send_devices, jtext);
  JNI_DELETE(jtext);
  return out;
}

/* The mc_sync_lib_state. */
JNI_FN(jint, CoreSync, nativeLibraryState, jlong handle) {
  mc_sync_lib_state state = MC_SYNC_LIBRARY_NO_GROUP;
  jni_check(env, mc_sync_library_state(sync_of(handle), &state));
  return (jint)state;
}

/* A SyncDevice for d, with the texts its row and page show (MC_SYNC_TEXT_APP_OPEN: this app
 * syncs only while it's open). */
static jobject new_device(JNIEnv *env, jclass cls, jmethodID ctor, mc_sync *sync, const mc_sync_device *d) {
  char warning[1024], note[256], reach[512];
  mc_sync_device_warning(sync, d, MC_SYNC_TEXT_APP_OPEN, warning, sizeof warning);
  mc_sync_device_note(d, MC_SYNC_TEXT_APP_OPEN, 1, note, sizeof note);
  mc_sync_reach_text(sync, d->uid, d->name, 0, reach, sizeof reach);
  jstring uid = new_string(env, d->uid);
  jstring name = new_string(env, d->name ? d->name : "");
  jstring address = new_string(env, d->address);
  jstring version = version_string(env, d->name, &d->version);
  jstring jwarning = new_string(env, warning);
  jstring jnote = new_string(env, note);
  jstring jreach = new_string(env, reach);
  jobject out = NULL;
  if (uid && name && version && jwarning && jnote && jreach)
    out = (*env)->NewObject(env, cls, ctor, uid, name, (jint)d->kind, JNI_BOOL(d->is_self), JNI_BOOL(d->online),
                            JNI_BOOL(d->archive), JNI_BOOL(d->removed), address, (jint)d->port,
                            (jlong)d->last_sync_ms, (jlong)d->auto_after_ms, JNI_BOOL(d->has_status),
                            (jlong)d->verified_ms, (jlong)d->expected, (jlong)d->ok, (jlong)d->missing,
                            (jlong)d->corrupt, (jlong)d->bytes, version, jwarning, jnote, jreach, (jint)d->hold,
                            (jint)d->peer_hold, (jlong)d->peer_hold_until_ms, (jlong)d->next_auto_ms,
                            JNI_BOOL(d->low_space), (jlong)d->last_seen_ms);
  JNI_DELETE(uid, name, address, version, jwarning, jnote, jreach);
  return out;
}

JNI_FN(jobjectArray, CoreSync, nativeListDevices, jlong handle) {
  jclass info;
  jmethodID ctor = jni_ctor(env, JNI_CLASS("SyncDevice"),
                            "(" J_STRING J_STRING "IZZZZ" J_STRING "IJJZJJJJJJ" J_STRING J_STRING J_STRING J_STRING
                            "IIJJZJ)V",
                            &info);
  if (!ctor) return NULL;
  mc_sync *sync = sync_of(handle);
  mc_sync_device_list *list = NULL;
  JNI_CHECK(mc_sync_list_devices(sync, &list), NULL);
  jobjectArray out = (*env)->NewObjectArray(env, list->count, info, NULL);
  for (int i = 0; out && i < list->count; i++) {
    jobject item = new_device(env, info, ctor, sync, &list->items[i]);
    if (!item) {
      out = NULL;
    } else {
      (*env)->SetObjectArrayElement(env, out, i, item);
      JNI_DELETE(item);
    }
  }
  mc_sync_device_list_free(list);
  return out;
}

static jobject new_nearby(JNIEnv *env, jclass cls, jmethodID ctor, const void *item) {
  const mc_sync_nearby *n = item;
  jstring uid = new_string(env, n->uid);
  jstring name = new_string(env, n->name ? n->name : "");
  jstring address = new_string(env, n->address);
  jstring version = version_string(env, n->name, &n->version);
  jobject out = (*env)->NewObject(env, cls, ctor, uid, name, (jint)n->kind, JNI_BOOL(n->in_other_group), address,
                                  (jint)n->port, version);
  JNI_DELETE(uid, name, address, version);
  return out;
}

JNI_FN(jobjectArray, CoreSync, nativeListNearby, jlong handle) {
  jclass info;
  jmethodID ctor =
    jni_ctor(env, JNI_CLASS("NearbyDevice"), "(" J_STRING J_STRING "IZ" J_STRING "I" J_STRING ")V", &info);
  if (!ctor) return NULL;
  mc_sync_nearby_list *list = NULL;
  JNI_CHECK(mc_sync_list_nearby(sync_of(handle), &list), NULL);
  jobjectArray out = JNI_OBJECT_ARRAY(info, ctor, list->items, list->count, new_nearby);
  mc_sync_nearby_list_free(list);
  return out;
}

/* --- Profiles --- */

/* Fills p, zeroed, from a SyncProfile's fields as CoreSync passes them: kbps 0 for originals,
 * a budget for Auto (kbps ignored), and the items as parallel kinds and uids. Returns 0 with
 * an exception pending on failure; either way free it with profile_args_free(). */
static int profile_from_args(JNIEnv *env, mc_sync_profile *p, jstring name, jboolean archive,
                             jboolean sync_all, jint kbps, jboolean budget_enabled,
                             jlong budget_bytes, jintArray kinds, jobjectArray uids) {
  p->archive = archive != JNI_FALSE;
  p->sync_all = sync_all != JNI_FALSE;
  p->budget_enabled = budget_enabled != JNI_FALSE;
  p->format.format = p->budget_enabled ? MC_FORMAT_AUTO : kbps > 0 ? MC_FORMAT_OPUS : MC_FORMAT_ORIGINAL;
  p->format.bitrate_kbps = !p->budget_enabled && kbps > 0 ? kbps : 0;
  p->budget_bytes = budget_bytes;
  if (name) {
    JNI_STR(cname, name, 0);
    p->name = strdup(cname);
    if (!p->name) {
      throw_status(env, MC_ERR_NOMEM);
      return 0;
    }
  }
  jsize count = (*env)->GetArrayLength(env, kinds);
  if (count != (*env)->GetArrayLength(env, uids)) {
    throw_status(env, MC_ERR_INVALID_ARG);
    return 0;
  }
  if (count == 0) return 1;
  p->items = calloc((size_t)count, sizeof *p->items);
  jint *ckinds = p->items ? (*env)->GetIntArrayElements(env, kinds, NULL) : NULL;
  if (!ckinds) {
    if (!(*env)->ExceptionCheck(env)) throw_status(env, MC_ERR_NOMEM);
    return 0;
  }
  int ok = 1;
  for (jsize i = 0; ok && i < count; i++) {
    jstring uid = (jstring)(*env)->GetObjectArrayElement(env, uids, i);
    const char *cuid = jni_chars(env, uid, &ok);
    if (cuid) {
      p->items[i].kind = (mc_profile_item_kind)ckinds[i];
      snprintf(p->items[i].ref_uid, sizeof p->items[i].ref_uid, "%s", cuid);
      jni_release(env, uid, cuid);
      p->count++;
    } else {
      ok = 0;
      if (!(*env)->ExceptionCheck(env)) throw_status(env, MC_ERR_INVALID_ARG);
    }
    JNI_DELETE(uid);
  }
  (*env)->ReleaseIntArrayElements(env, kinds, ckinds, JNI_ABORT);
  return ok;
}

static void profile_args_free(mc_sync_profile *p) {
  free(p->name);
  free(p->items);
}

/* The waiting tracks as a SyncWaiting, its devices as parallel arrays. */
JNI_FN(jobject, CoreSync, nativeWaiting, jlong handle) {
  jclass info;
  jmethodID ctor = jni_ctor(env, JNI_CLASS("SyncWaiting"), "(I" J_STRINGS J_STRINGS "[I[I)V", &info);
  jclass string_cls = ctor ? (*env)->FindClass(env, "java/lang/String") : NULL;
  if (!string_cls) return NULL;
  mc_sync_waiting *w = NULL;
  JNI_CHECK(mc_sync_get_waiting(sync_of(handle), &w), NULL);
  jobject out = NULL;
  jobjectArray uids = (*env)->NewObjectArray(env, w->count, string_cls, NULL);
  jobjectArray names = uids ? (*env)->NewObjectArray(env, w->count, string_cls, NULL) : NULL;
  jintArray kinds = names ? (*env)->NewIntArray(env, w->count) : NULL;
  jintArray counts = kinds ? (*env)->NewIntArray(env, w->count) : NULL;
  int ok = counts != NULL;
  for (int i = 0; ok && i < w->count; i++) {
    const mc_sync_waiting_device *d = &w->devices[i];
    jint kind = (jint)d->kind, tracks = (jint)d->tracks;
    (*env)->SetIntArrayRegion(env, kinds, i, 1, &kind);
    (*env)->SetIntArrayRegion(env, counts, i, 1, &tracks);
    jstring uid = new_string(env, d->uid);
    jstring name = new_string(env, d->name ? d->name : "");
    ok = uid && name;
    if (uid) (*env)->SetObjectArrayElement(env, uids, i, uid);
    if (name) (*env)->SetObjectArrayElement(env, names, i, name);
    JNI_DELETE(uid, name);
  }
  if (ok) out = (*env)->NewObject(env, info, ctor, (jint)w->tracks, uids, names, kinds, counts);
  mc_sync_waiting_free(w);
  return out;
}

/* What's wrong with the files here as a SyncProblems, its lost tracks as parallel arrays. */
JNI_FN(jobject, CoreSync, nativeProblems, jlong handle) {
  jclass info;
  jmethodID ctor =
    jni_ctor(env, JNI_CLASS("SyncProblems"),
             "(" J_STRING "II" J_STRINGS J_STRINGS J_STRINGS "[L" JNI_CLASS("SyncIssue") ";)V", &info);
  jclass string_cls = ctor ? (*env)->FindClass(env, "java/lang/String") : NULL;
  if (!string_cls) return NULL;
  mc_sync_problems *p = NULL;
  JNI_CHECK(mc_sync_get_problems(sync_of(handle), &p), NULL);
  char text[512];
  mc_sync_problems_text(p, text, sizeof text);
  jobject out = NULL;
  jstring sentences = new_string(env, text);
  jobjectArray uids = sentences ? (*env)->NewObjectArray(env, p->lost, string_cls, NULL) : NULL;
  jobjectArray titles = uids ? (*env)->NewObjectArray(env, p->lost, string_cls, NULL) : NULL;
  jobjectArray artists = titles ? (*env)->NewObjectArray(env, p->lost, string_cls, NULL) : NULL;
  int ok = artists != NULL;
  for (int i = 0; ok && i < p->lost; i++) {
    const mc_sync_lost_track *t = &p->tracks[i];
    jstring uid = new_string(env, t->uid);
    jstring title = new_string(env, t->title ? t->title : "");
    jstring artist = new_string(env, t->artist ? t->artist : "");
    ok = uid && title && artist;
    if (uid) (*env)->SetObjectArrayElement(env, uids, i, uid);
    if (title) (*env)->SetObjectArrayElement(env, titles, i, title);
    if (artist) (*env)->SetObjectArrayElement(env, artists, i, artist);
    JNI_DELETE(uid, title, artist);
  }
  jobjectArray issues = ok ? jni_issue_array(env, p) : NULL;
  if (issues)
    out = (*env)->NewObject(env, info, ctor, sentences, (jint)p->failed, (jint)p->unknown, uids, titles, artists,
                            issues);
  mc_sync_problems_free(p);
  return out;
}

/* Removes those of uids that are lost as of now, and returns how many went. */
JNI_FN(jint, CoreSync, nativeRemoveLost, jlong handle, jobjectArray uids) {
  int count = (*env)->GetArrayLength(env, uids);
  string_array cuids;
  mc_status st = string_array_get(env, uids, count, &cuids);
  if (st == MC_ERR_INVALID_ARG) st = MC_ERR_NOMEM; /* what this throws for a null or unreadable uid */
  mc_delete_result r;
  memset(&r, 0, sizeof r);
  if (st == MC_OK) st = mc_sync_remove_lost(sync_of(handle), cuids.strings, count, &r);
  string_array_release(env, &cuids);
  jni_check(env, st);
  return (jint)r.tracks_deleted;
}

/* items as parallel kinds and uids; 0 with an exception pending on failure. */
static int item_arrays(JNIEnv *env, jclass string_cls, const mc_profile_item *items, int count,
                       jintArray *kinds, jobjectArray *uids) {
  *kinds = (*env)->NewIntArray(env, count);
  *uids = *kinds ? (*env)->NewObjectArray(env, count, string_cls, NULL) : NULL;
  if (!*uids) return 0;
  for (int i = 0; i < count; i++) {
    jint kind = (jint)items[i].kind;
    (*env)->SetIntArrayRegion(env, *kinds, i, 1, &kind);
    jstring ref = new_string(env, items[i].ref_uid);
    if (!ref) return 0;
    (*env)->SetObjectArrayElement(env, *uids, i, ref);
    (*env)->DeleteLocalRef(env, ref);
  }
  return 1;
}

/* uid's profile as a SyncProfile, made with its private constructor that takes the items as
 * parallel kinds and uids. */
JNI_FN(jobject, CoreSync, nativeGetProfile, jlong handle, jstring uid) {
  jclass info;
  jmethodID ctor =
    jni_ctor(env, JNI_CLASS("SyncProfile"), "(" J_STRING "ZZIZJ[I" J_STRINGS "[I" J_STRINGS ")V", &info);
  jclass string_cls = ctor ? (*env)->FindClass(env, "java/lang/String") : NULL;
  if (!string_cls) return NULL;
  JNI_STR(cuid, uid, NULL);
  mc_sync_profile *p = NULL;
  JNI_CHECK(mc_sync_get_profile(sync_of(handle), cuid, &p), NULL);
  jobject out = NULL;
  jstring name = new_string(env, p->name);
  jintArray kinds, removed_kinds;
  jobjectArray uids, removed_uids;
  int ok = (!p->name || name) && item_arrays(env, string_cls, p->items, p->count, &kinds, &uids) &&
           item_arrays(env, string_cls, p->removed, p->removed_count, &removed_kinds, &removed_uids);
  if (ok) {
    jint kbps = p->format.format == MC_FORMAT_OPUS ? p->format.bitrate_kbps : 0;
    out = (*env)->NewObject(env, info, ctor, name, JNI_BOOL(p->archive), JNI_BOOL(p->sync_all), kbps,
                            JNI_BOOL(p->budget_enabled), (jlong)p->budget_bytes, kinds, uids, removed_kinds,
                            removed_uids);
  }
  mc_sync_profile_free(p);
  return out;
}

/* Stores device_uid's profile. There's no archive argument: mc_sync_set_profile() ignores it. */
JNI_FN(void, CoreSync, nativeSetProfile, jlong handle, jstring uid, jstring name, jboolean sync_all, jint kbps,
       jboolean budget_enabled, jlong budget_bytes, jintArray kinds, jobjectArray uids) {
  mc_sync_profile p;
  memset(&p, 0, sizeof p);
  if (profile_from_args(env, &p, name, JNI_FALSE, sync_all, kbps, budget_enabled, budget_bytes, kinds, uids)) {
    int ok = 1;
    const char *cuid = jni_chars(env, uid, &ok);
    if (cuid) jni_check(env, mc_sync_set_profile(sync_of(handle), cuid, &p));
    jni_release(env, uid, cuid);
  }
  profile_args_free(&p);
}

JNI_FN(void, CoreSync, nativeSetArchive, jlong handle, jstring uid, jboolean archive) {
  JNI_STR(cuid, uid);
  JNI_CHECK(mc_sync_set_archive(sync_of(handle), cuid, archive != JNI_FALSE));
}

/* Fills out with what device_uid would hold with the profile: tracks, total_bytes, kbps,
 * archive, over_budget, shortfall_bytes, limit_bytes and free_bytes, then step_bytes. */
JNI_FN(void, CoreSync, nativeEstimate, jlong handle, jstring uid, jstring name, jboolean archive,
       jboolean sync_all, jint kbps, jboolean budget_enabled, jlong budget_bytes, jintArray kinds,
       jobjectArray uids, jlongArray out) {
  mc_sync_profile p;
  memset(&p, 0, sizeof p);
  if (profile_from_args(env, &p, name, archive, sync_all, kbps, budget_enabled, budget_bytes, kinds, uids)) {
    int ok = 1;
    const char *cuid = jni_chars(env, uid, &ok);
    mc_sync_estimate e;
    if (cuid && jni_check(env, mc_sync_estimate_profile(sync_of(handle), cuid, &p, &e))) {
      jlong values[9 + MC_SYNC_ESTIMATE_STEPS] = {
        e.tracks,      e.total_bytes,     e.kbps,        e.archive,
        e.over_budget, e.shortfall_bytes, e.limit_bytes, e.free_bytes,
        e.imports_dropped,
      };
      for (int i = 0; i < MC_SYNC_ESTIMATE_STEPS; i++) values[9 + i] = e.step_bytes[i];
      (*env)->SetLongArrayRegion(env, out, 0, 9 + MC_SYNC_ESTIMATE_STEPS, values);
    }
    jni_release(env, uid, cuid);
  }
  profile_args_free(&p);
}

/* Checks this device's files (deep: re-hashes them) and fills out with checked, ok, failed and
 * deleted. */
JNI_FN(void, CoreSync, nativeVerify, jlong handle, jboolean deep, jlongArray out) {
  mc_verify_result r;
  memset(&r, 0, sizeof r);
  JNI_CHECK(mc_sync_verify(sync_of(handle), deep ? MC_VERIFY_DEEP : MC_VERIFY_FAST, &r));
  jlong values[4] = {r.checked, r.ok, r.failed, r.deleted};
  (*env)->SetLongArrayRegion(env, out, 0, 4, values);
}

/* Who this device transcodes for now (mc_sync_transcoding_for()): fills uids and tracks, as
 * long as both, and returns how many. */
JNI_FN(jint, CoreSync, nativeTranscodingFor, jlong handle, jobjectArray uids, jintArray tracks) {
  jsize max = (*env)->GetArrayLength(env, uids);
  jsize room = (*env)->GetArrayLength(env, tracks);
  if (room < max) max = room;
  if (max <= 0) return 0;
  mc_sync_transcode_target *t = calloc((size_t)max, sizeof *t);
  if (!t) return 0;
  int n = mc_sync_transcoding_for(sync_of(handle), t, (int)max);
  for (int i = 0; i < n; i++) {
    jstring uid = (*env)->NewStringUTF(env, t[i].device_uid);
    if (!uid) {
      n = i;
      break;
    }
    (*env)->SetObjectArrayElement(env, uids, i, uid);
    (*env)->DeleteLocalRef(env, uid);
    jint count = t[i].tracks;
    (*env)->SetIntArrayRegion(env, tracks, i, 1, &count);
  }
  free(t);
  return n;
}

/* Goes on with the weekly deep check for up to budget_ms; true once it's over for this week. */
JNI_FN(jboolean, CoreSync, nativeVerifyWeekly, jlong handle, jlong budget_ms) {
  int done = 0;
  jni_check(env, mc_sync_verify_weekly(sync_of(handle), (int64_t)budget_ms, &done));
  return JNI_BOOL(done);
}

/* Renames this device, for the whole group when in one. */
JNI_FN(void, CoreSync, nativeSetDeviceName, jlong handle, jstring name) {
  JNI_STR(cname, name);
  JNI_CHECK(mc_sync_set_device_name(sync_of(handle), cname));
}

/* This device's name as it is now: the group may have renamed it. */
JNI_FN(jstring, CoreSync, nativeGetDeviceName, jlong handle) {
  char name[4 * MC_SYNC_DEVICE_NAME_MAX + 4];
  JNI_CHECK(mc_sync_get_device_name(sync_of(handle), name, sizeof name), NULL);
  return new_string(env, name);
}

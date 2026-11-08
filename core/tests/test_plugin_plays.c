/* What a tethered device brings back (kithcore/tethered.h), through the test plugin's
 * folder target (plugin_folder.c, tethered_fixture.h): the device holds album Things,
 * which Kithsong wrote, and two tracks of its own, one of which is also in the library (by
 * artist, album, title and length within 2 seconds) and one not.
 *   - Plays: each is logged as a play from the device, the earlier ones of a track a track
 *     and 30 seconds apart before its last; counted; the count written back to the device's
 *     copy (never to the foreign track); unknown tracks counted as unmatched; and, with no
 *     scrobbler, nothing counted as unscrobbled. plays.tsv is cleared.
 *   - The same plays again (the same token) are never taken in twice.
 *   - On-The-Go playlists become the library's, named "On-The-Go N", with the device's
 *     name when the plain name is taken; only once.
 *   - A selected playlist goes to the device, as far as its tracks are there. */
#include "tethered_fixture.h"

static void play_line(char *buf, size_t cap, const char *key, int count, int64_t last) {
  size_t n = strlen(buf);
  snprintf(buf + n, cap - n, "%s\t%d\t%lld\n", key, count, (long long)last);
}

int main(int argc, char **argv) {
  if (argc < 2) return 2;
  CHECK(mc_init() == MC_OK);
  fx x;
  fx_open(&x, argv[1], "plugin_plays");

  char things[MC_UID_STRLEN], elsewhere[MC_UID_STRLEN], one[MC_UID_STRLEN], two[MC_UID_STRLEN],
    three[MC_UID_STRLEN], far[MC_UID_STRLEN];
  fx_album(&x, "Low", "Things", things);
  fx_album(&x, "Low", "Elsewhere", elsewhere);
  fx_track(&x, things, "One", 1, "wav", one, NULL);
  fx_track(&x, things, "Two", 2, "wav", two, NULL);
  fx_track(&x, things, "Three", 3, "wav", three, NULL);
  fx_track(&x, elsewhere, "Far", 1, "wav", far, NULL);

  char p[2048], buf[4096], sql[1024];
  fx_dev_path(&x, "KITHTEST", p, sizeof p);
  CHECK(fx_write_text(p, "id=DEV-2\nname=Walkman\n"));
  fx_dev_path(&x, "manifest.tsv", p, sizeof p);
  CHECK(fx_write_text(p, "F0001\t\t\t\t\tmp3\t128\t10\tfar\tLOW\telsewhere\t2700\t1\t0\n"
                         "F0002\t\t\t\t\tmp3\t128\t10\tNobody\tNope\tNil\t1000\t0\t0\n"));
  char uid[MC_UID_STRLEN];
  CHECK(mc_tethered_volume_added(x.t, x.dev, uid) == MC_OK);

  /* Things only, and the playlist Mix. */
  char mix[MC_UID_STRLEN];
  CHECK(mc_playlist_create(x.lib, "Mix", mix) == MC_OK);
  CHECK(mc_playlist_append(x.lib, mix, two, NULL) == MC_OK);
  CHECK(mc_playlist_append(x.lib, mix, far, NULL) == MC_OK);
  CHECK(mc_playlist_append(x.lib, mix, one, NULL) == MC_OK);
  mc_profile_item items[2];
  memset(items, 0, sizeof items);
  items[0].kind = MC_PROFILE_ALBUM;
  memcpy(items[0].ref_uid, things, MC_UID_STRLEN);
  items[1].kind = MC_PROFILE_PLAYLIST;
  memcpy(items[1].ref_uid, mix, MC_UID_STRLEN);
  mc_sync_profile prof;
  memset(&prof, 0, sizeof prof);
  prof.items = &items[0];
  prof.count = 1;
  CHECK(mc_tethered_set_profile(x.t, uid, &prof, NULL) == MC_OK);

  mc_tethered_result r;
  CHECK(fx_sync(&x, uid, &r));
  fx_print("first", &r);
  CHECK(r.status == MC_OK && r.written == 3 && r.foreign == 2 && r.plays_imported == 0);
  mline m[16];
  int n = fx_manifest(&x, m, 16);
  CHECK(n == 5);
  char key_one[64] = "", key_two[64] = "";
  mline *e = fx_find(m, n, one);
  if (e) snprintf(key_one, sizeof key_one, "%s", e->key);
  e = fx_find(m, n, two);
  if (e) snprintf(key_two, sizeof key_two, "%s", e->key);
  CHECK(key_one[0] && key_two[0]);

  /* Plays. */
  int64_t now = (int64_t)mc_wall_ms();
  int64_t last_one = now - 3600 * 1000, last_far = now - (int64_t)20 * 24 * 3600 * 1000;
  buf[0] = '\0';
  play_line(buf, sizeof buf, key_one, 3, last_one);
  play_line(buf, sizeof buf, "F0001", 1, last_far);
  play_line(buf, sizeof buf, "F0002", 2, now);
  char plays_path[2048];
  fx_dev_path(&x, "plays.tsv", plays_path, sizeof plays_path);
  CHECK(fx_write_text(plays_path, buf));
  CHECK(fx_sync(&x, uid, &r));
  fx_print("plays", &r);
  CHECK(r.status == MC_OK && r.plays_imported == 4 && r.plays_unmatched == 2);
  CHECK(r.plays_not_scrobbled == 0); /* no scrobbler: nothing was to be scrobbled */
  CHECK(r.updated == 1 && r.written == 0);
  CHECK(!mc_file_exists(plays_path));
  mc_play_log *log = NULL;
  CHECK(mc_library_list_play_events(x.lib, 0, INT64_MAX, &log) == MC_OK && log);
  if (log) {
    CHECK(log->count == 4);
    int ones = 0;
    for (int i = 0; i < log->count; i++) {
      const mc_play_event *ev = &log->items[i].event;
      CHECK(ev->context == MC_PLAY_CONTEXT_DEVICE && !strcmp(ev->context_uid, uid));
      CHECK(ev->end == MC_PLAY_END_FINISHED);
      if (!strcmp(ev->track_uid, one)) {
        /* Oldest first: last - 2 (1500 + 30000), last - (1500 + 30000), last. */
        CHECK(ev->started_ms == last_one - (int64_t)(2 - ones) * (TRACK_MS + 30000));
        CHECK(ev->listened_ms == TRACK_MS);
        ones++;
      } else {
        CHECK(!strcmp(ev->track_uid, far) && ev->started_ms == last_far);
      }
    }
    CHECK(ones == 3);
  }
  mc_play_log_free(log);
  snprintf(sql, sizeof sql, "SELECT sum(play_count) FROM track_plays WHERE track_uid = '%s'", one);
  CHECK(fx_int(&x, sql) == 3);
  snprintf(sql, sizeof sql, "SELECT max(last_played_ms) FROM track_plays WHERE track_uid = '%s'", one);
  CHECK(fx_int(&x, sql) == last_one);
  snprintf(sql, sizeof sql, "SELECT sum(play_count) FROM track_plays WHERE track_uid = '%s'", far);
  CHECK(fx_int(&x, sql) == 1);
  n = fx_manifest(&x, m, 16);
  e = fx_find(m, n, one);
  CHECK(e && e->play_count == 3 && e->last_played_ms == last_one); /* written back */
  e = fx_find(m, n, two);
  CHECK(e && e->play_count == 0);
  CHECK(n >= 1 && !strcmp(m[0].key, "F0001") && m[0].play_count == 1); /* foreign: untouched */

  /* The same plays again: an unplug before the device cleared them, say. */
  CHECK(fx_write_text(plays_path, buf));
  CHECK(fx_sync(&x, uid, &r));
  fx_print("again", &r);
  CHECK(r.status == MC_OK && r.plays_imported == 0 && r.plays_unmatched == 0 && r.updated == 0);
  CHECK(!mc_file_exists(plays_path));
  CHECK(fx_int(&x, "SELECT count(*) FROM play_events") == 4);
  snprintf(sql, sizeof sql, "SELECT sum(play_count) FROM track_plays WHERE track_uid = '%s'", one);
  CHECK(fx_int(&x, sql) == 3);

  /* On-The-Go playlists; the library has an "On-The-Go 1" already. */
  char taken[MC_UID_STRLEN];
  CHECK(mc_playlist_create(x.lib, "On-The-Go 1", taken) == MC_OK);
  snprintf(buf, sizeof buf, "On-The-Go\t%s,%s,F0002\nOn-The-Go\tF0001\n", key_one, key_two);
  char otg_path[2048];
  fx_dev_path(&x, "otg.tsv", otg_path, sizeof otg_path);
  CHECK(fx_write_text(otg_path, buf));
  CHECK(fx_sync(&x, uid, &r));
  fx_print("otg", &r);
  CHECK(r.status == MC_OK && r.playlists_imported == 2);
  CHECK(!mc_file_exists(otg_path));
  CHECK(fx_int(&x, "SELECT count(*) FROM playlists") == 4);
  char first[MC_UID_STRLEN] = "", second[MC_UID_STRLEN] = "";
  fx_text(&x, "SELECT uid FROM playlists WHERE name = 'On-The-Go 1 (Walkman)'", first, sizeof first);
  fx_text(&x, "SELECT uid FROM playlists WHERE name = 'On-The-Go 2'", second, sizeof second);
  CHECK(first[0] && second[0]);
  char got[MC_UID_STRLEN];
  snprintf(sql, sizeof sql, "SELECT count(*) FROM playlist_entries WHERE playlist_uid = '%s'", first);
  CHECK(fx_int(&x, sql) == 2);
  snprintf(sql, sizeof sql,
           "SELECT track_uid FROM playlist_entries WHERE playlist_uid = '%s' ORDER BY position, uid",
           first);
  fx_text(&x, sql, got, sizeof got);
  CHECK(!strcmp(got, one));
  snprintf(sql, sizeof sql, "SELECT track_uid FROM playlist_entries WHERE playlist_uid = '%s'", second);
  fx_text(&x, sql, got, sizeof got);
  CHECK(!strcmp(got, far));
  snprintf(sql, sizeof sql, "SELECT count(*) FROM playlist_entries WHERE playlist_uid = '%s'", taken);
  CHECK(fx_int(&x, sql) == 0);

  CHECK(fx_write_text(otg_path, buf)); /* again: not twice */
  CHECK(fx_sync(&x, uid, &r));
  CHECK(r.status == MC_OK && r.playlists_imported == 0);
  CHECK(fx_int(&x, "SELECT count(*) FROM playlists") == 4);

  /* Mix to the device: it brings Far, Kithsong's copy now beside the foreign one. */
  prof.count = 2;
  CHECK(mc_tethered_set_profile(x.t, uid, &prof, NULL) == MC_OK);
  CHECK(fx_sync(&x, uid, &r));
  fx_print("playlist", &r);
  CHECK(r.status == MC_OK && r.written == 1 && r.removed == 0);
  n = fx_manifest(&x, m, 16);
  e = fx_find(m, n, far);
  char key_far[64] = "";
  if (e) snprintf(key_far, sizeof key_far, "%s", e->key);
  CHECK(key_far[0]);
  fx_dev_path(&x, "playlists.tsv", p, sizeof p);
  FILE *f = mc_fopen(p, "rb");
  char line[512] = "", want[512];
  if (f) {
    if (!fgets(line, sizeof line, f)) line[0] = '\0';
    fclose(f);
  }
  snprintf(want, sizeof want, "%s\tMix\t%s,%s,%s\n", mix, key_two, key_far, key_one);
  CHECK(!strcmp(line, want));

  fx_close(&x);
  return TEST_RESULT();
}

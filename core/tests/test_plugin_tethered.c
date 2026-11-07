/* Syncing a tethered device (kithcore/tethered.h) through the test plugin's folder target,
 * loaded by the real loader (plugin_folder.c, tethered_fixture.h). The device plays WAV at
 * up to 16 bits, MP3 up to 320 kbps and the plugin's pretend lossy "lofi"; it holds a track
 * Kithsong didn't write.
 *   - A plain folder is no device; the device is found, listed and given a profile of
 *     everything in the target's default formats.
 *   - The first sync writes the three WAV tracks (through the plugin's encoder, for the
 *     limit) and copies the MP3 as it is, every file on the device hashing as the manifest
 *     says; the track with no original here is missing; the foreign track is left alone.
 *   - A second sync changes nothing.
 *   - A selection without the MP3's album removes it, and only it.
 *   - Lossless written as lofi at q2 replaces the WAVs under the same keys.
 *   - A budget too small writes and removes nothing, and says by how much.
 *   - Unplugged, it can't be synced, stays listed, and can then be forgotten. */
#include "tethered_fixture.h"

int main(int argc, char **argv) {
  if (argc < 2) return 2;
  CHECK(mc_init() == MC_OK);
  fx x;
  fx_open(&x, argv[1], "plugin_tethered");

  char things[MC_UID_STRLEN], other[MC_UID_STRLEN], t1[MC_UID_STRLEN], t2[MC_UID_STRLEN],
    t3[MC_UID_STRLEN], lossy[MC_UID_STRLEN], gone[MC_UID_STRLEN], lossy_file[MC_UID_STRLEN];
  fx_album(&x, "Low", "Things", things);
  fx_album(&x, "Low", "Other", other);
  fx_track(&x, things, "One", 1, "wav", t1, NULL);
  fx_track(&x, things, "Two", 2, "wav", t2, NULL);
  fx_track(&x, things, "Three", 3, "wav", t3, NULL);
  fx_track(&x, other, "Lossy", 1, "mp3", lossy, lossy_file);
  fx_track(&x, other, "Elsewhere", 2, NULL, gone, NULL);

  char p[2048], q[2048];
  /* No device: a folder without KITHTEST. */
  char uid[MC_UID_STRLEN];
  CHECK(mc_tethered_volume_added(x.t, x.libdir, uid) == MC_ERR_NOT_FOUND && !uid[0]);

  /* The device, with a track of its own. */
  fx_dev_path(&x, "KITHTEST", p, sizeof p);
  CHECK(fx_write_text(p, "id=DEV-1\nname=Walkman\nmax_bits=16\ncapacity=100000000\n"));
  fx_dev_path(&x, "Music", p, sizeof p);
  CHECK(mc_mkdirs(p) == MC_OK);
  fx_dev_path(&x, "Music/F0001.mp3", p, sizeof p);
  CHECK(fx_write_text(p, "not Kithsong's at all"));
  char foreign_sha[65];
  fx_sha(p, foreign_sha);
  fx_dev_path(&x, "manifest.tsv", q, sizeof q);
  CHECK(fx_write_text(q, "F0001\t\t\t\t\tmp3\t128\t21\tForeign\tSomeone\tElse\t1000\t4\t0\n"));

  CHECK(mc_tethered_volume_added(x.t, x.dev, uid) == MC_OK && strlen(uid) == 36);
  char again[MC_UID_STRLEN];
  CHECK(mc_tethered_volume_added(x.t, x.dev, again) == MC_OK && !strcmp(again, uid));
  mc_tethered_device_list *l = NULL;
  CHECK(mc_tethered_list(x.t, &l) == MC_OK && l && l->count == 1);
  if (l && l->count == 1) {
    CHECK(!strcmp(l->items[0].uid, uid));
    CHECK_STR(l->items[0].name, "Walkman");
    CHECK_STR(l->items[0].model, "Folder");
    CHECK_STR(l->items[0].kind, "Test folder");
    CHECK_STR(l->items[0].plugin_id, "ng.kithso.test.folder");
    CHECK(l->items[0].connected && l->items[0].usable && !l->items[0].syncing);
    CHECK(l->items[0].mount_path && mc_same_path(l->items[0].mount_path, x.dev));
    CHECK(l->items[0].capacity_bytes == 100000000 && l->items[0].last_sync_ms == 0);
  }
  mc_tethered_device_list_free(l);

  mc_sync_profile *prof = NULL;
  mc_tethered_policy pol;
  CHECK(mc_tethered_get_profile(x.t, uid, &prof, &pol) == MC_OK && prof);
  if (prof) CHECK(prof->sync_all && !prof->budget_enabled && prof->count == 0);
  CHECK_STR(pol.lossless_format, "wav");
  CHECK_STR(pol.lossless_setting, "");
  CHECK_STR(pol.lossy_format, "lofi");
  CHECK_STR(pol.lossy_setting, "q1");
  mc_sync_profile_free(prof);
  CHECK(mc_tethered_get_profile(x.t, "0192f1e4-7c3a-7b21-9d4e-1f2a3b4c5d6e", &prof, NULL) ==
        MC_ERR_NOT_FOUND);

  mc_tethered_choice_list *choices = NULL;
  CHECK(mc_tethered_choices(x.t, uid, &choices) == MC_OK && choices);
  if (choices) { /* wav, and lofi's two settings: mp3 has no encoder */
    CHECK(choices->count == 3);
    int lofi = 0;
    for (int i = 0; i < choices->count; i++)
      if (!strcmp(choices->items[i].format, "lofi")) lofi++;
    CHECK(lofi == 2);
  }
  mc_tethered_choice_list_free(choices);

  /* The first sync. */
  mc_tethered_result r;
  CHECK(fx_sync(&x, uid, &r));
  fx_print("first", &r);
  CHECK(r.status == MC_OK && r.written == 4 && r.missing == 1 && r.foreign == 1);
  CHECK(r.failed == 0 && r.removed == 0 && !r.over_budget && !r.full);
  CHECK(x.progress > 0);
  mline m[16];
  int n = fx_manifest(&x, m, 16);
  CHECK(n == 5);
  const char *mine[] = {t1, t2, t3, lossy};
  char keys[4][64];
  for (int i = 0; i < 4; i++) {
    mline *e = fx_find(m, n, mine[i]);
    CHECK(e != NULL);
    keys[i][0] = '\0';
    if (!e) continue;
    snprintf(keys[i], sizeof keys[i], "%s", e->key);
    char sha[65];
    fx_music(&x, e, p, sizeof p);
    fx_sha(p, sha);
    CHECK(!strcmp(sha, e->sha));
    CHECK(!strcmp(e->format, i < 3 ? "wav" : "mp3"));
    CHECK(!strcmp(e->artist, "Low"));
    CHECK(!strcmp(e->album, i < 3 ? "Things" : "Other"));
  }
  mline *lm = fx_find(m, n, lossy);
  if (lm) {
    char orig[65];
    snprintf(q, sizeof q, "%s/%s.mp3", x.libdir, lossy);
    fx_sha(q, orig);
    CHECK(!strcmp(lm->sha, orig) && !strcmp(lm->file_uid, lossy_file)); /* copied as it is */
    CHECK(lm->kbps == 192);
  }
  CHECK(fx_find(m, n, gone) == NULL);
  CHECK(!strcmp(m[0].key, "F0001") && !m[0].track_uid[0] && m[0].play_count == 4);
  fx_dev_path(&x, "Music/F0001.mp3", p, sizeof p);
  char sha_now[65];
  fx_sha(p, sha_now);
  CHECK(!strcmp(sha_now, foreign_sha));
  fx_dev_path(&x, "commits.log", p, sizeof p);
  CHECK(mc_file_exists(p));
  CHECK(mc_tethered_list(x.t, &l) == MC_OK && l && l->count == 1);
  if (l && l->count == 1) CHECK(l->items[0].last_sync_ms > 0 && !l->items[0].syncing);
  mc_tethered_device_list_free(l);

  /* Again: nothing to do. */
  char before[65], after[65];
  fx_dev_path(&x, "manifest.tsv", q, sizeof q);
  fx_sha(q, before);
  CHECK(fx_sync(&x, uid, &r));
  fx_print("again", &r);
  CHECK(r.status == MC_OK && r.written == 0 && r.removed == 0 && r.updated == 0 && r.failed == 0);
  CHECK(r.missing == 1 && r.foreign == 1);
  fx_sha(q, after);
  CHECK(!strcmp(before, after));

  /* Only Things: the MP3 goes, the foreign track stays. */
  mc_sync_profile sel;
  memset(&sel, 0, sizeof sel);
  mc_profile_item item;
  memset(&item, 0, sizeof item);
  item.kind = MC_PROFILE_ALBUM;
  memcpy(item.ref_uid, things, MC_UID_STRLEN);
  sel.items = &item;
  sel.count = 1;
  CHECK(mc_tethered_set_profile(x.t, uid, &sel, NULL) == MC_OK);
  CHECK(mc_tethered_get_profile(x.t, uid, &prof, &pol) == MC_OK && prof);
  if (prof) CHECK(!prof->sync_all && prof->count == 1 && !strcmp(prof->items[0].ref_uid, things));
  CHECK_STR(pol.lossy_format, "lofi"); /* NULL policy: kept */
  mc_sync_profile_free(prof);
  CHECK(fx_sync(&x, uid, &r));
  fx_print("selected", &r);
  CHECK(r.status == MC_OK && r.removed == 1 && r.written == 0 && r.foreign == 1 && r.missing == 0);
  n = fx_manifest(&x, m, 16);
  CHECK(n == 4 && fx_find(m, n, lossy) == NULL);
  snprintf(p, sizeof p, "%s/Music/%s.mp3", x.dev, keys[3]);
  CHECK(!mc_file_exists(p));
  fx_dev_path(&x, "Music/F0001.mp3", p, sizeof p);
  CHECK(mc_file_exists(p));

  /* Lossless as lofi q2: rewritten in place. */
  mc_tethered_policy lofi;
  memset(&lofi, 0, sizeof lofi);
  snprintf(lofi.lossless_format, sizeof lofi.lossless_format, "lofi");
  snprintf(lofi.lossless_setting, sizeof lofi.lossless_setting, "q2");
  snprintf(lofi.lossy_format, sizeof lofi.lossy_format, "lofi");
  snprintf(lofi.lossy_setting, sizeof lofi.lossy_setting, "q1");
  mc_tethered_policy bad = lofi;
  snprintf(bad.lossy_setting, sizeof bad.lossy_setting, "Q-1");
  CHECK(mc_tethered_set_profile(x.t, uid, &sel, &bad) == MC_ERR_INVALID_ARG);
  CHECK(mc_tethered_set_profile(x.t, uid, &sel, &lofi) == MC_OK);
  CHECK(fx_sync(&x, uid, &r));
  fx_print("lofi", &r);
  CHECK(r.status == MC_OK && r.written == 3 && r.removed == 0 && r.failed == 0);
  n = fx_manifest(&x, m, 16);
  CHECK(n == 4);
  for (int i = 0; i < 3; i++) {
    mline *e = fx_find(m, n, mine[i]);
    CHECK(e && !strcmp(e->format, "lofi") && !strcmp(e->setting, "q2") && !strcmp(e->key, keys[i]));
    if (!e) continue;
    char sha[65];
    fx_music(&x, e, p, sizeof p);
    fx_sha(p, sha);
    CHECK(!strcmp(sha, e->sha) && e->kbps == 64);
    snprintf(p, sizeof p, "%s/Music/%s.wav", x.dev, e->key);
    CHECK(!mc_file_exists(p)); /* the WAV it replaced */
  }

  /* A budget it can't meet: nothing changes. */
  sel.budget_enabled = 1;
  sel.budget_bytes = 1000;
  CHECK(mc_tethered_set_profile(x.t, uid, &sel, NULL) == MC_OK);
  fx_dev_path(&x, "manifest.tsv", q, sizeof q);
  fx_sha(q, before);
  sel.sync_all = 1; /* everything, so there's something to write and remove too */
  CHECK(mc_tethered_set_profile(x.t, uid, &sel, NULL) == MC_OK);
  CHECK(fx_sync(&x, uid, &r));
  fx_print("budget", &r);
  CHECK(r.status == MC_OK && r.over_budget && r.shortfall_bytes > 0);
  CHECK(r.written == 0 && r.removed == 0);
  fx_sha(q, after);
  CHECK(!strcmp(before, after));

  /* Unplugged. */
  CHECK(mc_tethered_forget(x.t, uid) == MC_ERR_IN_USE);
  CHECK(mc_tethered_volume_removed(x.t, x.dev) == MC_OK);
  CHECK(mc_tethered_volume_removed(x.t, x.dev) == MC_ERR_NOT_FOUND);
  CHECK(mc_tethered_sync(x.t, uid) == MC_ERR_NOT_FOUND);
  CHECK(mc_tethered_list(x.t, &l) == MC_OK && l && l->count == 1);
  if (l && l->count == 1) CHECK(!l->items[0].connected && !l->items[0].mount_path);
  mc_tethered_device_list_free(l);
  CHECK(mc_tethered_forget(x.t, uid) == MC_OK);
  CHECK(mc_tethered_list(x.t, &l) == MC_OK && l && l->count == 0);
  mc_tethered_device_list_free(l);
  CHECK(x.devices_events > 0);

  fx_close(&x);
  return TEST_RESULT();
}

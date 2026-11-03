/* Scoring used by the importer: string distance, assignment, release and recording scores. */
#include <math.h>
#include <string.h>

#include "musiccore/musiccore.h"
#include "tagger/match.h"
#include "tagger/musicbrainz.h"
#include "test.h"

#define NEAR(a, b) (fabs((a) - (b)) < 1e-9)

static void test_strings(void) {
  CHECK(NEAR(mc_string_distance("Abbey Road", "Abbey Road"), 0));
  CHECK(NEAR(mc_string_distance("AC/DC", "acdc"), 0));
  CHECK(NEAR(mc_string_distance("Bj\xC3\xB6rk", "Bjork"), 0));       /* Björk */
  CHECK(NEAR(mc_string_distance("The Beatles", "Beatles"), 0));
  CHECK(NEAR(mc_string_distance("Simon & Garfunkel", "Simon and Garfunkel"), 0));
  CHECK(NEAR(mc_string_distance("Don\xE2\x80\x99t Stop", "Don't Stop"), 0)); /* curly quote */

  double remaster = mc_string_distance("Help! (Remastered 2009)", "Help!");
  CHECK(remaster > 0 && remaster <= 0.1 + 1e-9);
  CHECK(mc_string_distance("Song - 2011 Remaster", "Song") <= 0.1 + 1e-9);

  CHECK(mc_string_distance("Abbey Road", "Let It Be") > 0.5);
  CHECK(mc_string_distance("Paranoid Android", "Paranoid Androd") < 0.1);

  CHECK(mc_string_distance(NULL, "x") < 0);
  CHECK(mc_string_distance("", "x") < 0);
  /* Only punctuation: compared exactly. */
  CHECK(NEAR(mc_string_distance("...", "..."), 0));
  CHECK(NEAR(mc_string_distance("...", "!!!"), 1));
  /* Non-Latin scripts are compared as code points. */
  CHECK(NEAR(mc_string_distance("\xE6\x9D\xB1\xE4\xBA\xAC", "\xE6\x9D\xB1\xE4\xBA\xAC"), 0));
}

/* Decorative dashes and tildes, subtitles and added translations in album titles. */
static void test_title_decorations(void) {
  /* SHARPNELSOUND COLLECTION -壱- */
  CHECK(NEAR(mc_string_distance("SHARPNELSOUND COLLECTION -\xE5\xA3\xB1-", "SHARPNELSOUND COLLECTION \xE5\xA3\xB1"), 0));
  CHECK(NEAR(mc_string_distance("YONDERDOME Decade -10 Years of DJ SHARPNEL-",
                                "YONDERDOME Decade ~10 Years of DJ SHARPNEL~"), 0));
  CHECK(NEAR(mc_string_distance("HARDDRIVE 150BPM \xEF\xBD\x9ELIFE IS GAME VOLUME.2.0", /* ～ */
                                "HARDDRIVE 150BPM \xE3\x80\x9CLIFE IS GAME VOLUME.2.0"), 0)); /* 〜 */
  CHECK(NEAR(mc_string_distance("SEX -Sound of EXtreme-", "S.E.X. Sound of EXtreme"), 0));
  /* Hyphens inside words are still only punctuation. */
  CHECK(NEAR(mc_string_distance("X-RATED", "X RATED"), 0));
  CHECK(NEAR(mc_string_distance("Kill'em All", "Killem All"), 0));
  CHECK(mc_string_distance("AIRWAVE-08", "AIRWAVE-09") > 0);

  /* A subtitle set off by a tilde or wrapped in dashes is a suffix, like "(Remastered)". */
  double sub = mc_string_distance("S.E.X. -Sound of EXtreme-", "S.E.X.");
  CHECK(sub > 0 && sub <= 0.1 + 1e-9);
  sub = mc_string_distance("BOSS ON PARADE ~XXX meets GABBA~", "BOSS ON PARADE");
  CHECK(sub > 0 && sub <= 0.1 + 1e-9);
  sub = mc_string_distance("HARDDRIVE 150BPM \xEF\xBD\x9ELIFE IS GAME", "HARDDRIVE 150BPM");
  CHECK(sub > 0 && sub <= 0.1 + 1e-9);
  /* Not a hyphen inside a word, nor one that only opens. */
  CHECK(mc_string_distance("X-RATED", "X") > 0.5);
  CHECK(mc_string_distance("TRACK -X", "TRACK") > 0.1 + 1e-9);

  /* アニメガバイト ANIME GABBA IT!: the title plus a translation in another script. */
  const char *anime = "\xE3\x82\xA2\xE3\x83\x8B\xE3\x83\xA1\xE3\x82\xAC\xE3\x83\x90\xE3\x82\xA4\xE3\x83\x88";
  char with[128];
  snprintf(with, sizeof with, "%s ANIME GABBA IT!", anime);
  double tr = mc_string_distance(with, anime);
  CHECK(tr > 0 && tr <= 0.1 + 1e-9);
  CHECK(NEAR(mc_string_distance(anime, with), tr));
  snprintf(with, sizeof with, "Anime Gabba %s", anime); /* and the other way round */
  tr = mc_string_distance(with, "Anime Gabba");
  CHECK(tr > 0 && tr <= 0.1 + 1e-9);
  /* Two titles that share only the first word aren't the same plus a translation. */
  snprintf(with, sizeof with, "%s ANIME", anime);
  char other[128];
  snprintf(other, sizeof other, "%s GABBA", anime);
  CHECK(mc_string_distance(with, other) > 0.2);
  /* Releases that really differ: more words in the same script, or a number. */
  CHECK(mc_string_distance("SPEEDCORE DANDY", "SPEEDCORE DANDY XXX") > 0.15);
  CHECK(mc_string_distance("SPF INFINI", "SPF INFINI 2") >= 0.1 - 1e-9);
  snprintf(with, sizeof with, "%s 2", anime);
  CHECK(mc_string_distance(with, anime) > 0.1 + 1e-9);
}

/* DJ-mix markers on track titles, and digit 0 for letter O. */
static void test_title_mix_markers(void) {
  CHECK(NEAR(mc_string_distance("Intro (Mixed)", "Intro"), 0));
  CHECK(NEAR(mc_string_distance("Nucl3ar Pow3r (Mixed)", "Nucl3ar Pow3r"), 0));
  CHECK(NEAR(mc_string_distance("Lalala (Want Somebody) (Paul Copping Remix - Mixed)",
                                "Lalala (Want Somebody) (Paul Copping remix)"), 0));
  CHECK(NEAR(mc_string_distance("Out of Time - Mixed", "Out of Time"), 0));
  CHECK(NEAR(mc_string_distance("Euphoria [Mixed Version]", "Euphoria"), 0));
  CHECK(NEAR(mc_string_distance("Tirrenia (Original Mix)", "Tirrenia"), 0));
  CHECK(NEAR(mc_string_distance("Coral Daze (Mixed) [Mixed]", "Coral Daze"), 0));
  /* A remix is still a suffix that costs something, and a title that is only "Mixed" stays. */
  double remix = mc_string_distance("Song (Extended Mix)", "Song");
  CHECK(remix > 0 && remix <= 0.1 + 1e-9);
  CHECK(mc_string_distance("Mixed", "Mixer") > 0);
  CHECK(mc_string_distance("Song (Mixed Feelings)", "Song") > 0);

  CHECK(NEAR(mc_string_distance("H20 (Mixed)", "H2O"), 0));
  CHECK(mc_string_distance("H20", "H2") > 0);
}

static void test_assign(void) {
  /* Optimum is 0->1, 1->0, 2->2 (cost 1 + 2 + 2 = 5). */
  const double square[] = {4, 1, 3,
                           2, 0, 5,
                           3, 2, 2};
  int out[3];
  CHECK(mc_assign(square, 3, 3, out) == MC_OK);
  CHECK(out[0] == 1 && out[1] == 0 && out[2] == 2);

  /* Fewer rows than columns: every row assigned, to distinct columns. */
  const double wide[] = {9, 1, 9,
                         1, 9, 9};
  CHECK(mc_assign(wide, 2, 3, out) == MC_OK);
  CHECK(out[0] == 1 && out[1] == 0);

  /* More rows than columns: the costliest row is left out. */
  const double tall[] = {1, 9,
                         9, 1,
                         5, 5};
  CHECK(mc_assign(tall, 3, 2, out) == MC_OK);
  CHECK(out[0] == 0 && out[1] == 1 && out[2] == -1);

  CHECK(mc_assign(NULL, 0, 0, out) == MC_OK);
}

static mb_track track(int number, const char *title, int length_ms) {
  mb_track t = {0};
  t.number = number;
  t.disc = 1;
  t.title = (char *)title;
  t.artist = "Low";
  t.length_ms = length_ms;
  return t;
}

static mc_tags file(int number, const char *title, int length_ms) {
  mc_tags f = {0};
  f.track = number;
  f.title = (char *)title;
  f.artist = "Low";
  f.album = "Things We Lost in the Fire";
  f.duration_ms = length_ms;
  f.year = 2001;
  return f;
}

static void test_release(void) {
  mb_track tracks[] = {track(1, "Sunflower", 274000), track(2, "Whore", 214000),
                       track(3, "Dinosaur Act", 381000)};
  mb_release rel = {0};
  rel.title = "Things We Lost in the Fire";
  rel.artist = "Low";
  rel.year = 2001;
  rel.tracks = tracks;
  rel.track_count = 3;

  /* Shuffled, slightly different lengths and an unnumbered file: still a clean match. */
  mc_tags files[] = {file(3, "Dinosaur Act", 383000), file(1, "Sunflower", 274500),
                     file(0, "Whore", 213000)};
  int map[3];
  mc_release_score s;
  CHECK(mc_score_release(&rel, files, 3, "Things We Lost in the Fire", "Low", map, &s) == MC_OK);
  CHECK(map[0] == 2 && map[1] == 0 && map[2] == 1);
  CHECK(s.missing == 0 && s.extra == 0);
  CHECK(s.distance < 0.02);

  /* One track missing: matched, but flagged (the importer never auto-accepts these). */
  CHECK(mc_score_release(&rel, files, 2, "Things We Lost in the Fire", "Low", map, &s) == MC_OK);
  CHECK(s.missing == 1 && s.extra == 0);
  CHECK(s.distance > 0.08);

  /* An unrelated file is left unmatched rather than forced onto a track. */
  mc_tags stray[] = {file(1, "Sunflower", 274000), file(2, "Whore", 214000),
                     file(3, "Dinosaur Act", 381000), file(4, "Completely Different Song", 95000)};
  stray[3].artist = "Someone Else";
  int map4[4];
  CHECK(mc_score_release(&rel, stray, 4, "Things We Lost in the Fire", "Low", map4, &s) == MC_OK);
  CHECK(map4[3] == -1);
  CHECK(s.extra == 1 && s.missing == 0);

  /* The same map scores the same; a pair the user makes counts however far apart. */
  mc_release_score again;
  CHECK(mc_score_release_map(&rel, stray, 4, "Things We Lost in the Fire", "Low", map4, &again) == MC_OK);
  CHECK(again.distance == s.distance && again.extra == 1 && again.missing == 0);
  rel.track_count = 4;
  mb_track four[4] = {tracks[0], tracks[1], tracks[2], track(4, "Bonus", 200000)};
  rel.tracks = four;
  CHECK(mc_score_release(&rel, stray, 4, "Things We Lost in the Fire", "Low", map4, &s) == MC_OK);
  CHECK(map4[3] == -1 && s.missing == 1 && s.extra == 1);
  map4[3] = 3;
  CHECK(mc_score_release_map(&rel, stray, 4, "Things We Lost in the Fire", "Low", map4, &again) == MC_OK);
  CHECK(again.missing == 0 && again.extra == 0 && again.distance < s.distance);
  rel.tracks = tracks;
  rel.track_count = 3;

  /* A different album title alone is enough to fall below the default 0.9 threshold. */
  mc_release_score right, wrong;
  mc_score_release(&rel, files, 3, "Things We Lost in the Fire", "Low", map, &right);
  rel.title = "Secret Name";
  mc_score_release(&rel, files, 3, "Things We Lost in the Fire", "Low", map, &wrong);
  CHECK(wrong.distance > 0.1);
  CHECK(right.distance < 0.02);

  /* A compilation credited to "V.A." counts as Various Artists. */
  rel.title = "Things We Lost in the Fire";
  rel.artist = "V.A.";
  CHECK(mc_score_release(&rel, files, 3, "Things We Lost in the Fire", "Various Artists", map, &s) == MC_OK);
  CHECK(s.distance < 0.02);
}

/* A release lookup's shape, cut down: a CD-R with the audio and a DVD-R Video whose tracks
 * are videos, though only one of the recordings says so (as on "SPF INFINI" by TDJ). */
#define TRACK(n, title, ms, video)                                                             \
  "{\"id\":\"3" #n "111111-2222-3333-4444-555555555555\",\"position\":" #n ",\"title\":\"" title \
  "\",\"length\":" #ms ",\"recording\":{\"id\":\"4" #n "111111-2222-3333-4444-555555555555\","  \
  "\"video\":" video "}}"
static const char k_cd_dvd_json[] =
  "{\"id\":\"85248da0-ab70-4310-8b51-878dbc11aaf4\",\"title\":\"SPF INFINI\",\"date\":\"2021\","
  "\"artist-credit\":[{\"name\":\"TDJ\",\"artist\":{\"id\":\"11111111-2222-3333-4444-555555555555\","
  "\"name\":\"TDJ\",\"sort-name\":\"TDJ\"}}],"
  "\"media\":["
  "{\"position\":1,\"format\":\"CD-R\",\"tracks\":["
  TRACK(1, "Intro", 85000, "false") "," TRACK(2, "Nucl3ar Pow3r", 139000, "false") ","
  TRACK(3, "H20", 264000, "false") "]},"
  "{\"position\":2,\"format\":\"DVD-R Video\",\"tracks\":["
  TRACK(1, "Intro", 85000, "false") "," TRACK(2, "H20", 264000, "true") "]}]}";

/* Plain "DVD"s: video when a recording on it is flagged as one, else audio. */
static const char k_dvd_json[] =
  "{\"id\":\"95248da0-ab70-4310-8b51-878dbc11aaf4\",\"title\":\"Live\","
  "\"media\":["
  "{\"position\":1,\"format\":\"DVD\",\"tracks\":["
  TRACK(1, "One", 200000, "false") "," TRACK(2, "Two", 200000, "true") "]},"
  "{\"position\":2,\"format\":\"DVD\",\"tracks\":[" TRACK(1, "Three", 200000, "false") "]}]}";

static void test_video_discs(void) {
  mb_release rel;
  CHECK(mb_release_parse(k_cd_dvd_json, sizeof k_cd_dvd_json - 1, &rel) == MC_OK);
  CHECK_STR(rel.format, "CD-R + DVD-R Video");
  /* Kept as MusicBrainz has them: two discs, five tracks, the DVD's numbered on it. */
  CHECK(rel.disc_total == 2 && rel.track_count == 5);
  if (rel.track_count == 5) {
    for (int j = 0; j < 3; j++) CHECK(!rel.tracks[j].video && !mc_track_left_out(&rel, j));
    for (int j = 3; j < 5; j++) CHECK(rel.tracks[j].video && mc_track_left_out(&rel, j));
    CHECK(rel.tracks[3].disc == 2 && rel.tracks[3].number == 1);
  }

  /* The CD's files match it fully: the DVD's tracks aren't missing, nor paired with a file,
   * though their titles and lengths are the CD's. */
  mc_tags files[] = {file(1, "Intro", 85000), file(2, "Nucl3ar Pow3r", 139000),
                     file(3, "H20", 264000)};
  for (int i = 0; i < 3; i++) files[i].artist = "TDJ", files[i].album = "SPF INFINI", files[i].year = 2021;
  int map[3];
  mc_release_score s;
  CHECK(mc_score_release(&rel, files, 3, "SPF INFINI", "TDJ", map, &s) == MC_OK);
  CHECK(map[0] == 0 && map[1] == 1 && map[2] == 2);
  CHECK(s.missing == 0 && s.extra == 0);
  CHECK(s.distance < 0.02);

  /* A CD track without its file is still missing. */
  CHECK(mc_score_release(&rel, files, 2, "SPF INFINI", "TDJ", map, &s) == MC_OK);
  CHECK(s.missing == 1 && s.extra == 0);

  /* A file the user pairs with a video is paired, and the CD track it isn't stays missing. */
  int user[3] = {0, 1, 4};
  CHECK(mc_score_release_map(&rel, files, 3, "SPF INFINI", "TDJ", user, &s) == MC_OK);
  CHECK(s.missing == 1 && s.extra == 0);

  /* All video (the sound of a concert DVD): scored against it as usual. */
  for (int j = 0; j < 3; j++) rel.tracks[j].video = 1;
  CHECK(!mc_track_left_out(&rel, 0) && !mc_track_left_out(&rel, 4));
  CHECK(mc_score_release(&rel, files, 3, "SPF INFINI", "TDJ", map, &s) == MC_OK);
  CHECK(s.missing == 2 && s.extra == 0);
  mb_release_clear(&rel);

  CHECK(mb_release_parse(k_dvd_json, sizeof k_dvd_json - 1, &rel) == MC_OK);
  CHECK(rel.track_count == 3);
  if (rel.track_count == 3) CHECK(rel.tracks[0].video && rel.tracks[1].video && !rel.tracks[2].video);
  mb_release_clear(&rel);
}

/* "SHARPNELSOUND COLLECTION 壱": credited to Various Artists, tagged with the label. */
static void test_various(void) {
  mb_track tracks[] = {track(1, "BLUE NOAH", 335000), track(2, "Kill'em All", 209000),
                       track(3, "PANTS", 358000)};
  tracks[0].artist = "Neon Genesis Gabbangelion";
  tracks[1].artist = tracks[2].artist = "DJ SHARPNEL";
  mb_credit_part va = {"Various Artists", "89ad4ac3-39f7-470e-963a-56509c546377", "Various Artists",
                       NULL};
  mb_release rel = {0};
  rel.title = "SHARPNELSOUND COLLECTION \xE5\xA3\xB1";
  rel.artist = "Various Artists";
  rel.credit = (mb_credit){&va, 1};
  rel.year = 2004;
  rel.tracks = tracks;
  rel.track_count = 3;

  const char *album = "SHARPNELSOUND COLLECTION -\xE5\xA3\xB1-";
  mc_tags files[] = {file(1, "BLUE NOAH", 335000), file(2, "Kill'em All", 209000),
                     file(3, "PANTS", 358000)};
  for (int i = 0; i < 3; i++) files[i].artist = tracks[i].artist, files[i].album = (char *)album, files[i].year = 2004;
  int map[3];
  mc_release_score s;
  CHECK(mc_score_release(&rel, files, 3, album, "SHARPNELSOUND", map, &s) == MC_OK);
  CHECK(s.missing == 0 && s.extra == 0);
  CHECK(s.distance < 0.02);

  /* A DJ with Various Artists is a compilation too. */
  mb_credit_part dj_va[] = {
    {"DJ SHARPNEL", "11111111-2222-3333-4444-555555555555", "DJ SHARPNEL", " & "},
    {"V.A.", "89ad4ac3-39f7-470e-963a-56509c546377", "Various Artists", NULL}};
  rel.artist = "DJ SHARPNEL & V.A.";
  rel.credit = (mb_credit){dj_va, 2};
  CHECK(mc_score_release(&rel, files, 3, album, "SHARPNELSOUND", map, &s) == MC_OK);
  CHECK(s.distance < 0.02);

  /* An ordinary album with the wrong album artist is still penalised. */
  mb_credit_part dj = dj_va[0];
  dj.join = NULL;
  rel.artist = "DJ SHARPNEL";
  rel.credit = (mb_credit){&dj, 1};
  mc_release_score right;
  CHECK(mc_score_release(&rel, files, 3, album, "DJ SHARPNEL", map, &right) == MC_OK);
  CHECK(mc_score_release(&rel, files, 3, album, "SHARPNELSOUND", map, &s) == MC_OK);
  CHECK(right.distance < 0.02);
  CHECK(s.distance > right.distance + 0.05);
}

static void test_recording(void) {
  mb_track t = track(1, "Hey Ya!", 235000);
  t.artist = "OutKast";
  mb_release rec = {0};
  rec.tracks = &t;
  rec.track_count = 1;
  rec.is_single = 1;

  mc_tags f = {0};
  f.title = "Hey Ya";
  f.artist = "Outkast";
  f.duration_ms = 236000;
  CHECK(mc_score_recording(&rec, &f) < 0.02);

  f.title = "Roses";
  CHECK(mc_score_recording(&rec, &f) > 0.2);

  /* No title tag at all counts against the candidate instead of being ignored. */
  f.title = NULL;
  CHECK(mc_score_recording(&rec, &f) > 0.3);
}

int main(void) {
  test_strings();
  test_title_decorations();
  test_title_mix_markers();
  test_assign();
  test_release();
  test_video_discs();
  test_various();
  test_recording();
  return TEST_RESULT();
}

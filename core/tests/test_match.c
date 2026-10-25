/* Scoring used by the importer: string distance, assignment, release and recording scores. */
#include <math.h>
#include <string.h>

#include "musiccore/musiccore.h"
#include "tagger/match.h"
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

  /* A different album title alone is enough to fall below the default 0.9 threshold. */
  mc_release_score right, wrong;
  mc_score_release(&rel, files, 3, "Things We Lost in the Fire", "Low", map, &right);
  rel.title = "Secret Name";
  mc_score_release(&rel, files, 3, "Things We Lost in the Fire", "Low", map, &wrong);
  CHECK(wrong.distance > 0.1);
  CHECK(right.distance < 0.02);
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
  test_assign();
  test_release();
  test_recording();
  return TEST_RESULT();
}

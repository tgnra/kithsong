/* Artist credits: the string splitter (credit.c) against a list of known artists and against
 * a library, and MusicBrainz artist-credit arrays as musicbrainz.c parses them. */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "library/library_internal.h"
#include "musiccore/musiccore.h"
#include "tagger/credit.h"
#include "tagger/musicbrainz.h"
#include "util/platform.h"
#include "test.h"

static int nocase_eq(const char *a, const char *b) {
  for (; *a && *b; a++, b++)
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
  return *a == *b;
}

/* user: a NULL-terminated list of names. */
static int known_in(void *user, const char *name) {
  for (const char *const *k = user; *k; k++)
    if (nocase_eq(*k, name)) return 1;
  return 0;
}

static const char *const k_known[] = {"Jay-Z", "Rihanna", "Beyoncé", "Simon & Garfunkel", "Simon",
                                      "Garfunkel", "Earth, Wind & Fire", "Earth", "Wind", "Fire",
                                      "The Emotions", "Daft Punk", "Pharrell Williams",
                                      "Nile Rodgers", "Skrillex", "Diplo", "Tyler", NULL};

/* Splits credit and checks the parts: name, join, name, join, ..., name (NULL ends). */
static void check_split_with(mc_known_artist_fn fn, void *user, const char *credit, ...) {
  mc_credit_split s;
  CHECK(mc_credit_split_text(credit, fn, user, &s) == MC_OK);
  va_list ap;
  va_start(ap, credit);
  int i = 0, ok = 1;
  for (const char *name; (name = va_arg(ap, const char *)); i++) {
    const char *join = va_arg(ap, const char *);
    if (i >= s.count || strcmp(s.parts[i].name, name) != 0) ok = 0;
    else if (join ? !s.parts[i].join || strcmp(s.parts[i].join, join) != 0 : s.parts[i].join != NULL)
      ok = 0;
    if (!join) {
      i++;
      break;
    }
  }
  va_end(ap);
  if (!ok || i != s.count) {
    fprintf(stderr, "split of \"%s\" gave %d part(s):", credit, s.count);
    for (int k = 0; k < s.count; k++)
      fprintf(stderr, " [%s|%s]", s.parts[k].name, s.parts[k].join ? s.parts[k].join : "-");
    fprintf(stderr, "\n");
    CHECK(0);
  }
  mc_credit_split_clear(&s);
}

#define SPLIT(credit, ...) check_split_with(known_in, (void *)k_known, credit, __VA_ARGS__)

static void check_splitter(void) {
  /* "feat."-style separators always split, known or not. */
  SPLIT("Jay-Z feat. Rihanna", "Jay-Z", " feat. ", "Rihanna", NULL);
  SPLIT("Nobody feat Somebody", "Nobody", " feat ", "Somebody", NULL);
  SPLIT("Nobody ft. Somebody", "Nobody", " ft. ", "Somebody", NULL);
  SPLIT("Nobody ft Somebody", "Nobody", " ft ", "Somebody", NULL);
  SPLIT("Nobody featuring Somebody", "Nobody", " featuring ", "Somebody", NULL);
  SPLIT("Nobody FEAT. Somebody", "Nobody", " FEAT. ", "Somebody", NULL);
  SPLIT("Nobody (feat. Somebody)", "Nobody", " (feat. ", "Somebody", NULL);
  SPLIT("Nobody [ft. Somebody]", "Nobody", " [ft. ", "Somebody", NULL);
  /* Not a separator inside a word or without spaces. */
  SPLIT("Aftermath", "Aftermath", NULL);
  SPLIT("Crafty feature", "Crafty feature", NULL);
  SPLIT("Nobody (Live)", "Nobody (Live)", NULL);

  /* Ambiguous separators: only between known artists. */
  SPLIT("Jay-Z & Rihanna", "Jay-Z", " & ", "Rihanna", NULL);
  SPLIT("Nobody & Rihanna", "Nobody & Rihanna", NULL);
  SPLIT("Jay-Z & Somebody", "Jay-Z & Somebody", NULL);
  SPLIT("Simon & Garfunkel", "Simon & Garfunkel", NULL); /* the pair is itself known */
  SPLIT("Jay-Z, Beyoncé & Rihanna", "Jay-Z", ", ", "Beyoncé", " & ", "Rihanna", NULL);
  SPLIT("Jay-Z, Beyoncé, and Rihanna", "Jay-Z", ", ", "Beyoncé", ", and ", "Rihanna", NULL);
  SPLIT("Skrillex x Diplo", "Skrillex", " x ", "Diplo", NULL);
  SPLIT("Skrillex X Diplo", "Skrillex", " X ", "Diplo", NULL);
  SPLIT("Skrillex vs. Diplo", "Skrillex", " vs. ", "Diplo", NULL);
  SPLIT("Skrillex vs Diplo", "Skrillex", " vs ", "Diplo", NULL);
  SPLIT("Skrillex with Diplo", "Skrillex", " with ", "Diplo", NULL);
  SPLIT("Skrillex / Diplo", "Skrillex", " / ", "Diplo", NULL);
  SPLIT("Skrillex; Diplo", "Skrillex", "; ", "Diplo", NULL);
  SPLIT("Skrillex and Diplo", "Skrillex", " and ", "Diplo", NULL);
  SPLIT("AC/DC", "AC/DC", NULL);

  /* Greedy: the longest known name first. */
  SPLIT("Earth, Wind & Fire", "Earth, Wind & Fire", NULL);
  SPLIT("Earth, Wind & Fire & The Emotions", "Earth, Wind & Fire", " & ", "The Emotions", NULL);
  SPLIT("Earth, Wind & Fire with The Emotions", "Earth, Wind & Fire", " with ", "The Emotions",
        NULL);
  /* "Tyler" is known, "the Creator" isn't: one artist. */
  SPLIT("Tyler, the Creator", "Tyler, the Creator", NULL);

  /* Both kinds: the "feat." split, then each side on its own. */
  SPLIT("Daft Punk feat. Pharrell Williams & Nile Rodgers", "Daft Punk", " feat. ",
        "Pharrell Williams", " & ", "Nile Rodgers", NULL);
  SPLIT("Daft Punk (feat. Pharrell Williams & Nile Rodgers)", "Daft Punk", " (feat. ",
        "Pharrell Williams", " & ", "Nile Rodgers", NULL);
  SPLIT("Somebody & Nobody feat. Rihanna", "Somebody & Nobody", " feat. ", "Rihanna", NULL);

  /* Case, whitespace, empty parts. */
  SPLIT("jay-z & RIHANNA", "jay-z", " & ", "RIHANNA", NULL);
  SPLIT("  Jay-Z   &   Rihanna  ", "Jay-Z", " & ", "Rihanna", NULL);
  SPLIT("Jay-Z & & Rihanna", "Jay-Z & & Rihanna", NULL);
  SPLIT("Jay-Z &", "Jay-Z &", NULL);
  SPLIT("Jay-Z feat. ", "Jay-Z feat.", NULL);
  SPLIT("Jay-Z feat.  feat. Rihanna", "Jay-Z feat.  feat. Rihanna", NULL);
  SPLIT("Jay-Z ()", "Jay-Z ()", NULL);
  SPLIT("(feat. Rihanna)", "(feat. Rihanna)", NULL);

  /* No callback: ambiguous separators never split. */
  check_split_with(NULL, NULL, "Jay-Z & Rihanna", "Jay-Z & Rihanna", NULL);
  check_split_with(NULL, NULL, "Jay-Z feat. Rihanna", "Jay-Z", " feat. ", "Rihanna", NULL);

  mc_credit_split s;
  CHECK(mc_credit_split_text("", known_in, (void *)k_known, &s) == MC_OK && s.count == 0);
  mc_credit_split_clear(&s);
  CHECK(mc_credit_split_text(NULL, known_in, (void *)k_known, &s) == MC_ERR_INVALID_ARG);

  CHECK(mc_credit_has_separator("A & B"));
  CHECK(mc_credit_has_separator("A (feat. B)"));
  CHECK(!mc_credit_has_separator("AC/DC"));
  CHECK(!mc_credit_has_separator("Low"));
}

/* --- Against a library --- */

static void add_artist(mc_library *lib, const char *uid, const char *name, const char *mbid) {
  sqlite3_stmt *stmt;
  CHECK(sqlite3_prepare_v2(mc_library_db(lib),
                           "INSERT INTO artists (uid, name, mb_artistid) VALUES (?1, ?2, ?3)", -1,
                           &stmt, NULL) == SQLITE_OK);
  sqlite3_bind_text(stmt, 1, uid, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, name, -1, SQLITE_STATIC);
  if (mbid) sqlite3_bind_text(stmt, 3, mbid, -1, SQLITE_STATIC);
  CHECK(sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);
}

static void check_library(const char *dir) {
  char path[1100];
  snprintf(path, sizeof path, "%s/test_credit.d", dir);
  mc_mkdirs(path);
  char db[1200];
  snprintf(db, sizeof db, "%s/library.db", path);
  mc_remove(db);
  mc_library *lib;
  CHECK(mc_library_open(path, &lib) == MC_OK);
  if (!lib) return;
  add_artist(lib, "00000000-0000-7000-8000-000000000001", "Jay-Z", NULL);
  add_artist(lib, "00000000-0000-7000-8000-000000000002", "Rihanna", NULL);
  /* Made by an older version from a whole credit: not one artist. */
  add_artist(lib, "00000000-0000-7000-8000-000000000003", "Jay-Z & Rihanna", NULL);
  /* One artist, as MusicBrainz says. */
  add_artist(lib, "00000000-0000-7000-8000-000000000004", "Simon & Garfunkel",
             "5d02f264-e225-41ff-83f7-d9b1f0b1874a");
  add_artist(lib, "00000000-0000-7000-8000-000000000005", "Simon", NULL);
  add_artist(lib, "00000000-0000-7000-8000-000000000006", "Garfunkel", NULL);

  CHECK(mc_credit_known_in_library(lib, "Jay-Z"));
  CHECK(mc_credit_known_in_library(lib, "jay-z"));
  CHECK(!mc_credit_known_in_library(lib, "Jay-Z & Rihanna"));
  CHECK(mc_credit_known_in_library(lib, "Simon & Garfunkel"));
  CHECK(!mc_credit_known_in_library(lib, "Nobody"));
  CHECK(!mc_credit_known_in_library(lib, ""));

  check_split_with(mc_credit_known_in_library, lib, "Jay-Z & Rihanna", "Jay-Z", " & ", "Rihanna",
                   NULL);
  check_split_with(mc_credit_known_in_library, lib, "Simon & Garfunkel", "Simon & Garfunkel", NULL);
  check_split_with(mc_credit_known_in_library, lib, "Jay-Z & Nobody", "Jay-Z & Nobody", NULL);
  mc_library_close(lib);
}

/* --- MusicBrainz artist-credit arrays --- */

static void check_part(const mb_credit *c, int i, const char *name, const char *id, const char *sort,
                       const char *join) {
  if (i >= c->count) {
    CHECK(i < c->count);
    return;
  }
  const mb_credit_part *p = &c->parts[i];
  CHECK_STR(p->name, name);
  CHECK(id ? p->id && strcmp(p->id, id) == 0 : !p->id);
  CHECK(sort ? p->sort_name && strcmp(p->sort_name, sort) == 0 : !p->sort_name);
  CHECK(join ? p->join && strcmp(p->join, join) == 0 : !p->join);
}

#define JAY "f82bcf78-5b69-4622-a5ef-73800768d9ac"
#define RIRI "73e5e69d-3554-40d8-8516-00cb38737a1c"
#define KANYE "164f0d73-1234-4e2c-8743-d77bf2191051"

/* A release lookup's shape, cut down: the release credited to two artists, one track to
 * one and one with a "feat." credited under another name. */
static const char k_release_json[] =
  "{\"id\":\"11111111-2222-3333-4444-555555555555\",\"title\":\"Watch the Throne\","
  "\"date\":\"2011-08-08\","
  "\"artist-credit\":["
  "{\"name\":\"Jay-Z\",\"joinphrase\":\" & \",\"artist\":{\"id\":\"" JAY "\",\"name\":\"JAY-Z\","
  "\"sort-name\":\"Jay-Z\"}},"
  "{\"name\":\"Kanye West\",\"joinphrase\":\"\",\"artist\":{\"id\":\"" KANYE "\","
  "\"name\":\"Kanye West\",\"sort-name\":\"West, Kanye\"}}],"
  "\"release-group\":{\"id\":\"21111111-2222-3333-4444-555555555555\",\"primary-type\":\"Album\"},"
  "\"media\":[{\"position\":1,\"format\":\"CD\",\"tracks\":["
  "{\"id\":\"31111111-2222-3333-4444-555555555555\",\"position\":1,\"title\":\"Otis\","
  "\"artist-credit\":[{\"name\":\"Jay-Z\",\"joinphrase\":\"\",\"artist\":{\"id\":\"" JAY "\","
  "\"name\":\"JAY-Z\",\"sort-name\":\"Jay-Z\"}}],"
  "\"recording\":{\"id\":\"41111111-2222-3333-4444-555555555555\"}},"
  "{\"id\":\"32222222-2222-3333-4444-555555555555\",\"position\":2,\"title\":\"Run This Town\","
  "\"artist-credit\":["
  "{\"name\":\"Hova\",\"joinphrase\":\" feat. \",\"artist\":{\"id\":\"" JAY "\",\"name\":\"JAY-Z\","
  "\"sort-name\":\"Jay-Z\"}},"
  "{\"name\":\"Rihanna\",\"artist\":{\"id\":\"" RIRI "\",\"name\":\"Rihanna\","
  "\"sort-name\":\"Rihanna\"}}],"
  "\"recording\":{\"id\":\"42222222-2222-3333-4444-555555555555\"}}]}]}";

/* A recording search's shape: the recording's credit stands in for its release's. */
static const char k_recordings_json[] =
  "{\"recordings\":[{\"id\":\"51111111-2222-3333-4444-555555555555\",\"title\":\"Umbrella\","
  "\"artist-credit\":["
  "{\"name\":\"Rihanna\",\"joinphrase\":\" feat. \",\"artist\":{\"id\":\"" RIRI "\","
  "\"name\":\"Rihanna\",\"sort-name\":\"Rihanna\"}},"
  "{\"name\":\"Jay-Z\",\"artist\":{\"id\":\"" JAY "\",\"name\":\"JAY-Z\",\"sort-name\":\"Jay-Z\"}}],"
  "\"releases\":[]}]}";

static void check_musicbrainz(void) {
  mb_release rel;
  CHECK(mb_release_parse(k_release_json, sizeof k_release_json - 1, &rel) == MC_OK);
  CHECK_STR(rel.artist, "Jay-Z & Kanye West");
  CHECK(rel.artist_id == NULL);
  CHECK_STR(rel.primary_artist_id, JAY);
  CHECK(rel.credit.count == 2);
  check_part(&rel.credit, 0, "Jay-Z", JAY, "Jay-Z", " & ");
  check_part(&rel.credit, 1, "Kanye West", KANYE, "West, Kanye", NULL);
  CHECK(rel.track_count == 2);
  if (rel.track_count == 2) {
    CHECK_STR(rel.tracks[0].artist, "Jay-Z");
    CHECK_STR(rel.tracks[0].artist_id, JAY);
    CHECK(rel.tracks[0].credit.count == 1);
    check_part(&rel.tracks[0].credit, 0, "Jay-Z", JAY, "Jay-Z", NULL);
    CHECK_STR(rel.tracks[1].artist, "Hova feat. Rihanna");
    CHECK(rel.tracks[1].artist_id == NULL);
    CHECK(rel.tracks[1].credit.count == 2);
    check_part(&rel.tracks[1].credit, 0, "Hova", JAY, "Jay-Z", " feat. ");
    check_part(&rel.tracks[1].credit, 1, "Rihanna", RIRI, "Rihanna", NULL);
  }
  mb_release_clear(&rel);
  CHECK(rel.credit.count == 0 && rel.credit.parts == NULL);

  mb_release recs[2];
  int n = 0;
  CHECK(mb_recordings_parse(k_recordings_json, sizeof k_recordings_json - 1, recs, 2, &n) == MC_OK);
  CHECK(n == 1);
  if (n == 1) {
    CHECK_STR(recs[0].artist, "Rihanna feat. Jay-Z");
    CHECK(recs[0].credit.count == 2);
    check_part(&recs[0].credit, 0, "Rihanna", RIRI, "Rihanna", " feat. ");
    check_part(&recs[0].credit, 1, "Jay-Z", JAY, "Jay-Z", NULL);
    CHECK(recs[0].track_count == 1);
    if (recs[0].track_count == 1) {
      CHECK(recs[0].tracks[0].credit.count == 2);
      check_part(&recs[0].tracks[0].credit, 1, "Jay-Z", JAY, "Jay-Z", NULL);
    }
    mb_release_clear(&recs[0]);
  }
}

int main(int argc, char **argv) {
  CHECK(mc_init() == MC_OK);
  check_splitter();
  check_library(argc > 1 ? argv[1] : ".");
  check_musicbrainz();
  return TEST_RESULT();
}

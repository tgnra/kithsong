/* Keys for the importer's extended search: folder names, catalogue numbers, artists in file
 * names and titles, and the keys gathered for a whole album. The names are real ones. */
#include <stdlib.h>
#include <string.h>

#include "musiccore/musiccore.h"
#include "tagger/search_keys.h"
#include "test.h"

#define EM " \xE2\x80\x94 " /* " — " */

static void test_folder_names(void) {
  mc_folder_name f;
  mc_folder_name_parse("SHARPNELSOUND (DJ CHUCKY) - NOW OR NEVER [SRPC-M002] (C63)", &f);
  CHECK_STR(f.prefix, "SHARPNELSOUND");
  CHECK_STR(f.paren, "DJ CHUCKY");
  CHECK_STR(f.title, "NOW OR NEVER");
  CHECK(f.catno_count == 1);
  CHECK_STR(f.catnos[0], "SRPC-M002");
  CHECK(!f.various);
  mc_folder_name_clear(&f);

  /* A compilation: the "and V.A." goes, and says so. The title keeps its own " - ". */
  mc_folder_name_parse("SHARPNELSOUND (The Speed Freak and V.A.) - A GABBERDISCO COMPILATION - "
                       "The future sound of retro vol.1 [SRPD-0009] (C89)", &f);
  CHECK_STR(f.prefix, "SHARPNELSOUND");
  CHECK_STR(f.paren, "The Speed Freak");
  CHECK(f.various);
  CHECK_STR(f.title, "A GABBERDISCO COMPILATION - The future sound of retro vol.1");
  CHECK(f.catno_count == 1);
  CHECK_STR(f.catnos[0], "SRPD-0009");
  mc_folder_name_clear(&f);

  /* No parentheses: the prefix may be a label or an artist; the caller decides. A
   * parenthesised part of the title stays in the title. */
  mc_folder_name_parse("SHARPNELSOUND - MAD BREAKS (Comiket Version) [SRPC-0019] (C68)", &f);
  CHECK_STR(f.prefix, "SHARPNELSOUND");
  CHECK(f.paren == NULL);
  CHECK_STR(f.title, "MAD BREAKS (Comiket Version)");
  mc_folder_name_clear(&f);

  /* A date in brackets is no catalogue number. */
  mc_folder_name_parse("SHARPNELSOUND - X-RATED -\xE3\x82\xAF (Comiket Version) [2002.08.11] (C62)", &f);
  CHECK(f.catno_count == 0);
  CHECK_STR(f.title, "X-RATED -\xE3\x82\xAF (Comiket Version)");
  mc_folder_name_clear(&f);

  /* Two labels and two catalogue numbers, split at full-width slashes. */
  mc_folder_name_parse("SHARPNELSOUND\xEF\xBC\x8FX-TREME HARD (The Speed Freak) - TOKYO\xEF\xBC\x8F"
                       "DEVASTATE [SRPD-0007\xEF\xBC\x8FXHCD-022] (M3-36)", &f);
  CHECK_STR(f.prefix, "SHARPNELSOUND\xEF\xBC\x8FX-TREME HARD");
  CHECK_STR(f.paren, "The Speed Freak");
  CHECK(f.catno_count == 2);
  CHECK_STR(f.catnos[0], "SRPD-0007");
  CHECK_STR(f.catnos[1], "XHCD-022");
  mc_folder_name_clear(&f);

  /* An edition suffix: the number itself and its base. */
  mc_folder_name_parse("SHARPNELSOUND - SEX -Sound of EXtreme- (Promo CDr) [SRPC-0008LTD] (C60)", &f);
  CHECK(f.catno_count == 2);
  CHECK_STR(f.catnos[0], "SRPC-0008LTD");
  CHECK_STR(f.catnos[1], "SRPC-0008");
  CHECK_STR(f.title, "SEX -Sound of EXtreme- (Promo CDr)");
  mc_folder_name_clear(&f);

  /* A plain "Artist - Album (Year)" and a name with no dash at all. */
  mc_folder_name_parse("Radiohead - OK Computer (1997)", &f);
  CHECK_STR(f.prefix, "Radiohead");
  CHECK_STR(f.title, "OK Computer");
  CHECK(f.catno_count == 0);
  mc_folder_name_clear(&f);
  mc_folder_name_parse("OK Computer", &f);
  CHECK(f.prefix == NULL);
  CHECK_STR(f.title, "OK Computer");
  mc_folder_name_clear(&f);
  mc_folder_name_parse("\xE3\x82\xA2\xE3\x83\xAB\xE3\x83\x90\xE3\x83\xA0\xE3\x80\x90VICL-12345\xE3\x80\x91", &f);
  CHECK(f.catno_count == 1);
  CHECK_STR(f.catnos[0], "VICL-12345");
  mc_folder_name_clear(&f);
}

static int is_catno(const char *s) {
  return mc_is_catno(s, strlen(s));
}

static void test_catnos(void) {
  CHECK(is_catno("SRPC-0014"));
  CHECK(is_catno("SRPC-000X"));
  CHECK(is_catno("XHCD-022"));
  CHECK(is_catno("VICL 12345"));
  CHECK(!is_catno("2002.08.11")); /* no letter */
  CHECK(!is_catno("2016"));
  CHECK(!is_catno("V0"));         /* too short, one digit */
  CHECK(!is_catno("FLAC"));
  CHECK(!is_catno("24bit 96kHz"));
  CHECK(!is_catno("320kbps"));
  CHECK(!is_catno("WEB 2016"));
  CHECK(!is_catno("C89"));         /* an event (Comiket 89) */
  CHECK(!is_catno("Disc 12"));
  CHECK(!is_catno("\xE4\xBE\x8D 0001")); /* non-ASCII */

  char *list[MC_KEYS_MAX] = {0};
  int n = mc_catnos_add(list, 0, MC_KEYS_MAX, "SRPD-0007\xEF\xBC\x8FXHCD-022");
  n = mc_catnos_add(list, n, MC_KEYS_MAX, "srpd-0007"); /* already there */
  n = mc_catnos_add(list, n, MC_KEYS_MAX, NULL);
  CHECK(n == 2);
  CHECK_STR(list[0], "SRPD-0007");
  CHECK_STR(list[1], "XHCD-022");
  n = mc_catnos_add(list, n, MC_KEYS_MAX, "A-100; B-200, C-300");
  CHECK(n == MC_KEYS_MAX); /* capped */
  for (int i = 0; i < n; i++) free(list[i]);
}

static void test_file_names(void) {
  char *a = mc_filename_artist("C:\\Music\\X\\02. DJ SHARPNEL" EM "SAMURAI HARDBEAT.flac", "SAMURAI HARDBEAT");
  CHECK_STR(a, "DJ SHARPNEL");
  free(a);
  a = mc_filename_artist("/m/x/08. NEOPHTE & STUNEED GUYS" EM "THERE IS NO OTHER.flac", NULL);
  CHECK_STR(a, "NEOPHTE & STUNEED GUYS");
  free(a);
  /* "Title - Artist": the title tag tells which side is which. */
  a = mc_filename_artist("/m/x/03 - Paranoid Android - Radiohead.mp3", "Paranoid Android");
  CHECK_STR(a, "Radiohead");
  free(a);
  /* Neither side is the title: no guess. */
  a = mc_filename_artist("/m/x/03 - Foo - Bar.mp3", "Something Else Entirely");
  CHECK(a == NULL);
  free(a);
  /* No separator, or only a track number. */
  CHECK(mc_filename_artist("/m/x/03 Airbag.flac", "Airbag") == NULL);
  CHECK(mc_filename_artist("/m/x/03.flac", NULL) == NULL);
  /* An artist whose name starts with digits isn't a track number. */
  a = mc_filename_artist("/m/x/2Pac - Changes.mp3", "Changes");
  CHECK_STR(a, "2Pac");
  free(a);

  a = mc_title_artist("DJ SHARPNEL - SAMURAI HARDBEAT");
  CHECK_STR(a, "DJ SHARPNEL");
  free(a);
  CHECK(mc_title_artist("P.H.P intro -Gimme More BPM!!-") == NULL); /* no spaced dash */
  CHECK(mc_title_artist("SAMURAI HARDBEAT") == NULL);

  a = mc_main_artist("DJ SHARPNEL feat. \xE5\xA4\xA7\xE7\x80\xAC\xE8\x89\xAF");
  CHECK_STR(a, "DJ SHARPNEL");
  free(a);
  a = mc_main_artist("Search & Destroy");
  CHECK_STR(a, "Search & Destroy");
  free(a);
}

static int has(char *const *list, int n, const char *s) {
  for (int i = 0; i < n; i++)
    if (strcmp(list[i], s) == 0) return 1;
  return 0;
}

static void test_keys(void) {
  /* "SHARPNELSOUND - Cyclick": every track is DJ SHARPNEL's, so the album artist tag is the
   * label. The catalogue number comes from the CATALOGID tag. */
  mc_tags files[3] = {0};
  const char *paths[3] = {"/d/SHARPNELSOUND - Cyclick [SRPC-0026] (C78)/01. DJ SHARPNEL" EM "ALERT.flac",
                          "/d/SHARPNELSOUND - Cyclick [SRPC-0026] (C78)/02. DJ SHARPNEL" EM "KTN GTR.flac",
                          "/d/SHARPNELSOUND - Cyclick [SRPC-0026] (C78)/03. DJ SHARPNEL" EM "AIM.flac"};
  const char *titles[3] = {"ALERT", "KTN GTR", "AIM"};
  for (int i = 0; i < 3; i++) {
    files[i].title = (char *)titles[i];
    files[i].artist = "DJ SHARPNEL";
    files[i].album_artist = "SHARPNELSOUND";
    files[i].catalog = "SRPC-0026";
  }
  mc_search_keys k;
  CHECK(mc_search_keys_build("/d/SHARPNELSOUND - Cyclick [SRPC-0026] (C78)", files, paths, 3, "Cyclick",
                             "SHARPNELSOUND", &k) == MC_OK);
  CHECK_STR(k.album, "Cyclick");
  CHECK(k.folder_album == NULL);
  CHECK(k.artist_count == 1);
  CHECK_STR(k.artists[0], "DJ SHARPNEL");
  CHECK(k.label_count == 1);
  CHECK_STR(k.labels[0], "SHARPNELSOUND");
  CHECK(k.catno_count == 1);
  CHECK_STR(k.catnos[0], "SRPC-0026");
  CHECK(!k.various);
  mc_search_keys_clear(&k);

  /* A compilation with "Label (Artist and V.A.)" in the album artist tag and no tags but
   * the artist in the file names. */
  const char *comp[3] = {"/d/L (The Speed Freak and V.A.) - Comp [SRPD-0009]/01. The Speed Freak" EM "Never.flac",
                         "/d/L (The Speed Freak and V.A.) - Comp [SRPD-0009]/02. Wendy Milan" EM "Gabberdisco.flac",
                         "/d/L (The Speed Freak and V.A.) - Comp [SRPD-0009]/03. S.C.U.M." EM "Forever.flac"};
  mc_tags bare[3] = {0};
  CHECK(mc_search_keys_build("/d/L (The Speed Freak and V.A.) - Comp [SRPD-0009]", bare, comp, 3, "Comp",
                             "L (The Speed Freak and V.A.)", &k) == MC_OK);
  CHECK(k.various);
  CHECK(k.artist_count == 1);
  CHECK_STR(k.artists[0], "The Speed Freak");
  CHECK(k.label_count == 1);
  CHECK_STR(k.labels[0], "L");
  CHECK(k.catno_count == 1);
  mc_search_keys_clear(&k);

  /* An ordinary album: the folder prefix is the artist, not a label, and nothing repeats
   * the item's own artist. */
  mc_tags ok[2] = {0};
  const char *okp[2] = {"/d/Radiohead - OK Computer (1997)/01 Airbag.flac",
                        "/d/Radiohead - OK Computer (1997)/02 Paranoid Android.flac"};
  ok[0].artist = ok[1].artist = "Radiohead";
  ok[0].title = "Airbag";
  ok[1].title = "Paranoid Android";
  CHECK(mc_search_keys_build("/d/Radiohead - OK Computer (1997)", ok, okp, 2, "OK Computer", "Radiohead", &k) ==
        MC_OK);
  CHECK(k.artist_count == 0);
  CHECK(k.label_count == 0);
  CHECK(k.catno_count == 0);
  CHECK(!k.various);
  mc_search_keys_clear(&k);

  /* The album artist tag "Label (Artist)", with a folder title that differs from the tag. */
  mc_tags one[1] = {0};
  one[0].artist = "BASSALERT";
  one[0].title = "DEWEGWIJER";
  const char *onep[1] = {"/d/x/01. BASSALERT" EM "DEWEGWIJER.flac"};
  CHECK(mc_search_keys_build("/d/x", one, onep, 1, "NOW OR NEVER", "SHARPNELSOUND (DJ CHUCKY)", &k) == MC_OK);
  CHECK(has(k.artists, k.artist_count, "DJ CHUCKY"));
  CHECK_STR(k.artists[0], "DJ CHUCKY");
  CHECK(has(k.labels, k.label_count, "SHARPNELSOUND"));
  CHECK_STR(k.folder_album, "x");
  mc_search_keys_clear(&k);

  /* A DJ mix in one file: the catalogue number is in its album tag. */
  mc_tags mix[1] = {0};
  mix[0].artist = "DJ SHARPNEL";
  mix[0].title = mix[0].album = "BREAK IT OLDSCHOOL [SRPC-L007]";
  const char *mixp[1] = {"/d/Downloads/BREAK IT OLDSCHOOL.flac"};
  CHECK(mc_search_keys_build("/d/Downloads", mix, mixp, 1, mix[0].album, "DJ SHARPNEL", &k) == MC_OK);
  CHECK(k.catno_count == 1 && has(k.catnos, k.catno_count, "SRPC-L007"));
  mc_search_keys_clear(&k);
}

/* The title without the disc, or NULL if mc_disc_suffix finds none; *disc gets the disc. */
static char *disc_base(const char *title, int *disc) {
  size_t len = 0;
  static char buf[256];
  *disc = mc_disc_suffix(title, &len);
  if (!*disc || len >= sizeof buf) return NULL;
  memcpy(buf, title, len);
  buf[len] = '\0';
  return buf;
}

static void test_disc_suffixes(void) {
  int disc;
  CHECK_STR(disc_base("\xE5\x8D\xB1\xE3\x81\xAA\xE3\x81\x84\xE3\x83\x99\xE3\x83\xBC\xE3\x82\xB9\xE3\x83\xA9"
                      "\xE3\x82\xA4\xE3\x83\xB3 Disc1", &disc),
            "\xE5\x8D\xB1\xE3\x81\xAA\xE3\x81\x84\xE3\x83\x99\xE3\x83\xBC\xE3\x82\xB9\xE3\x83\xA9"
            "\xE3\x82\xA4\xE3\x83\xB3"); /* 危ないベースライン */
  CHECK(disc == 1);
  CHECK_STR(disc_base("Some Album - Disc 2", &disc), "Some Album");
  CHECK(disc == 2);
  CHECK_STR(disc_base("Some Album CD3", &disc), "Some Album");
  CHECK(disc == 3);
  CHECK_STR(disc_base("Some Album (Disc 1 of 2)", &disc), "Some Album");
  CHECK(disc == 1);
  CHECK_STR(disc_base("Some Album [cd 2/2]", &disc), "Some Album");
  CHECK(disc == 2);
  CHECK_STR(disc_base("Some Album disk_4", &disc), "Some Album");
  CHECK(disc == 4);
  /* Not a disc suffix: nothing before it, a word that only ends in "cd", no number, or more
   * after it. */
  CHECK(!disc_base("Disc 1", &disc));
  CHECK(!disc_base("ABCD1", &disc));
  CHECK(!disc_base("Some Album Disc", &disc));
  CHECK(!disc_base("Some Album Disc 1 Remastered", &disc));
  CHECK(!disc_base(NULL, &disc));
}

int main(void) {
  test_folder_names();
  test_catnos();
  test_file_names();
  test_keys();
  test_disc_suffixes();
  return TEST_RESULT();
}

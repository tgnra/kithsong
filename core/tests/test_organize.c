/* Library folder layout and per-device settings. */
#include <stdio.h>
#include <string.h>

#include "musiccore/musiccore.h"
#include "tagger/organize.h"
#include "test.h"

#define CHECK_PATH(expr, want)  \
  do {                          \
    char *got_ = (expr);        \
    CHECK_STR(got_, want);      \
    if (got_ && strcmp(got_, want) != 0) fprintf(stderr, "  got \"%s\"\n", got_); \
    mc_free(got_);              \
  } while (0)

static void test_components(void) {
  CHECK_PATH(mc_path_component("AC/DC", NULL), "AC_DC");
  CHECK_PATH(mc_path_component("What? Why: \"Now\" <*|>", NULL), "What_ Why_ _Now_ ____");
  CHECK_PATH(mc_path_component("...And Justice for All", NULL), "And Justice for All");
  CHECK_PATH(mc_path_component("Hello.  ", NULL), "Hello");
  CHECK_PATH(mc_path_component("Bj\xC3\xB6rk", NULL), "Bj\xC3\xB6rk");
  CHECK_PATH(mc_path_component("", "Unknown Artist"), "Unknown Artist");
  CHECK_PATH(mc_path_component(NULL, "Unknown Album"), "Unknown Album");
  CHECK_PATH(mc_path_component("...", NULL), "_");
  /* Windows device names are reserved, with or without an extension. */
  CHECK_PATH(mc_path_component("CON", NULL), "CON_");
  CHECK_PATH(mc_path_component("nul.flac", NULL), "nul.flac_");
  CHECK_PATH(mc_path_component("Lpt1", NULL), "Lpt1_");
  CHECK_PATH(mc_path_component("Console", NULL), "Console");

  /* Long names are cut on a character boundary, never inside a UTF-8 sequence. */
  char longname[400] = "";
  for (int i = 0; i < 60; i++) strcat(longname, "\xC3\xA9"); /* 120 bytes of "é" */
  char *cut = mc_path_component(longname, NULL);
  CHECK(cut && strlen(cut) <= 100 && strlen(cut) % 2 == 0);
  mc_free(cut);
}

static void test_layout(void) {
  CHECK_PATH(mc_album_dir("Radiohead", "OK Computer", 1997), "Radiohead/OK Computer (1997)");
  CHECK_PATH(mc_album_dir("Low", "Secret Name", 0), "Low/Secret Name");
  CHECK_PATH(mc_album_dir(NULL, NULL, 0), "Unknown Artist/Unknown Album");
  CHECK_PATH(mc_album_dir("OutKast", "OutKast Singles", 0), "OutKast/OutKast Singles");

  CHECK_PATH(mc_track_file("Airbag", 1, 1, 1, "flac"), "01 Airbag.flac");
  CHECK_PATH(mc_track_file("Untitled", 2, 3, 7, "mp3"), "2-07 Untitled.mp3");
  CHECK_PATH(mc_track_file("Hey Ya!", 0, 0, 0, "mp3"), "Hey Ya!.mp3");
  CHECK_PATH(mc_track_file("Who/What?", 1, 1, 12, "flac"), "12 Who_What_.flac");
  CHECK_PATH(mc_track_file(NULL, 0, 0, 0, "flac"), "Untitled.flac");
}

static void test_settings(const char *base) {
  char dir[1024], file[1100];
  snprintf(dir, sizeof dir, "%s/test_settings", base);
  snprintf(file, sizeof file, "%s/settings.json", dir);
  remove(file);

  /* First run: nothing chosen yet, and nothing written until saved. */
  mc_settings *s = NULL;
  CHECK(mc_settings_load(dir, &s) == MC_OK);
  CHECK(mc_settings_library_dir(s) == NULL);
  CHECK(!mc_settings_shuffle(s));
  CHECK(mc_settings_set_shuffle(s, 1) == MC_OK);
  CHECK(mc_settings_volume(s) == 100);
  CHECK(mc_settings_set_volume(s, 140) == MC_OK); /* clamped */
  CHECK(mc_settings_volume(s) == 100);
  CHECK(mc_settings_set_volume(s, 35) == MC_OK);
  { /* a frontend's own key, which needn't outlive the call (Android's come from JNI) */
    char key[] = "frontend_flag";
    CHECK(mc_settings_set_bool(s, key, 1) == MC_OK);
    memset(key, 'x', sizeof key - 1);
  }
  CHECK(mc_settings_set_library_dir(s, "C:\\Users\\Me\\Music") == MC_OK);
  CHECK(mc_settings_set_library_dir(s, "D:\\M\xC3\xBAsica") == MC_OK); /* replaces */
  CHECK(mc_settings_save(s) == MC_OK);
  mc_settings_free(s);

  s = NULL;
  CHECK(mc_settings_load(dir, &s) == MC_OK);
  CHECK_STR(mc_settings_library_dir(s), "D:\\M\xC3\xBAsica");
  CHECK(mc_settings_shuffle(s));
  CHECK(mc_settings_volume(s) == 35);
  CHECK(mc_settings_bool(s, "frontend_flag", 0));
  mc_settings_free(s);

  /* Keys from a newer version survive a save; a damaged file is treated as empty. */
  FILE *f = fopen(file, "wb");
  if (f) {
    fputs("{\"library_dir\": \"/music\", \"future_key\": [1, 2]}", f);
    fclose(f);
  }
  s = NULL;
  CHECK(mc_settings_load(dir, &s) == MC_OK);
  CHECK(mc_settings_set_library_dir(s, "/srv/music") == MC_OK);
  CHECK(mc_settings_save(s) == MC_OK);
  mc_settings_free(s);
  char text[512] = "";
  if ((f = fopen(file, "rb"))) {
    size_t n = fread(text, 1, sizeof text - 1, f);
    text[n] = '\0';
    fclose(f);
  }
  CHECK(strstr(text, "future_key") != NULL);
  CHECK(strstr(text, "/srv/music") != NULL);

  if ((f = fopen(file, "wb"))) {
    fputs("{ not json", f);
    fclose(f);
  }
  s = NULL;
  CHECK(mc_settings_load(dir, &s) == MC_OK);
  CHECK(mc_settings_library_dir(s) == NULL);
  mc_settings_free(s);
}

int main(int argc, char **argv) {
  test_components();
  test_layout();
  test_settings(argc > 1 ? argv[1] : ".");
  return TEST_RESULT();
}

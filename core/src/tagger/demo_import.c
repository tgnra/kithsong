/* The demo import (kithcore/demo.h): an import part-way through, of made-up albums that
 * aren't the demo library's, for the import page's store screenshots. Fixed data handed to
 * a session that only holds it (mc_import_start_fixed), so nothing is read, looked up or
 * written. */
#include "kithcore/demo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tagger/tagger_internal.h"

enum { MAX_FILES = 13, MAX_MATCHES = 3 };

typedef struct demo_match {
  const char *album, *format, *country, *label, *catalog, *type;
  int year;
  double confidence;
  int ntracks;          /* the release's tracks: the album's first ntracks titles */
  const char *extra[4]; /* titles after the album's, for a longer release; NULL-ended */
  int files_matched;    /* files 0 ..< this are on tracks 0 ..< this; the rest are extra */
} demo_match;

typedef struct demo_item {
  mc_import_kind kind;
  mc_import_state state;
  mc_import_outcome outcome;
  const char *artist, *album;
  int year;
  int nfiles;
  const char *titles[MAX_FILES];
  int chosen;
  int nmatches;
  demo_match matches[MAX_MATCHES];
} demo_item;

/* In the list's order. Every name is made up; none is the demo library's but the duplicate's. */
static const demo_item k_items[] = {
  {MC_IMPORT_ALBUM, MC_IMPORT_DONE, MC_IMPORT_OUTCOME_NONE, "The Quiet Arboretum", "Lanterns for the Tidewater", 2022, 8,
   {"Lanterns", "Tidewater", "Moss on the Lintel", "A Window Left Open", "Ferry Lights", "Undercurrent",
    "Harbour Mouth", "The Last Lantern"},
   0, 1, {{NULL, "Digital Media", "GB", "Fernhouse Records", "FERN031", "Album", 2022, 0.98, 8, {NULL}, 8}}},
  {MC_IMPORT_ALBUM, MC_IMPORT_DONE, MC_IMPORT_OUTCOME_NONE, "Wren & Vellum", "Saltmarsh Hymnal", 2021, 7,
   {"Saltmarsh", "Hymn for the Low Road", "Reedbeds", "Brackish", "Heron, Standing", "Spring Tide", "Amen, Estuary"},
   0, 2,
   {{NULL, "CD", "IE", "Lowlight Recordings", "LLR-12", "Album", 2021, 0.95, 7, {NULL}, 7},
    {NULL, "12\" Vinyl", "IE", "Lowlight Recordings", "LLR-12LP", "Album", 2021, 0.91, 7, {NULL}, 7}}},
  {MC_IMPORT_ALBUM, MC_IMPORT_REVIEW, MC_IMPORT_OUTCOME_NONE, "Maren Ellisdottir", "Glass Orchard Sessions", 2023, 9,
   {"Glass Orchard", "Windbreak", "Pruning Song", "Cold Frame", "Quince", "Lattice", "Grafting", "First Frost",
    "Orchard at Night"},
   -1, 3,
   {{NULL, "Digital Media", "XW", "Tallgrass Audio", "TGA-2304", "Album", 2023, 0.79, 9, {"Orchard Reprise", NULL}, 9},
    {"Glass Orchard Sessions (Deluxe)", "CD", "GB", "Tallgrass Audio", "TGA-2304D", "Album", 2024, 0.64, 9,
     {"Orchard Reprise", "Windbreak (Live)", "Quince (Demo)", "Cold Frame (Acoustic)"}, 9},
    {"Glass Orchard", "Digital Media", "NO", "Tallgrass Audio", "TGA-2201", "EP", 2022, 0.41, 5, {NULL}, 5}}},
  {MC_IMPORT_ALBUM, MC_IMPORT_WRITING, MC_IMPORT_OUTCOME_NONE, "Tolly Brightwater", "Paper Comets", 2020, 10,
   {"Paper Comets", "Kite String", "Sparkler", "Night Bus Constellations", "Origami Heart", "Tailwind",
    "Fold Here", "Bright Water", "Low Orbit", "Paper Comets (Coda)"},
   0, 1, {{NULL, "CD", "US", "Littlefield Sound", "LFS-088", "Album", 2020, 0.93, 10, {NULL}, 10}}},
  {MC_IMPORT_SINGLE, MC_IMPORT_DONE, MC_IMPORT_OUTCOME_NONE, "Odette Marlowe", "Northbound Signal", 2024, 1,
   {"Northbound Signal"},
   0, 1, {{"Northbound Signal", "Digital Media", "CA", "Greywater Music", NULL, "Single", 2024, 0.96, 1, {NULL}, 1}}},
  {MC_IMPORT_ALBUM, MC_IMPORT_MATCHING, MC_IMPORT_OUTCOME_NONE, "Hollis Lamplighter", "Undertow Lullabies", 2019, 11,
   {"Undertow", "Lullaby for a Lighthouse", "Sandbar", "The Drowned Bell", "Night Swimming", "Riptide Waltz",
    "Driftwood", "Still Water", "Fog Signal", "Slack Water", "Goodnight, Shoreline"},
   -1, 0, {{.album = NULL}}},
  {MC_IMPORT_ALBUM, MC_IMPORT_PENDING, MC_IMPORT_OUTCOME_NONE, "Juniper Vale Quartet", "Afterglow Arithmetic", 2023, 6,
   {"Afterglow", "Long Division", "Remainder", "Carry the One", "Fractions of Dusk", "Sum of Evenings"},
   -1, 0, {{.album = NULL}}},
  {MC_IMPORT_ALBUM, MC_IMPORT_PENDING, MC_IMPORT_OUTCOME_NONE, "Sablefen", "Quiet Engines", 2021, 8,
   {"Ignition", "Idle", "Quiet Engines", "Flywheel", "Coolant", "Governor", "Freewheeling", "Engine Off"},
   -1, 0, {{.album = NULL}}},
  /* The demo library has it already. */
  {MC_IMPORT_ALBUM, MC_IMPORT_SKIPPED, MC_IMPORT_OUTCOME_DUPLICATE, "Ivy Sunderholt", "Copperline", 2024, 6,
   {"Copperline", "Good Bones", "Paper Towns Burn Bright", "Ladder Song", "Keep the Engine Running",
    "Copperline (Reprise)"},
   0, 1, {{NULL, "Digital Media", "XW", NULL, NULL, "Album", 2024, 0.97, 6, {NULL}, 6}}},
};
#define ITEM_COUNT ((int)(sizeof k_items / sizeof k_items[0]))

/* A track's length, 2:31 to 5:10, from its album and number. */
static int length_ms(int item, int track) {
  unsigned h = (unsigned)(item * 31 + track * 17 + 7) * 2654435761u;
  return (151 + (int)(h % 160)) * 1000;
}

/* A made-up release id, the same every run. */
static void release_id(int item, int match, char *out, size_t n) {
  snprintf(out, n, "d3m0%04x-1a7e-4b0c-9e5d-%012x", item * 16 + match, 0x5eed00 + item * 97 + match);
}

#ifdef _WIN32
#define DOWNLOADS "C:\\Users\\Alice\\Downloads\\"
#define SEP "\\"
#else
#define DOWNLOADS "/home/alice/Downloads/"
#define SEP "/"
#endif

mc_status mc_demo_import_start(mc_import_fn callback, void *user, mc_import **out) {
  if (!out) return MC_ERR_INVALID_ARG;
  *out = NULL;
  /* Snapshots in the public shape, pointing into these buffers and k_items. */
  static char folders[ITEM_COUNT][160], paths[ITEM_COUNT][MAX_FILES][256], ids[ITEM_COUNT][MAX_MATCHES][40];
  mc_import_item items[ITEM_COUNT];
  mc_import_file files[ITEM_COUNT][MAX_FILES];
  mc_import_match matches[ITEM_COUNT][MAX_MATCHES];
  mc_import_match_track tracks[ITEM_COUNT][MAX_MATCHES][MAX_FILES + 4];
  memset(items, 0, sizeof items);
  memset(files, 0, sizeof files);
  memset(matches, 0, sizeof matches);
  memset(tracks, 0, sizeof tracks);
  for (int i = 0; i < ITEM_COUNT; i++) {
    const demo_item *d = &k_items[i];
    mc_import_item *it = &items[i];
    it->kind = d->kind;
    it->state = d->state;
    it->outcome = d->outcome;
    it->artist = (char *)d->artist;
    it->album = (char *)d->album;
    it->chosen = d->chosen;
    if (d->outcome == MC_IMPORT_OUTCOME_DUPLICATE) it->duplicate_files = d->nfiles;
    if (d->kind == MC_IMPORT_SINGLE) snprintf(folders[i], sizeof folders[i], "%s", DOWNLOADS);
    else snprintf(folders[i], sizeof folders[i], DOWNLOADS "%s - %s (%d)", d->artist, d->album, d->year);
    it->folder = folders[i];
    it->files = files[i];
    it->file_count = d->nfiles;
    for (int f = 0; f < d->nfiles; f++) {
      if (d->kind == MC_IMPORT_SINGLE)
        snprintf(paths[i][f], sizeof paths[i][f], "%s%s - %s.flac", folders[i], d->artist, d->titles[f]);
      else snprintf(paths[i][f], sizeof paths[i][f], "%s" SEP "%02d %s.flac", folders[i], f + 1, d->titles[f]);
      files[i][f] = (mc_import_file){paths[i][f], (char *)d->titles[f], (char *)d->artist,
                                     d->kind == MC_IMPORT_SINGLE ? 0 : f + 1, 1, length_ms(i, f) + 400};
    }
    it->matches = matches[i];
    it->match_count = d->nmatches;
    for (int m = 0; m < d->nmatches; m++) {
      const demo_match *dm = &d->matches[m];
      mc_import_match *im = &matches[i][m];
      release_id(i, m, ids[i][m], sizeof ids[i][m]);
      im->album = im->release = (char *)(dm->album ? dm->album : d->album);
      im->album_artist = (char *)d->artist;
      im->country = (char *)dm->country;
      im->label = (char *)dm->label;
      im->catalog = (char *)dm->catalog;
      im->format = (char *)dm->format;
      im->release_type = (char *)dm->type;
      im->mb_albumid = ids[i][m];
      im->year = dm->year;
      im->is_single = d->kind == MC_IMPORT_SINGLE;
      im->confidence = dm->confidence;
      im->extra_files = d->nfiles - dm->files_matched;
      im->tracks = tracks[i][m];
      int n = dm->ntracks;
      for (int t = 0; t < n; t++)
        tracks[i][m][t] = (mc_import_match_track){(char *)d->titles[t], (char *)d->artist, 1, t + 1, length_ms(i, t),
                                                  t < dm->files_matched ? t : -1};
      for (int k = 0; k < 4 && dm->extra[k]; k++, n++)
        tracks[i][m][n] = (mc_import_match_track){(char *)dm->extra[k], (char *)d->artist, 1, n + 1,
                                                  length_ms(i, n), -1};
      im->track_count = n;
    }
  }
  return mc_import_start_fixed(items, ITEM_COUNT, callback, user, out);
}

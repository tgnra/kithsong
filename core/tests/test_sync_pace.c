/* Sync's speed and time left, on made-up sessions run against a made-up clock: what is shown
 * must stay calm through bursts, stalls, a slow start and a change of speed, follow a real
 * change, and show nothing rather than something wrong. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "musiccore/musiccore.h"
#include "sync/pace.h"
#include "test.h"

#define MB ((int64_t)1024 * 1024)
#define TICK_MS 250

/* A session under way: the bytes and the clock, and what was seen of what's shown. */
typedef struct run {
  mc_sync_pace pace;
  uint64_t now;
  int64_t moved, total;
  int total_known;
  /* From the moment watch() is called. */
  int eta_changes, eta_rises, eta_hidden, speed_changes;
  int last_eta;
  int64_t last_speed, min_speed, max_speed;
  int eta_min, eta_max;
} run;

static void step(run *r, int64_t bytes) {
  r->now += TICK_MS;
  r->moved += bytes;
  int64_t left = r->total_known ? r->total - r->moved : -1;
  mc_sync_pace_update(&r->pace, r->now, r->moved, left);
  int eta = r->pace.eta;
  int64_t speed = r->pace.speed;
  if (!eta) {
    r->eta_hidden++;
  } else {
    if (r->last_eta && eta != r->last_eta) r->eta_changes++;
    if (r->last_eta && eta > r->last_eta) r->eta_rises++;
    if (!r->eta_min || eta < r->eta_min) r->eta_min = eta;
    if (eta > r->eta_max) r->eta_max = eta;
    r->last_eta = eta;
  }
  if (speed) {
    if (r->last_speed && speed != r->last_speed) r->speed_changes++;
    if (!r->min_speed || speed < r->min_speed) r->min_speed = speed;
    if (speed > r->max_speed) r->max_speed = speed;
    r->last_speed = speed;
  }
}

static void watch(run *r) {
  r->eta_changes = r->eta_rises = r->eta_hidden = r->speed_changes = 0;
  r->last_eta = r->pace.eta;
  r->last_speed = r->pace.speed;
  r->eta_min = r->eta_max = r->pace.eta;
  r->min_speed = r->max_speed = r->pace.speed;
}

/* seconds at an even mb_s. */
static void steady(run *r, int seconds, double mb_s) {
  int ticks = seconds * 1000 / TICK_MS;
  for (int i = 0; i < ticks; i++) step(r, (int64_t)(mb_s * (double)MB * TICK_MS / 1000.0));
}

static void new_run(run *r, int64_t total) {
  memset(r, 0, sizeof *r);
  r->now = 5000000;
  r->total = total;
  r->total_known = total > 0;
  step(r, 0); /* the phase starts, nothing moved yet */
}

static int near(int64_t value, double target, double within) {
  double v = (double)value;
  return v >= target * (1.0 - within) && v <= target * (1.0 + within);
}

/* An even session: the speed is right and stays put, and the time left only counts down, a
 * step at a time, about right. */
static void test_steady(void) {
  run r;
  new_run(&r, 12000 * MB);
  steady(&r, 2, 10);
  CHECK(r.pace.speed == 0); /* warming up */
  CHECK(r.pace.eta == 0);
  steady(&r, 3, 10);
  CHECK(near(r.pace.speed, 10.0 * (double)MB, 0.02));
  CHECK(r.pace.eta == 0);
  steady(&r, 7, 10);
  CHECK(r.pace.eta == 20 * 60); /* 1188 s, on the grid */
  watch(&r);
  steady(&r, 600, 10); /* ten minutes on, 588 s left */
  CHECK(r.speed_changes == 0);
  CHECK(r.eta_rises == 0);
  CHECK(r.eta_changes <= 11);
  CHECK(r.pace.eta >= 9 * 60 && r.pace.eta <= 11 * 60);
}

/* Most of the bytes in one second of every eight, as with many small files after pauses:
 * same mean, and it mustn't show. */
static void test_bursts(void) {
  run r;
  new_run(&r, 4000 * MB);
  for (int s = 0; s < 40; s++) steady(&r, 1, s % 8 ? 2 : 66);
  watch(&r);
  for (int s = 40; s < 280; s++) steady(&r, 1, s % 8 ? 2 : 66);
  CHECK(r.eta_rises == 0);
  CHECK(r.eta_hidden == 0);
  /* The mean is 10 MB/s, the window ten seconds: one burst more or less in it. */
  CHECK(r.min_speed >= 7 * MB && r.max_speed <= 15 * MB);
  CHECK(r.eta_changes <= 8); /* in four minutes */
}

/* A short stall while the sender encodes leaves the time left alone. */
static void test_short_stalls(void) {
  run r;
  new_run(&r, 3000 * MB);
  steady(&r, 60, 10);
  watch(&r);
  for (int i = 0; i < 6; i++) {
    steady(&r, 4, 0);
    steady(&r, 20, 10);
  }
  CHECK(r.eta_rises == 0);
  CHECK(r.eta_hidden == 0);
  CHECK(r.eta_max - r.eta_min <= 3 * 60); /* in 144 s of a countdown */
  CHECK(r.min_speed >= 5 * MB + MB / 2); /* six seconds of ten is 6 MB/s */
}

/* A long one hides both, and the value comes back near where it was instead of far above. */
static void test_long_stall(void) {
  run r;
  new_run(&r, 3000 * MB);
  steady(&r, 120, 10);
  int before = r.pace.eta;
  CHECK(before > 0);
  watch(&r);
  steady(&r, 8, 0);
  CHECK(r.pace.eta == before); /* still held */
  CHECK(r.pace.speed > 0 && r.pace.speed < 10 * MB);
  steady(&r, 22, 0);
  CHECK(r.pace.speed == 0); /* nothing moving: nothing to say */
  CHECK(r.pace.eta == 0);
  steady(&r, 3, 10);
  CHECK(r.pace.speed > 0);
  CHECK(r.pace.eta == before);
  watch(&r);
  steady(&r, 60, 10);
  CHECK(r.eta_rises <= 1); /* the stall took time: it may say so once */
  CHECK(r.eta_max <= before * 3 / 2);
}

/* A slow start, as with a new connection: what's first shown is too long, and it only comes
 * down. */
static void test_slow_start(void) {
  run r;
  new_run(&r, 3000 * MB);
  steady(&r, 6, 1);
  steady(&r, 6, 10);
  CHECK(r.pace.eta > 0);
  watch(&r);
  steady(&r, 240, 10);
  CHECK(r.eta_rises == 0);
  /* 2940 MB left at the first reading, so 54 s of them gone by 60 and so on. */
  CHECK(near(r.pace.eta, 2700.0 / 10.0 - 30.0, 0.2) || r.pace.eta <= 5 * 60);
}

/* Many small files and then a few large ones: faster later, so it falls and never climbs. */
static void test_small_then_large(void) {
  run r;
  new_run(&r, 2000 * MB);
  for (int s = 0; s < 120; s++) steady(&r, 1, s % 3 ? 0.6 : 2.4);
  CHECK(r.pace.eta > 0);
  int before = r.pace.eta;
  watch(&r);
  steady(&r, 100, 18);
  CHECK(r.eta_rises == 0);
  CHECK(r.pace.eta < before / 2);
  CHECK(near(r.pace.speed, 18.0 * (double)MB, 0.05));
}

/* A real drop in speed: the time left does climb, but in a step or two, not a crawl, and
 * ends up about right. */
static void test_speed_drop(void) {
  run r;
  new_run(&r, 6000 * MB);
  steady(&r, 120, 20);
  int before = r.pace.eta;
  CHECK(before >= 3 * 60 && before <= 5 * 60 + 30);
  watch(&r);
  steady(&r, 12, 5);
  CHECK(r.eta_rises == 0); /* too early to believe */
  CHECK(near(r.pace.speed, 5.0 * (double)MB, 0.1)); /* the speed has followed already */
  steady(&r, 108, 5);
  CHECK(r.eta_rises >= 1 && r.eta_rises <= 2);
  /* 3000 MB left, at 5 MB/s. */
  double truth = (6000.0 - 2400.0 - 600.0) / 5.0;
  CHECK(near(r.pace.eta, truth, 0.25));
}

/* The speed doesn't flicker over noise of a few percent. */
static void test_noise(void) {
  run r;
  new_run(&r, 20000 * MB);
  steady(&r, 30, 10);
  watch(&r);
  uint32_t x = 12345;
  for (int i = 0; i < 2400; i++) {
    x = x * 1664525u + 1013904223u;
    /* 10 MB/s, each quarter second off by up to 50%. */
    double factor = 0.5 + (double)(x >> 8 & 0xffff) / 65535.0;
    step(&r, (int64_t)(10.0 * (double)MB * factor * TICK_MS / 1000.0));
  }
  CHECK(r.speed_changes <= 15); /* in ten minutes of noise this bad */
  CHECK(r.eta_rises == 0);
  CHECK(near(r.min_speed, 10.0 * (double)MB, 0.2));
  CHECK(near(r.max_speed, 10.0 * (double)MB, 0.2));
}

/* No total, no time left; a trickle, no time left either. */
static void test_nothing_to_say(void) {
  run r;
  new_run(&r, 0);
  steady(&r, 60, 10);
  CHECK(r.pace.speed > 0);
  CHECK(r.pace.eta == 0);

  new_run(&r, 1000 * MB);
  steady(&r, 60, 0.01);
  CHECK(r.pace.speed == 0);
  CHECK(r.pace.eta == 0);

  /* An estimate that says thousands of hours is none. */
  new_run(&r, 100000000 * MB);
  steady(&r, 60, 0.1);
  CHECK(r.pace.speed > 0);
  CHECK(r.pace.eta == 0);

  /* Nothing moved at all. */
  new_run(&r, 1000 * MB);
  steady(&r, 60, 0);
  CHECK(r.pace.speed == 0);
  CHECK(r.pace.eta == 0);
}

/* The end of a turn, then the peer's turn with a work of its own at its own speed: the time
 * left goes, and comes back after a fresh warm-up, not from the last turn's speed. */
static void test_next_turn(void) {
  run r;
  new_run(&r, 600 * MB);
  steady(&r, 30, 10);
  CHECK(r.pace.eta > 0);
  steady(&r, 30, 10);
  CHECK(r.moved >= r.total);
  CHECK(r.pace.eta == 0);
  steady(&r, 12, 0); /* between turns */
  CHECK(r.pace.speed == 0);
  r.total = r.moved + 900 * MB;
  steady(&r, 7, 2);
  CHECK(r.pace.eta == 0);
  steady(&r, 4, 2);
  CHECK(near(r.pace.eta, 900.0 / 2.0, 0.2));
  CHECK(near(r.pace.speed, 2.0 * (double)MB, 0.1));
}

/* A session with another peer is a new session, and starts over: what the link to the last
 * one did is no guide to this one. */
static void test_next_peer(void) {
  run a, b;
  new_run(&a, 3000 * MB);
  steady(&a, 60, 40);
  new_run(&b, 3000 * MB);
  CHECK(b.pace.speed == 0 && b.pace.eta == 0);
  steady(&b, 12, 2);
  CHECK(near(b.pace.speed, 2.0 * (double)MB, 0.1));
  CHECK(near(b.pace.eta, 3000.0 / 2.0, 0.1));
}

/* A MB is 1024 * 1024, as in the sizes the app shows, and the grid coarsens. */
static void test_units_and_grid(void) {
  run r;
  new_run(&r, 200000 * MB);
  steady(&r, 20, 12.34);
  CHECK(r.pace.speed % (MB / 10) == 0);
  CHECK(near(r.pace.speed, 12.3 * (double)MB, 0.01));

  /* Hours: a coarse step. */
  CHECK(r.pace.eta > 3 * 3600);
  CHECK(r.pace.eta % 1800 == 0);

  new_run(&r, 200 * MB);
  steady(&r, 12, 150);
  /* All of it gone by now; the speed had a whole MB for a step. */
  new_run(&r, 200000 * MB);
  steady(&r, 12, 150.4);
  CHECK(r.pace.speed == 150 * MB);
}

static void text_of(int64_t speed, int eta, const char *want) {
  mc_sync_progress p;
  memset(&p, 0, sizeof p);
  p.speed = speed;
  p.eta_seconds = eta;
  char out[128];
  mc_sync_pace_text(&p, out, sizeof out);
  if (strcmp(out, want)) fprintf(stderr, "got \"%s\", wanted \"%s\"\n", out, want);
  CHECK(!strcmp(out, want));
}

static void test_text(void) {
  mc_strings_set_locale("en");
  text_of(0, 0, "");
  text_of(MB * 83 / 10, 0, "8.3 MB/s");
  text_of(0, 720, "about 12 min left");
  text_of(MB * 83 / 10, 720, "8.3 MB/s \xC2\xB7 about 12 min left");
  text_of(150 * MB, 30, "150 MB/s \xC2\xB7 less than a minute left");
  text_of(MB, 60, "1.0 MB/s \xC2\xB7 about 1 min left");
  text_of(MB, 3600, "1.0 MB/s \xC2\xB7 about 1 h left");
  text_of(MB, 4800, "1.0 MB/s \xC2\xB7 about 1 h 20 min left");
}

int main(void) {
  test_steady();
  test_bursts();
  test_short_stalls();
  test_long_stall();
  test_slow_start();
  test_small_then_large();
  test_speed_drop();
  test_noise();
  test_nothing_to_say();
  test_next_turn();
  test_next_peer();
  test_units_and_grid();
  test_text();
  return TEST_RESULT();
}

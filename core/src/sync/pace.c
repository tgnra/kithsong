#include "sync/pace.h"

#include <stdio.h>

#include "musiccore/musiccore.h"

/* A MB as everywhere else in the app: sizes count in 1024s. */
#define MB ((int64_t)1024 * 1024)
#define BIN_MS 1000

/* Counts of bins are seconds. */
#define SPEED_WARMUP 3          /* bins before there's a speed */
#define SPEED_HOLD 2            /* bins between changes */
#define SPEED_MIN (MB / 20)     /* less would read 0.0 MB/s: show nothing */
#define ETA_WARMUP 8            /* bins before there's a time left, and again after new work */
#define ETA_HOLD 5              /* bins between changes on the way down */
#define ETA_CLIMB_HOLD 15       /* bins the estimate must stand high for the value to climb */
#define ETA_CLIMB_MARGIN 1.3    /* how high that is, over the value shown, plus a step */
#define ETA_MAX (7 * 24 * 3600) /* an answer past this is no answer */
#define SLOW_KEEP 0.97802       /* exp(-1/45): a horizon of 45 s */
#define SHIFT_RATIO 1.3         /* the speed this far from the longer mean, ... */
#define SHIFT_HOLD 20           /* ... for this many bins, is a new speed */
#define NEW_WORK MB             /* left growing by less is noise in the estimates */
#define MAX_GAP 600             /* bins counted for one gap between updates: all gone by then */

/* The step of the grid at x seconds: coarser as the wait gets longer. */
static int eta_step(int x) {
  if (x < 20 * 60) return 60;
  if (x < 3600) return 5 * 60;
  if (x < 3 * 3600) return 10 * 60;
  if (x < 10 * 3600) return 30 * 60;
  return 3600;
}

static int eta_round(int x) {
  if (x < 50) return 30; /* less than a minute */
  int step = eta_step(x);
  return (x + step / 2) / step * step;
}

static int64_t speed_round(double bytes) {
  int64_t step = bytes < 100.0 * (double)MB ? MB / 10 : MB;
  return ((int64_t)bytes + step / 2) / step * step;
}

static void close_bin(mc_sync_pace *p, int64_t bytes) {
  int at = (int)(p->bins % MC_SYNC_PACE_WINDOW);
  p->window_sum += bytes - p->window[at];
  p->window[at] = bytes;
  p->bins++;
  p->slow_bytes = p->slow_bytes * SLOW_KEEP + (double)bytes;
  p->slow_bins = p->slow_bins * SLOW_KEEP + 1.0;
}

static void update_speed(mc_sync_pace *p) {
  if (p->bins < SPEED_WARMUP) return;
  uint64_t n = p->bins < MC_SYNC_PACE_WINDOW ? p->bins : MC_SYNC_PACE_WINDOW;
  double raw = (double)p->window_sum / (double)n;
  if (raw < (double)SPEED_MIN) {
    p->speed = 0;
    return;
  }
  double shown = (double)p->speed;
  double apart = raw > shown ? raw - shown : shown - raw;
  double margin = shown / 10.0;
  if (margin < 0.15 * (double)MB) margin = 0.15 * (double)MB;
  if (!p->speed || (apart >= margin && p->bins - p->speed_bin >= SPEED_HOLD)) {
    p->speed = speed_round(raw);
    p->speed_bin = p->bins;
  }
}

static void set_held(mc_sync_pace *p, int raw) {
  p->held = eta_round(raw);
  p->held_bin = p->bins;
  p->climbing = 0;
}

/* A speed that has really changed: the longer mean would take minutes to believe it, so it
 * starts again from the speed. A window that swings either way around it does not count. */
static void follow_shift(mc_sync_pace *p) {
  double fast = (double)p->window_sum / MC_SYNC_PACE_WINDOW, slow = p->slow_bytes / p->slow_bins;
  int way = fast > slow * SHIFT_RATIO ? 1 : fast * SHIFT_RATIO < slow ? -1 : 0;
  p->shifted = way && way == p->shift_way ? p->shifted + 1 : 0;
  p->shift_way = way;
  if (p->shifted >= SHIFT_HOLD) {
    p->slow_bytes = (double)p->window_sum;
    p->slow_bins = MC_SYNC_PACE_WINDOW;
    p->shifted = 0;
  }
}

static void update_eta(mc_sync_pace *p, int64_t left) {
  p->eta = 0;
  /* A stall: nothing to go on until it ends, when the value held goes on from where it was.
   * Its bins stay in the mean, as stalls that keep coming do take time. */
  if (!p->speed) {
    p->climbing = 0;
    return;
  }
  if (p->bins - p->eta_since < ETA_WARMUP) return;
  follow_shift(p);
  double rate = p->slow_bytes / p->slow_bins;
  if (rate < (double)SPEED_MIN) return;
  double seconds = (double)left / rate;
  if (seconds > ETA_MAX) {
    p->held = 0;
    return;
  }
  int raw = (int)seconds;
  if (!p->held) {
    set_held(p, raw);
  } else {
    /* Down as the clock runs, a quarter step past where rounding would go, so an estimate
     * sitting on the edge doesn't flicker. Up only if it stands well above for a while. */
    int step = eta_step(p->held);
    if (eta_round(raw + step / 4) < p->held) {
      p->climbing = 0;
      if (p->bins - p->held_bin >= ETA_HOLD) set_held(p, raw);
    } else if (raw > (double)p->held * ETA_CLIMB_MARGIN + step) {
      if (++p->climbing >= ETA_CLIMB_HOLD) set_held(p, raw);
    } else {
      p->climbing = 0;
    }
  }
  p->eta = p->held;
}

void mc_sync_pace_update(mc_sync_pace *p, uint64_t now_ms, int64_t moved, int64_t left) {
  if (left < 0) left = 0;
  /* New work, such as the second turn's: what the last one ran at says little of it. */
  if (left > p->left + NEW_WORK) {
    p->slow_bytes = p->slow_bins = 0.0;
    p->eta_since = p->bins;
    p->held = p->eta = p->climbing = 0;
  }
  p->left = left;
  if (!left) p->held = p->eta = p->climbing = 0;

  /* The clock starts with the first bytes: waiting for the first encode says nothing of the
   * link. */
  if (!p->started) {
    if (moved > 0) {
      p->started = 1;
      p->bin_start = now_ms;
      p->moved = moved;
    }
    return;
  }
  if (moved > p->moved) {
    p->bin_bytes += moved - p->moved;
    p->moved = moved;
  }
  if (now_ms < p->bin_start + BIN_MS) return;
  uint64_t gone = (now_ms - p->bin_start) / BIN_MS;
  p->bin_start += gone * BIN_MS;
  if (gone > MAX_GAP) gone = MAX_GAP;
  /* What came in a gap between updates came at its end, as far as anyone can tell. */
  for (; gone > 1; gone--) close_bin(p, 0);
  close_bin(p, p->bin_bytes);
  p->bin_bytes = 0;
  update_speed(p);
  if (left) update_eta(p, left);
}

void mc_sync_pace_text(const mc_sync_progress *p, char *out, size_t cap) {
  char speed[48] = "", eta[64] = "";
  if (p->speed > 0) {
    char num[32];
    double mb = (double)p->speed / (double)MB;
    snprintf(num, sizeof num, mb < 100.0 ? "%.1f" : "%.0f", mb);
    mc_format(speed, sizeof speed, mc_str(MC_STR_SYNC_SPEED), MC_S(num));
  }
  if (p->eta_seconds > 0 && p->eta_seconds < 60) {
    snprintf(eta, sizeof eta, "%s", mc_str(MC_STR_SYNC_LEFT_UNDER_MINUTE));
  } else if (p->eta_seconds > 0) {
    int minutes = p->eta_seconds / 60, hours = minutes / 60;
    minutes %= 60;
    if (!hours)
      mc_format(eta, sizeof eta, mc_str_plural(MC_STR_SYNC_LEFT_MINUTES, minutes), MC_D(minutes));
    else if (!minutes)
      mc_format(eta, sizeof eta, mc_str_plural(MC_STR_SYNC_LEFT_HOURS, hours), MC_D(hours));
    else
      mc_format(eta, sizeof eta, mc_str(MC_STR_SYNC_LEFT_HOURS_MINUTES), MC_D(hours), MC_D(minutes));
  }
  snprintf(out, cap, "%s%s%s", speed, speed[0] && eta[0] ? mc_str(MC_STR_SEPARATOR_DOT) : "", eta);
}

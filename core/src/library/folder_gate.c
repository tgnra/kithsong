#include "library/folder_gate.h"

#include <stdint.h>
#include <stdlib.h>

#include "util/platform.h"

typedef struct gate {
  struct gate *next;
  char *dir;
  char *config_dir;
  int holds;
  int watched;
  uint64_t arrivals; /* holds ever taken */
} gate;

struct mc_folder_watch {
  gate *g;
  int own_holds;
  uint64_t arrivals;
};

/* A few instructions under it, and it must work with nothing set up: a test needn't call
 * mc_init(), and nothing here can wait on the mutexes that need it. */
static char g_spin;
static gate *g_gates;

static void lock(void) {
  while (__atomic_test_and_set(&g_spin, __ATOMIC_ACQUIRE)) mc_sleep_ms(0);
}

static void unlock(void) {
  __atomic_clear(&g_spin, __ATOMIC_RELEASE);
}

/* dir's gate, made if make is set, or NULL. Under lock. */
static gate *find(const char *dir, int make) {
  for (gate *g = g_gates; g; g = g->next)
    if (mc_same_path(g->dir, dir)) return g;
  if (!make) return NULL;
  gate *g = calloc(1, sizeof *g);
  if (g && !(g->dir = mc_strdup(dir))) {
    free(g);
    return NULL;
  }
  if (g) {
    g->next = g_gates;
    g_gates = g;
  }
  return g;
}

/* Under lock. */
static void forget_if_idle(gate *g) {
  if (g->holds || g->watched || g->config_dir) return;
  for (gate **at = &g_gates; *at; at = &(*at)->next)
    if (*at == g) {
      *at = g->next;
      break;
    }
  free(g->dir);
  free(g->config_dir);
  free(g);
}

void mc_folder_hold(const char *dir) {
  if (!dir) return;
  lock();
  gate *g = find(dir, 1);
  if (g) {
    g->holds++;
    g->arrivals++;
  }
  unlock();
}

void mc_folder_release(const char *dir) {
  if (!dir) return;
  lock();
  gate *g = find(dir, 0);
  if (g) {
    if (g->holds) g->holds--;
    forget_if_idle(g);
  }
  unlock();
}

mc_folder_watch *mc_folder_watch_begin(const char *dir, int own_holds) {
  mc_folder_watch *w = dir ? calloc(1, sizeof *w) : NULL;
  if (!w) return NULL;
  lock();
  gate *g = find(dir, 1);
  int free_to_go = g && !g->watched && g->holds <= own_holds;
  if (free_to_go) {
    g->watched = 1;
    w->g = g;
    w->own_holds = own_holds;
    w->arrivals = g->arrivals;
  } else if (g) {
    forget_if_idle(g);
  }
  unlock();
  if (free_to_go) return w;
  free(w);
  return NULL;
}

int mc_folder_watch_clear(mc_folder_watch *w) {
  lock();
  int clear = w->g->arrivals == w->arrivals && w->g->holds <= w->own_holds;
  unlock();
  return clear;
}

void mc_folder_watch_end(mc_folder_watch *w) {
  if (!w) return;
  lock();
  w->g->watched = 0;
  forget_if_idle(w->g);
  unlock();
  free(w);
}

void mc_folder_note_config(const char *dir, const char *config_dir) {
  if (!dir) return;
  char *copy = mc_strdup(config_dir);
  lock();
  gate *g = find(dir, 1);
  if (g) {
    free(g->config_dir);
    g->config_dir = copy;
    copy = NULL;
    forget_if_idle(g);
  }
  unlock();
  free(copy);
}

char *mc_folder_config(const char *dir) {
  if (!dir) return NULL;
  lock();
  gate *g = find(dir, 0);
  char *copy = g ? mc_strdup(g->config_dir) : NULL;
  unlock();
  return copy;
}

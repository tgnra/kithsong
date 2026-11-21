/* Generates a stress test library from the command line, for scripts and for timing the
 * generator on its own: manual_stress <folder> <1k|10k|100k>. */
#include <stdio.h>
#include <string.h>

#include "kithcore/kithcore.h"
#include "util/platform.h"

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: manual_stress <folder> <1k|10k|100k>\n");
    return 2;
  }
  mc_stress_preset preset = !strcmp(argv[2], "1k") ? MC_STRESS_1K : !strcmp(argv[2], "100k") ? MC_STRESS_100K : MC_STRESS_10K;
  uint64_t start = mc_perf_now_us();
  mc_stress_job *job = NULL;
  mc_status st = mc_stress_start(argv[1], preset, &job);
  if (st != MC_OK) {
    fprintf(stderr, "couldn't start: %s\n", mc_status_str(st));
    return 1;
  }
  while (!mc_stress_job_done(job, &st)) mc_sleep_ms(20);
  mc_stress_job_free(job);
  printf("%s: %.2f s\n", st == MC_OK ? "generated" : mc_status_str(st), (double)(mc_perf_now_us() - start) / 1e6);
  return st == MC_OK ? 0 : 1;
}

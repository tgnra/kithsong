/* The demo library from the command line (kithcore/demo.h): manual_demo <folder> writes the
 * tagged files and covers into an empty folder, for importing by hand (an Android zip, say);
 * manual_demo --library <folder> makes the library itself there. */
#include <stdio.h>
#include <string.h>

#include "kithcore/kithcore.h"

int main(int argc, char **argv) {
  int library = argc == 3 && !strcmp(argv[1], "--library");
  if (argc != 2 && !library) {
    fprintf(stderr, "usage: manual_demo [--library] <folder>\n");
    return 2;
  }
  mc_init();
  uint64_t start = mc_perf_now_us();
  mc_status st = library ? mc_demo_create(argv[2], NULL) : mc_demo_write_folder(argv[1]);
  printf("%s: %.2f s\n", st == MC_OK ? "written" : mc_status_str(st), (double)(mc_perf_now_us() - start) / 1e6);
  mc_shutdown();
  return st == MC_OK ? 0 : 1;
}

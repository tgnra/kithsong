/* Linked into every test: before main, points every trash hook the core has (sync's, the
 * duplicates check's and deletion's) at a stand-in that moves the file into the test build
 * folder's test-trash/<test>/ instead of the real Recycle Bin or trash, so a test that
 * doesn't set its own can't fill the user's. A test that checks what was trashed sets its
 * own after. */
#include <stdio.h>
#include <string.h>

#include "library/delete_internal.h"
#include "sync/transfer.h"
#include "tagger/dupes.h"
#include "util/platform.h"

static mc_status test_trash(const char *path) {
  static int n; /* two devices' sessions in one test may trash at once */
  int i = __atomic_fetch_add(&n, 1, __ATOMIC_RELAXED);
  const char *name = strrchr(path, '/'), *sep = strrchr(path, MC_PATH_SEP);
  if (sep > name) name = sep;
  char to[2048];
  mc_mkdirs(MC_TEST_TRASH_DIR);
  /* Replacing what an earlier run left. */
  snprintf(to, sizeof to, "%s%c%d-%s", MC_TEST_TRASH_DIR, MC_PATH_SEP, i, name ? name + 1 : path);
  return mc_rename(path, to, 1);
}

__attribute__((constructor)) static void use_test_trash(void) {
  mc_transfer_trash = test_trash;
  mc_dupes_trash = test_trash;
  mc_delete_trash = test_trash;
}

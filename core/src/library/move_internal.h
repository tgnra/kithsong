/* Test hooks for library_move.c. Tests set them before calling in; nothing else does. */
#ifndef MC_LIBRARY_MOVE_INTERNAL_H
#define MC_LIBRARY_MOVE_INTERNAL_H

#include <stdint.h>

/* >= 0: the destination's free space, in place of the volume's. */
extern int64_t mc_move_test_free_space;
/* > 0: copying the nth file (from 1) fails, as a full disk or a vanished file would. */
extern int mc_move_test_fail_at;
/* Copy even on the same volume. */
extern int mc_move_test_no_rename;

#endif

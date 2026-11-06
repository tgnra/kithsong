/* Settings' Testing group: an expander row, closed to begin with, holding the profiler
 * (kithcore/perf.h) with its report, and stress test libraries (kithcore/stress.h).
 *
 *   |  Testing                                       v |
 *   |    Profile the app's speed                  [==] |
 *   |    Run Scroll Test                               |
 *   |    Copy Report                                   |
 *   |    Save Report...                                |
 *   |    Clear                                         |
 *   |    Stress test library        [10000 tracks  v]  |
 *   |    Generate and Open                             |
 *   |    Back to My Library                            |
 *   |    Delete Stress Libraries...                    |
 *   |    the report, selectable                        |
 *
 * Generating goes on if the dialog closes, and opens the library when it's done. */
#ifndef KITHSONG_TESTING_VIEW_H
#define KITHSONG_TESTING_VIEW_H

#include <adwaita.h>

#include "app.h"

/* The group for Settings' page. */
GtkWidget *testing_view_group(app *a);
/* The next group made starts open, for showing the report after a scroll test, or the way back
 * in a stress library. */
void testing_view_open_next(void);

#endif

/* What the GTK frontend feeds the profiler (musiccore/perf.h), all from the window's frame
 * clock: a frame in which a watched list moved is a frame of scrolling, and a view switch or
 * startup's first content is painted once a frame has finished after it. Also the scroll test,
 * which scrolls the main views by the core's fixed script, one step per frame. */
#ifndef KITHSONG_PERF_UI_H
#define KITHSONG_PERF_UI_H

#include <adwaita.h>

#include "app.h"

/* Call once the window exists: starts listening to its frame clock (once it's realized). */
void perf_ui_init(app *a);
/* Reports the frames in which scrolled (a GtkScrolledWindow) moves, under series. Costs one
 * flag per scroll while the profiler's off. name is kept. */
void perf_ui_watch(GtkWidget *scrolled, const char *series);
/* A view switch to view began; it ends with the next frame painted. */
void perf_ui_view(const char *view);
/* The library's first listing is done: the next frame painted is startup's first content. */
void perf_ui_content_ready(void);

/* Runs the scroll test; a no-op while the profiler's off or a test runs. Ends by showing
 * Settings with the report, or with KITHSONG_PERF_RUN set, by saving there and quitting. */
void perf_ui_run_scroll_test(void);

#endif

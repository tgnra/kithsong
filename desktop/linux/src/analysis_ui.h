/* The audio analysis of the library that's open, and what shows of it. A small batch runs flat
 * out with nothing shown. A large one opens a dialog with a progress bar, Background (carry on
 * paced, with a button in the header bar next to Settings, which opens the dialog again) and
 * Cancel (stop, over restarts too, until Settings' Resume analysis). Background is remembered
 * until the work is done, so a restart doesn't ask again. */
#ifndef KITHSONG_ANALYSIS_UI_H
#define KITHSONG_ANALYSIS_UI_H

#include "app.h"

/* It comes and goes with sync: devices_start() and devices_stop(). */
void analysis_ui_start(app *a);
void analysis_ui_stop(void);
/* The header bar button's: opens the dialog. */
void analysis_ui_present(app *a);
/* For Settings: whether Cancel has stopped it, the tracks waiting, and Resume analysis, which
 * carries on in the background. Counting again is a query, so ask for it when opening. */
gboolean analysis_ui_stopped(void);
int analysis_ui_left(gboolean count_again);
void analysis_ui_resume(void);

#endif

/* The audio analysis of the library that's open, and what shows of it. A small batch runs flat
 * out with nothing shown. A large one opens a dialog with a progress bar, Background (carry on
 * paced, with a button in the header bar next to Settings, which opens the dialog again) and
 * Cancel (stop until the app next starts). Background is remembered until the work is done, so
 * a restart doesn't ask again. */
#ifndef KITHSONG_ANALYSIS_UI_H
#define KITHSONG_ANALYSIS_UI_H

#include "app.h"

/* It comes and goes with sync: devices_start() and devices_stop(). */
void analysis_ui_start(app *a);
void analysis_ui_stop(void);
/* The header bar button's: opens the dialog. */
void analysis_ui_present(app *a);

#endif

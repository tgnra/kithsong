/* The first run: before there's a library, a dialog over the main window asks where it should
 * live, the same steps as Windows' first_run.c:
 *
 *   +-------------------------------------------+
 *   |            Welcome to Kithsong          x |
 *   |                  (logo)                   |
 *   |      Choose a home for your library       |
 *   |     Kithsong copies the music you ...     |
 *   |  +-------------------------------------+  |
 *   |  | ( ) Suggested                       |  |
 *   |  |     ~/Music/Kithsong Library        |  |
 *   |  | ( ) Your folder                     |  |  <- once one was chosen
 *   |  |     /mnt/big/music                  |  |
 *   |  | [] Choose a folder…                 |  |
 *   |  +-------------------------------------+  |
 *   |                 ( Start )                 |
 *   +-------------------------------------------+
 *
 * The suggestion is Kithsong Library in the user's Music folder. A folder the user chose that
 * already has other things in it gets a Kithsong Library folder inside it. Start stays greyed
 * out until a row is picked. Closing the dialog quits, as nothing can go on without a library.
 *
 * Needs libadwaita 1.5 (AdwDialog).
 */
#ifndef KITHSONG_FIRST_RUN_H
#define KITHSONG_FIRST_RUN_H

#include <adwaita.h>

/* Called once the dialog has gone, with the folder chosen, which is empty, doesn't exist yet or
 * already holds a library; NULL if the user closed it. */
typedef void (*first_run_done_fn)(void *user, const char *dir);

/* Shows the dialog over parent (the main window). */
void first_run_present(GtkWidget *parent, first_run_done_fn done, void *user);

#endif

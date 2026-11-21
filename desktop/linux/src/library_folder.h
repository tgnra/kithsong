/* Whether a folder will do for the library, for every route that picks one or starts with
 * one: mc_library_folder_check(), with the Flatpak's reach when the app runs in one. Inside the
 * sandbox only the Music folder (--filesystem=xdg-music) and the app's own data folder
 * ($XDG_DATA_HOME, ~/.var/app/ng.kithso.Kithsong/data on the host) last; a folder from
 * anywhere else comes through the file chooser portal as a /run/user/<uid>/doc path. Outside a
 * Flatpak any folder may do, as long as it can be written and its free space read.
 */
#ifndef KITHSONG_LIBRARY_FOLDER_H
#define KITHSONG_LIBRARY_FOLDER_H

#include <adwaita.h>

#include "kithcore/kithcore.h"

gboolean library_folder_sandboxed(void);
/* The user's Music folder, whether or not it exists yet; g_free it. */
char *library_folder_music(void);
/* Kithsong Library in the app's own data folder; g_free it. */
char *library_folder_own(void);

mc_folder_problem library_folder_check(const char *dir, mc_folder_use use);

/* Opens a folder picker where it should start: the Music folder in a Flatpak, else from
 * (which may be NULL, leaving it to the picker). */
void library_folder_start_in(GtkFileDialog *chooser, const char *from);

typedef enum library_folder_answer {
  LIBRARY_FOLDER_AGAIN, /* choose another folder */
  LIBRARY_FOLDER_OWN,   /* use library_folder_own(), passed as own */
  LIBRARY_FOLDER_CANCEL
} library_folder_answer;
typedef void (*library_folder_answer_fn)(gpointer data, library_folder_answer answer, const char *own);

/* Says why dir won't do (problem, from library_folder_check(dir, use)) over parent, with
 * Cancel and Again: Choose in Music after a problem of reach, else Choose a folder. In a Flatpak
 * it also offers Kithsong's own storage, unless the problem is what dir holds or own storage
 * fails the check for use too. heading NULL is Can't use that folder, body NULL is
 * library_folder_why(), again NULL as above. done may be NULL. */
void library_folder_explain(GtkWidget *parent, const char *heading, const char *body, mc_folder_problem problem,
                            const char *dir, mc_folder_use use, const char *again, library_folder_answer_fn done,
                            gpointer data);

/* Why dir won't do, for library_folder_explain()'s body or one of its own; g_free it. */
char *library_folder_why(mc_folder_problem problem, const char *dir, mc_folder_use use);

#endif

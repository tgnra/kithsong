/* Who's working in a library folder, so the orphan sweep (sync/orphans.h) never runs beside
 * anything that puts a file there before a files row names it. Imports, edits, deletes, a
 * merge, a library move and every sync session hold the folder while they run; the sweep
 * watches it. Holding never blocks, so it's fine on a UI thread: a sweep that finds the folder
 * held doesn't start, and one that sees someone arrive stops before its next removal. A file
 * made after that can't be in its list, as its walk had finished by then. In this process
 * only; see the top of orphans.c for two apps on one library. Keyed by folder, as a process
 * can have several libraries open (tests do). */
#ifndef MC_LIBRARY_FOLDER_GATE_H
#define MC_LIBRARY_FOLDER_GATE_H

/* Each hold needs one release, with the folder named the same. NULL does nothing. */
void mc_folder_hold(const char *dir);
void mc_folder_release(const char *dir);

typedef struct mc_folder_watch mc_folder_watch;

/* Starts watching dir, or NULL if anyone but the caller's own own_holds holds it, or it's
 * being watched already. */
mc_folder_watch *mc_folder_watch_begin(const char *dir, int own_holds);
/* Whether nobody has come since mc_folder_watch_begin(). */
int mc_folder_watch_clear(mc_folder_watch *w);
void mc_folder_watch_end(mc_folder_watch *w);

/* Sync says which config folder goes with a library it runs on, and NULL when it stops, so a
 * sweep that doesn't start from a session (after an import) can find sync.db and the recycle
 * bin's records. mc_folder_config() gives it malloc'd, or NULL if sync isn't running here. */
void mc_folder_note_config(const char *dir, const char *config_dir);
char *mc_folder_config(const char *dir);

#endif

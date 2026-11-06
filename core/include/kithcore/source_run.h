/* Running from the source code: a build started from inside a checkout, as opposed to an
 * installed app. Such a run keeps away from the real thing, on the desktops: settings in a
 * folder of their own (MC_SOURCE_SETTINGS_DIR next to the usual one), and a library the app
 * makes itself under the system's temp folder, so the first run never offers the Music folder.
 * Settings' Reset Kithsong… deletes that library for good too.
 *
 * It goes by where the executable is, not by how it was built: a release build in the build
 * folder counts, and a debug build copied elsewhere doesn't. Everything here fails closed. If
 * something can't be read or doesn't match, the run is an installed one, and a library is
 * the user's. Android has no executable to go by and doesn't use this. */
#ifndef KITHCORE_SOURCE_RUN_H
#define KITHCORE_SOURCE_RUN_H

#include "kithcore/kithcore.h"
#include "kithcore/settings.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The settings folder's name, in place of "kithsong". */
#define MC_SOURCE_SETTINGS_DIR "kithsong-debug"
/* The library's folder in the temp folder: this and a hash of the settings folder. */
#define MC_SOURCE_LIBRARY_PREFIX "kithsong-debug-library-"
/* In a library this made. Without it, Reset leaves the folder alone. Named as the orphan sweep
 * leaves things be (orphans.c), or the sweep would bin it. */
#define MC_SOURCE_LIBRARY_MARKER ".kithsong-debug-library"

/* Whether this process's executable is inside a checkout (mc_source_root()). A few file checks,
 * so call it once at startup rather than often. */
int mc_source_run(void);

/* The checkout that dir is in, going up from dir itself a few levels, or NULL. A checkout has
 * all of a list of this repository's files; .git isn't one of them, as it's a file in a
 * worktree and missing from a source archive. malloc'd; mc_free it. */
char *mc_source_root(const char *dir);

/* The library folder for a run with settings in settings_dir. Each settings folder has its
 * own, so two --config-dir instances never share one. malloc'd; mc_free it. NULL on failure. */
char *mc_source_library_dir(const char *settings_dir);

/* For a source run, after loading settings: with no library chosen, makes
 * mc_source_library_dir() and saves it as the library. Also makes it again if settings name it
 * and the system has cleared the temp folder. A library chosen elsewhere is left as it is. */
mc_status mc_source_library_prepare(const char *settings_dir, mc_settings *settings);

/* Whether Reset would delete library_dir: only from_source, and only if it's the folder
 * mc_source_library_prepare() made for settings_dir. The two are compared with links resolved,
 * neither may be a link or junction, and the marker must be there. Anything else is someone's
 * library, and Reset's confirmation shouldn't mention it. */
int mc_source_reset_deletes_library(int from_source, const char *settings_dir, const char *library_dir);

/* mc_settings_reset_if_requested(), which is all it is unless from_source. Then, once settings
 * have been cleared, the library goes too if mc_source_reset_deletes_library() said so of the
 * library they named: outright, with no Recycle Bin, and links in it removed, not followed.
 * *left_behind can be that library, if some of it couldn't be deleted. */
mc_status mc_source_reset_if_requested(const char *dir, int from_source, int *reset, char **left_behind);

#ifdef __cplusplus
}
#endif

#endif

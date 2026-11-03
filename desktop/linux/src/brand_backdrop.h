/* The backdrop of the app's own pages, which have no art of their own: the empty pages (no
 * music yet, no playlists). The same four glows as art_backdrop.h, in the logo's colours
 * (packaging/icons/kithsong-128.png, built in), as the site's background is in its palette.
 * The logo is decoded and its colours found once, the first time one is made; the header
 * bar shows the same decoded logo. Call on the UI thread. */
#ifndef KITHSONG_BRAND_BACKDROP_H
#define KITHSONG_BRAND_BACKDROP_H

#include <gtk/gtk.h>

/* The logo itself, the 128-pixel icon: decoded once and kept, so don't unref it. NULL if it
 * couldn't be decoded. */
GdkTexture *brand_logo(void);

/* An ArtBackdrop with child over the logo's glows. */
GtkWidget *brand_backdrop_new(GtkWidget *child);

/* Shows the glows (lit) or fades them to clear, for a page that's only sometimes empty, such
 * as Playlists with its header: glowing while it has none, plain once there's a grid. */
void brand_backdrop_set_lit(GtkWidget *backdrop, gboolean lit);

#endif

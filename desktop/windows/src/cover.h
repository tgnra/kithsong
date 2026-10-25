/* Album covers: the cover.jpg (or cover.png) in an album's folder, decoded with WIC. */
#ifndef MUSICAPP_COVER_H
#define MUSICAPP_COVER_H

#include <stdint.h>
#include <wincodec.h>
#include <windows.h>

/* Decodes dir's cover to a size x size premultiplied, top-down DIB: scaled so the short side
 * fits, centre-cropped, and with rounded corners. Sets *pixels to its bits. Returns NULL if
 * there's no cover or it can't be decoded. Safe on any thread with COM initialised. */
HBITMAP cover_decode(IWICImagingFactory *wic, const wchar_t *dir, int size, int radius,
                     const uint32_t **pixels);

#endif

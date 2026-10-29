#define COBJMACROS
#include "cover.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include "canvas.h"
#include "musiccore/library.h"
#include "wstr.h"

BOOL cover_find(const wchar_t *dir, wchar_t *path, size_t n) {
  char *utf8 = dir ? wstr_to_utf8(dir) : NULL;
  char *found = utf8 ? mc_coverart_find(utf8) : NULL;
  if (found) wstr_from_utf8(path, n, found);
  free(utf8);
  mc_free(found);
  return found != NULL;
}

HBITMAP cover_decode(IWICImagingFactory *wic, const wchar_t *dir, int size, int radius,
                     const uint32_t **pixels) {
  wchar_t path[MAX_PATH * 2];
  return cover_find(dir, path, MAX_PATH * 2) ? cover_decode_file(wic, path, size, radius, pixels) : NULL;
}

/* WIC's JPEG decoder does most of the downscaling while decoding, which is what makes this
 * fast. */
HBITMAP cover_decode_file(IWICImagingFactory *wic, const wchar_t *path, int size, int radius,
                          const uint32_t **pixels) {
  if (!path || size <= 0) return NULL;

  IWICBitmapDecoder *decoder = NULL;
  IWICBitmapFrameDecode *frame = NULL;
  IWICBitmapScaler *scaler = NULL;
  IWICBitmapClipper *clipper = NULL;
  IWICFormatConverter *converter = NULL;
  HBITMAP bmp = NULL;
  UINT w = 0, h = 0;

  if (FAILED(IWICImagingFactory_CreateDecoderFromFilename(wic, path, NULL, GENERIC_READ,
                                                          WICDecodeMetadataCacheOnDemand, &decoder)) ||
      FAILED(IWICBitmapDecoder_GetFrame(decoder, 0, &frame)) ||
      FAILED(IWICBitmapFrameDecode_GetSize(frame, &w, &h)) || !w || !h)
    goto done;

  double s = (double)size / (double)(w < h ? w : h);
  UINT sw = (UINT)fmax(size, floor(w * s + 0.5)), sh = (UINT)fmax(size, floor(h * s + 0.5));
  WICRect crop = {(INT)(sw - (UINT)size) / 2, (INT)(sh - (UINT)size) / 2, size, size};
  if (FAILED(IWICImagingFactory_CreateBitmapScaler(wic, &scaler)) ||
      FAILED(IWICBitmapScaler_Initialize(scaler, (IWICBitmapSource *)frame, sw, sh,
                                         WICBitmapInterpolationModeFant)) ||
      FAILED(IWICImagingFactory_CreateBitmapClipper(wic, &clipper)) ||
      FAILED(IWICBitmapClipper_Initialize(clipper, (IWICBitmapSource *)scaler, &crop)) ||
      FAILED(IWICImagingFactory_CreateFormatConverter(wic, &converter)) ||
      FAILED(IWICFormatConverter_Initialize(converter, (IWICBitmapSource *)clipper,
                                            &GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                            NULL, 0.0, WICBitmapPaletteTypeCustom)))
    goto done;

  BITMAPINFO bi = {.bmiHeader = {.biSize = sizeof bi.bmiHeader, .biWidth = size,
                                 .biHeight = -size, /* top-down */
                                 .biPlanes = 1, .biBitCount = 32, .biCompression = BI_RGB}};
  void *bits = NULL;
  bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
  UINT stride = (UINT)size * 4;
  if (!bmp || FAILED(IWICFormatConverter_CopyPixels(converter, NULL, stride, stride * (UINT)size,
                                                    (BYTE *)bits))) {
    if (bmp) DeleteObject(bmp);
    bmp = NULL;
    goto done;
  }
  canvas corners = {.px = bits, .w = size, .h = size};
  RECT all = {0, 0, size, size};
  canvas_round_corners(&corners, &all, radius, CORNER_ALL, 0, 0);
  *pixels = bits;

done:
  if (converter) IWICFormatConverter_Release(converter);
  if (clipper) IWICBitmapClipper_Release(clipper);
  if (scaler) IWICBitmapScaler_Release(scaler);
  if (frame) IWICBitmapFrameDecode_Release(frame);
  if (decoder) IWICBitmapDecoder_Release(decoder);
  return bmp;
}

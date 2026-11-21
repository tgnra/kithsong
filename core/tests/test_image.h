/* Real images for tests that decode what they're given. Link mc_stb. */
#ifndef MC_TEST_IMAGE_H
#define MC_TEST_IMAGE_H

#include <stdlib.h>
#include <string.h>

#include <stb_image_write.h>

typedef struct test_image {
  unsigned char *data;
  size_t len;
} test_image;

static void test_image_write(void *user, void *data, int size) {
  test_image *t = user;
  unsigned char *more = realloc(t->data, t->len + (size_t)size);
  if (!more) return;
  memcpy(more + t->len, data, (size_t)size);
  t->data = more;
  t->len += (size_t)size;
}

/* A side x side PNG or JPEG of noise that seed picks. Free its data. */
static test_image test_image_make(int png, int seed, int side) {
  test_image t = {NULL, 0};
  unsigned char *px = malloc((size_t)side * side * 3);
  if (!px) return t;
  unsigned v = (unsigned)seed * 2654435761u + 1;
  for (int i = 0; i < side * side * 3; i++) px[i] = (unsigned char)((v = v * 1103515245u + 12345u) >> 16);
  if (png) stbi_write_png_to_func(test_image_write, &t, side, side, 3, px, side * 3);
  else stbi_write_jpg_to_func(test_image_write, &t, side, side, 3, px, 90);
  free(px);
  return t;
}

#endif

/* The stb_image and stb_image_write implementations, compiled once. Only what playlist cover
 * art needs (core/src/playlist/playlist_art.c): JPEG and PNG in from memory, JPEG out through
 * a callback. Files are read and written by core, so UTF-8 paths work on Windows. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_FAILURE_STRINGS
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

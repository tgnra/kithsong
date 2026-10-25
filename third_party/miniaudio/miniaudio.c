/* miniaudio's implementation. Only device output and format conversion are used: decoding
 * is dr_libs (see dr_libs.c), whose newer MP3 decoder trims the encoder delay and padding.
 * The configuration macros come from third_party/CMakeLists.txt. */
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "util/fracindex.h"

#include <stdlib.h>
#include <string.h>

#define BASE 62
static const char k_digits[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
/* The smallest integer part, "A" then 26 zeros: nothing has a smaller one. */
static const char k_smallest[] = "A00000000000000000000000000";

static int digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
  if (c >= 'a' && c <= 'z') return c - 'a' + 36;
  return -1;
}

/* Length of the integer part that starts with head, the head included; 0 if head can't
 * start one. */
static size_t int_len(char head) {
  if (head >= 'a' && head <= 'z') return (size_t)(head - 'a' + 2);
  if (head >= 'A' && head <= 'Z') return (size_t)('Z' - head + 2);
  return 0;
}

static int valid_key(const char *k) {
  size_t n = strlen(k), len = int_len(k[0]);
  if (!len || n < len || !strcmp(k, k_smallest)) return 0;
  for (size_t i = 1; i < n; i++)
    if (digit(k[i]) < 0) return 0;
  return n == len || k[n - 1] != '0';
}

/* Writes to out a fraction strictly between fractions a and b ("" is 0, NULL b is 1).
 * a < b, and neither ends in '0'. */
static void midpoint(const char *a, const char *b, char *out) {
  for (;;) {
    if (b) {
      /* Keep the common prefix, padding a with zeros. */
      size_t n = 0, alen = strlen(a);
      while ((n < alen ? a[n] : '0') == b[n]) n++;
      memcpy(out, b, n);
      out += n;
      a += n < alen ? n : alen;
      b += n;
    }
    int da = *a ? digit(*a) : 0;
    int db = b ? digit(*b) : BASE;
    if (db - da > 1) {
      *out++ = k_digits[(da + db + 1) / 2];
      break;
    }
    /* Adjacent digits: b's first digit alone is between if b goes on, otherwise take a's
     * digit and find a fraction between the rest of a and 1. */
    if (b && b[1]) {
      *out++ = b[0];
      break;
    }
    *out++ = k_digits[da];
    if (*a) a++;
    b = NULL;
  }
  *out = '\0';
}

/* The integer part after x's, into out (room for int_len + 2). 0 if x is the largest. */
static int increment_int(const char *x, char *out) {
  size_t len = int_len(x[0]);
  memcpy(out, x, len);
  out[len] = '\0';
  for (size_t i = len - 1; i >= 1; i--) {
    int d = digit(out[i]) + 1;
    if (d < BASE) {
      out[i] = k_digits[d];
      return 1;
    }
    out[i] = '0';
  }
  /* Every digit carried: the next length up. */
  if (x[0] == 'Z') {
    strcpy(out, "a0");
    return 1;
  }
  if (x[0] == 'z') return 0;
  out[0] = (char)(x[0] + 1);
  if (out[0] > 'a') {
    out[len] = '0';
    out[len + 1] = '\0';
  } else {
    out[len - 1] = '\0';
  }
  return 1;
}

/* The integer part before x's, into out (room for int_len + 2). 0 if x is the smallest. */
static int decrement_int(const char *x, char *out) {
  size_t len = int_len(x[0]);
  memcpy(out, x, len);
  out[len] = '\0';
  for (size_t i = len - 1; i >= 1; i--) {
    int d = digit(out[i]) - 1;
    if (d >= 0) {
      out[i] = k_digits[d];
      return 1;
    }
    out[i] = k_digits[BASE - 1];
  }
  if (x[0] == 'a') {
    strcpy(out, "Zz");
    return 1;
  }
  if (x[0] == 'A') return 0;
  out[0] = (char)(x[0] - 1);
  if (out[0] < 'Z') {
    out[len] = k_digits[BASE - 1];
    out[len + 1] = '\0';
  } else {
    out[len - 1] = '\0';
  }
  return 1;
}

char *mc_frac_key_between(const char *a, const char *b) {
  if ((a && !valid_key(a)) || (b && !valid_key(b)) || (a && b && strcmp(a, b) >= 0)) return NULL;
  /* The result is at most one character longer than the longer of a and b, or a fresh
   * integer part. */
  char *out = malloc((a ? strlen(a) : 0) + (b ? strlen(b) : 0) + sizeof k_smallest + 2);
  if (!out) return NULL;

  if (!a && !b) {
    strcpy(out, "a0");
    return out;
  }
  if (!a) {
    size_t len = int_len(b[0]);
    if (!strncmp(b, k_smallest, len) && len == sizeof k_smallest - 1) {
      memcpy(out, b, len);
      midpoint("", b + len, out + len);
    } else if (b[len]) {
      /* b has a fraction, so its integer part alone sorts before it. */
      memcpy(out, b, len);
      out[len] = '\0';
    } else if (!decrement_int(b, out)) {
      free(out);
      return NULL;
    }
    return out;
  }

  size_t len = int_len(a[0]);
  if (!b) {
    if (!increment_int(a, out)) {
      memcpy(out, a, len);
      midpoint(a + len, NULL, out + len);
    }
    return out;
  }
  if (int_len(b[0]) == len && !strncmp(a, b, len)) {
    memcpy(out, a, len);
    midpoint(a + len, b + len, out + len);
    return out;
  }
  if (!increment_int(a, out)) {
    free(out);
    return NULL;
  }
  if (strcmp(out, b) < 0) return out;
  memcpy(out, a, len);
  midpoint(a + len, NULL, out + len);
  return out;
}

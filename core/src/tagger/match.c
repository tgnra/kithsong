#include "tagger/match.h"

#include <float.h>
#include <stdlib.h>
#include <string.h>

#include "util/platform.h"
#include "util/text.h"

/* --- String distance --- */

/* Appends the comparable form of [s, end) to out; returns the new length. Letters and digits
 * are kept (folded, see mc_text_fold), everything else is dropped. */
static size_t normalize(const char *s, const char *end, uint32_t *out) {
  size_t n = 0;
  const unsigned char *p = (const unsigned char *)s;
  while (p < (const unsigned char *)end && *p) {
    int k = mc_text_fold(mc_utf8_next(&p), out + n);
    if (k > 0) n += (size_t)k;
  }
  return n;
}

/* Digit zero and letter O are the same: "H20" is "H2O" typed carelessly. */
static int same_char(uint32_t x, uint32_t y) {
  return x == y || (x == '0' && y == 'o') || (x == 'o' && y == '0');
}

static size_t levenshtein(const uint32_t *a, size_t n, const uint32_t *b, size_t m) {
  size_t *row = malloc((m + 1) * sizeof *row);
  if (!row) return n > m ? n : m;
  for (size_t j = 0; j <= m; j++) row[j] = j;
  for (size_t i = 1; i <= n; i++) {
    size_t diag = row[0];
    row[0] = i;
    for (size_t j = 1; j <= m; j++) {
      size_t up = row[j];
      size_t best = diag + !same_char(a[i - 1], b[j - 1]);
      if (up + 1 < best) best = up + 1;
      if (row[j - 1] + 1 < best) best = row[j - 1] + 1;
      row[j] = best;
      diag = up;
    }
  }
  size_t d = row[m];
  free(row);
  return d;
}

/* Skips a leading "the ". */
static const char *skip_article(const char *s) {
  if ((s[0] == 'T' || s[0] == 't') && (s[1] == 'H' || s[1] == 'h') &&
      (s[2] == 'E' || s[2] == 'e') && s[3] == ' ')
    return s + 4;
  return s;
}

static int is_space(char c) {
  return c == ' ' || c == '\t';
}

static int lower_ascii(char c) {
  return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

/* Markers that only say a track comes from a continuous DJ mix ("Intro (Mixed)"), or that
 * it isn't a remix ("(Original Mix)"); the release's own track titles leave them out. */
static int is_mix_marker(const char *s, const char *e) {
  static const char *const markers[] = {"mixed", "mixed version", "mixed edit", "mix cut",
                                        "original mix", NULL};
  while (s < e && is_space(*s)) s++;
  while (e > s && is_space(e[-1])) e--;
  for (const char *const *m = markers; *m; m++) {
    size_t n = strlen(*m), i = 0;
    if ((size_t)(e - s) != n) continue;
    while (i < n && lower_ascii(s[i]) == (*m)[i]) i++;
    if (i == n) return 1;
  }
  return 0;
}

/* Where the " - " or ", " before a marker that ends [s, e) is, or NULL:
 * "Paul Copping Remix - Mixed" -> " - Mixed". */
static char *marker_tail(char *s, char *e) {
  for (char *p = e - 1; p > s; p--) {
    size_t sep = p[0] == ' ' && p[1] == '-' && p[2] == ' ' ? 3 : p[0] == ',' && p[1] == ' ' ? 2 : 0;
    if (sep && is_mix_marker(p + sep, e)) return p;
  }
  return NULL;
}

/* Drops DJ-mix markers from the end of s, in place: "Intro (Mixed)", "Intro - Mixed",
 * "Lalala (X Remix - Mixed)" -> "Lalala (X Remix)". Never all of it. */
static void drop_mix_markers(char *s) {
  for (;;) {
    char *e = s + strlen(s);
    while (e > s && is_space(e[-1])) e--;
    *e = '\0';
    if (e - s < 2) return;
    char close = e[-1];
    if (close == ')' || close == ']') {
      char opener = close == ')' ? '(' : '[';
      char *open = e - 2;
      while (open > s && *open != opener) open--;
      if (open == s || *open != opener) return;
      if (is_mix_marker(open + 1, e - 1)) {
        *open = '\0';
        continue;
      }
      char *tail = marker_tail(open + 1, e - 1);
      if (!tail) return;
      memmove(tail, e - 1, 2); /* the closing bracket and the terminator */
      continue;
    }
    char *tail = marker_tail(s, e);
    if (!tail) return;
    *tail = '\0';
  }
}

/* Where a "(Remastered)", "[Live]" or " - 2011 Remaster" suffix starts, or the end. So do
 * subtitles after a tilde or wrapped in dashes, as Japanese releases have them:
 * "HARDDRIVE 150BPM ～LIFE IS GAME", "S.E.X. -Sound of EXtreme-". Only after a space, so a
 * hyphen inside a word ("X-RATED", "AIRWAVE-08") isn't one. */
static const char *suffix_start(const char *s) {
  const char *end = s + strlen(s);
  for (const char *p = s + 1; *p; p++) {
    if (*p == '(' || *p == '[') return p;
    if (p[0] != ' ') continue;
    if (p[1] == '-' && p[2] == ' ') return p;
    if (p[1] == '~' || strncmp(p + 1, "\xEF\xBD\x9E", 3) == 0 || strncmp(p + 1, "\xE3\x80\x9C", 3) == 0)
      return p; /* ~ ～ 〜 */
    if (p[1] == '-' && p[2] && p[2] != ' ' && p[2] != '-') {
      /* "-Subtitle-": closed by a dash at the end of a word. */
      for (const char *q = p + 3; *q; q++)
        if (q[0] == '-' && q[-1] != ' ' && (!q[1] || q[1] == ' ' || q[1] == '(' || q[1] == '[')) return p;
    }
  }
  return end;
}

/* Whether [s, s + n) is one of words, ignoring case. */
static int is_word(const char *s, size_t n, const char *const *words) {
  for (; *words; words++) {
    size_t i = 0;
    if (strlen(*words) != n) continue;
    while (i < n && lower_ascii(s[i]) == (*words)[i]) i++;
    if (i == n) return 1;
  }
  return 0;
}

/* Whether [s, e) only numbers a part: "2", "Vol. 2", "Part II", "Disc Two", "2nd Season",
 * up to any bracketed suffix. */
static int is_numbering(const char *s, const char *e) {
  static const char *const labels[] = {"vol", "volume", "part", "pt", "no", "number", "disc",
                                       "disk", "cd", "chapter", "ch", "episode", "act", "book",
                                       "season", "st", "nd", "rd", "th", NULL};
  static const char *const numbers[] = {"one", "two", "three", "four", "five", "six", "seven",
                                        "eight", "nine", "ten", NULL};
  int number = 0;
  while (s < e) {
    unsigned char c = (unsigned char)*s;
    if (c >= 0x80) return 0;
    if (c == '(' || c == '[') break; /* a suffix: "Vol. 2 (Remastered)" still numbers */
    if (c >= '0' && c <= '9') {
      while (s < e && *s >= '0' && *s <= '9') s++;
      number = 1;
      continue;
    }
    if (lower_ascii((char)c) < 'a' || lower_ascii((char)c) > 'z') {
      s++;
      continue;
    }
    const char *w = s;
    int roman = 1;
    while (s < e && lower_ascii(*s) >= 'a' && lower_ascii(*s) <= 'z') {
      char l = (char)lower_ascii(*s++);
      roman &= l == 'i' || l == 'v' || l == 'x';
    }
    size_t n = (size_t)(s - w);
    if ((roman && n <= 4) || is_word(w, n, numbers)) number = 1;
    else if (!is_word(w, n, labels)) return 0;
  }
  return number;
}

/* The next colon from p on that starts a subtitle, or NULL: ": " or a fullwidth "：", not at
 * the start of s, and not before what only numbers a part ("Title: Part 2"). */
static const char *colon_from(const char *s, const char *p) {
  const char *end = s + strlen(s);
  for (p = p > s ? p : s + 1; *p; p++) {
    size_t len = p[0] == ':' && p[1] == ' ' ? 1 : strncmp(p, "\xEF\xBC\x9A", 3) == 0 ? 3 : 0;
    if (len && !is_numbering(p + len, end)) return p;
  }
  return NULL;
}

enum { SCRIPT_NONE, SCRIPT_LATIN, SCRIPT_GREEK, SCRIPT_CYRILLIC, SCRIPT_CJK };

/* The script of a letter; SCRIPT_NONE for digits, punctuation and what isn't classified. */
static int script_of(uint32_t c) {
  if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) return SCRIPT_LATIN;
  if (c >= 0xC0 && c <= 0x24F) return c == 0xD7 || c == 0xF7 ? SCRIPT_NONE : SCRIPT_LATIN;
  if ((c >= 0xFF21 && c <= 0xFF3A) || (c >= 0xFF41 && c <= 0xFF5A)) return SCRIPT_LATIN; /* fullwidth */
  if (c >= 0x370 && c <= 0x3FF) return SCRIPT_GREEK;
  if (c >= 0x400 && c <= 0x4FF) return SCRIPT_CYRILLIC;
  if ((c >= 0x3040 && c <= 0x30FF) || (c >= 0x31F0 && c <= 0x31FF) || (c >= 0x3400 && c <= 0x4DBF) ||
      (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xAC00 && c <= 0xD7AF) ||
      (c >= 0xFF66 && c <= 0xFF9F))
    return SCRIPT_CJK; /* kana, Han, Hangul */
  return SCRIPT_NONE;
}

/* Where a translation added after the title starts, or the end: a space after letters of
 * one script, followed only by letters of others ("アニメガバイト ANIME GABBA IT!"). More
 * words in the same script ("SPEEDCORE DANDY XXX") or a number ("SPF INFINI 2") aren't one. */
static const char *translation_start(const char *s) {
  int script = SCRIPT_NONE;
  const unsigned char *p = (const unsigned char *)s;
  while (*p) {
    const unsigned char *at = p;
    uint32_t c = mc_utf8_next(&p);
    int sc = script_of(c);
    if (sc && !script) script = sc;
    else if (sc && sc != script) break; /* already mixed: no one script to tell it from */
    if ((c != ' ' && c != 0x3000) || !script) continue;
    int others = 0, same = 0;
    for (const unsigned char *q = p; *q && !same;) {
      int qs = script_of(mc_utf8_next(&q));
      same = qs == script;
      others |= qs != SCRIPT_NONE;
    }
    if (others && !same) return (const char *)at;
  }
  return s + strlen(s);
}

static double distance_range(const char *a, const char *a_end, const char *b, const char *b_end) {
  /* Each byte yields at most three code points ("&" -> "and"). */
  uint32_t *na = malloc(((size_t)(a_end - a) * 3 + 1) * sizeof *na);
  uint32_t *nb = malloc(((size_t)(b_end - b) * 3 + 1) * sizeof *nb);
  double d = 1.0;
  if (na && nb) {
    size_t n = normalize(a, a_end, na), m = normalize(b, b_end, nb);
    size_t longest = n > m ? n : m;
    if (longest == 0) {
      /* Nothing comparable left (all punctuation): fall back to exact comparison. */
      d = (a_end - a == b_end - b && memcmp(a, b, (size_t)(a_end - a)) == 0) ? 0.0 : 1.0;
    } else {
      d = (double)levenshtein(na, n, nb, m) / (double)longest;
    }
  }
  free(na);
  free(nb);
  return d;
}

double mc_string_distance(const char *a, const char *b) {
  if (!a || !b || !*a || !*b) return -1.0;
  char *ma = mc_strdup(skip_article(a)), *mb = mc_strdup(skip_article(b));
  if (!ma || !mb) {
    free(ma);
    free(mb);
    return 1.0;
  }
  drop_mix_markers(ma);
  drop_mix_markers(mb);
  a = ma;
  b = mb;
  const char *a_end = a + strlen(a), *b_end = b + strlen(b);
  double d = distance_range(a, a_end, b, b_end);

  /* "Song (2011 Remaster)" vs "Song": close, but not identical. */
  const char *as = suffix_start(a), *bs = suffix_start(b);
  if (d > 0 && (as != a_end || bs != b_end)) {
    double stripped = distance_range(a, as, b, bs) + 0.1;
    if (stripped < d) d = stripped;
  }
  /* "アニメガバイト ANIME GABBA IT!" vs "アニメガバイト": the same title plus a translation.
   * Only one side's is dropped, so titles that share just their first word stay apart. */
  const char *at = translation_start(a), *bt = translation_start(b);
  if (d > 0.1 && at != a_end) {
    double stripped = distance_range(a, at, b, b_end) + 0.1;
    if (stripped < d) d = stripped;
  }
  if (d > 0.1 && bt != b_end) {
    double stripped = distance_range(a, a_end, b, bt) + 0.1;
    if (stripped < d) d = stripped;
  }
  /* "DupleX EP: 204 vs SHARPNEL" vs "DupleX EP": the title plus a subtitle. Also only one
   * side's, so movements ("Op. 5: I. Allegro", "Op. 5: II. Adagio") stay apart. */
  for (const char *c = colon_from(a, a); d > 0.1 && c; c = colon_from(a, c + 1)) {
    double stripped = distance_range(a, c, b, b_end) + 0.1;
    if (stripped < d) d = stripped;
  }
  for (const char *c = colon_from(b, b); d > 0.1 && c; c = colon_from(b, c + 1)) {
    double stripped = distance_range(a, a_end, b, c) + 0.1;
    if (stripped < d) d = stripped;
  }
  free(ma);
  free(mb);
  return d;
}

typedef struct word {
  const uint32_t *s;
  size_t n;
} word;

/* Splits s into its normalised words: buf gets their letters, words where each is. Returns
 * the count; buf needs room for normalize's output, words for one per byte. */
static size_t words_of(const char *s, uint32_t *buf, word *words) {
  size_t count = 0, n = 0;
  int in_word = 0;
  const unsigned char *p = (const unsigned char *)s;
  while (*p) {
    int k = mc_text_fold(mc_utf8_next(&p), buf + n);
    if (k == MC_FOLD_SPACE || k == MC_FOLD_PUNCT) in_word = 0;
    if (k <= 0) continue;
    if (!in_word) words[count++] = (word){buf + n, 0};
    in_word = 1;
    words[count - 1].n += (size_t)k;
    n += (size_t)k;
  }
  return count;
}

static int word_cmp(const void *x, const void *y) {
  const word *a = x, *b = y;
  for (size_t i = 0; i < a->n && i < b->n; i++)
    if (a->s[i] != b->s[i]) return a->s[i] < b->s[i] ? -1 : 1;
  return a->n < b->n ? -1 : a->n > b->n;
}

static int has_digit(const word *w) {
  for (size_t i = 0; i < w->n; i++)
    if (w->s[i] >= '0' && w->s[i] <= '9') return 1;
  return 0;
}

/* Whether a and b have the same words in another order: "SHOCKWAVE GREATEST HITS 1994-2002
 * MEGAMIX!!!" and "SHOCKWAVE GREATEST HITS MEGAMIX 1994-2002". Their numbers must be in the
 * same order, so "Part 1 Chapter 2" and "Part 2 Chapter 1" stay apart. */
static int same_words(const char *a, const char *b) {
  size_t la = strlen(a), lb = strlen(b);
  uint32_t *ba = malloc((la * 3 + 1) * sizeof *ba), *bb = malloc((lb * 3 + 1) * sizeof *bb);
  word *wa = malloc((la + 1) * sizeof *wa), *wb = malloc((lb + 1) * sizeof *wb);
  int same = 0;
  if (ba && bb && wa && wb) {
    size_t n = words_of(a, ba, wa), m = words_of(b, bb, wb);
    same = n == m && n >= 2;
    for (size_t i = 0, j = 0; same && (i < n || j < m); i++, j++) {
      while (i < n && !has_digit(&wa[i])) i++;
      while (j < m && !has_digit(&wb[j])) j++;
      same = (i < n) == (j < m) && (i == n || word_cmp(&wa[i], &wb[j]) == 0);
    }
    qsort(wa, n, sizeof *wa, word_cmp);
    qsort(wb, m, sizeof *wb, word_cmp);
    for (size_t i = 0; same && i < n; i++) same = word_cmp(&wa[i], &wb[i]) == 0;
  }
  free(ba);
  free(bb);
  free(wa);
  free(wb);
  return same;
}

/* The next quoted part of s from p on: 「…」, 『…』, “…” or "…". Sets [*in, *in_end) to what's
 * inside and returns what follows the closing quote, or NULL if there's none. */
static const char *next_quote(const char *p, const char **in, const char **in_end) {
  static const char *const pairs[][2] = {{"\xE3\x80\x8C", "\xE3\x80\x8D"}, /* 「」 */
                                         {"\xE3\x80\x8E", "\xE3\x80\x8F"}, /* 『』 */
                                         {"\xE2\x80\x9C", "\xE2\x80\x9D"}, /* “” */
                                         {"\"", "\""}};
  for (; *p; p++)
    for (size_t k = 0; k < sizeof pairs / sizeof *pairs; k++) {
      size_t len = strlen(pairs[k][0]);
      if (strncmp(p, pairs[k][0], len) != 0) continue;
      const char *close = strstr(p + len, pairs[k][1]);
      if (!close) continue;
      *in = p + len;
      *in_end = close;
      return close + strlen(pairs[k][1]);
    }
  return NULL;
}

/* The smallest distance between the whole of b and a part of a quoted in it, plus 0.1, or 1. */
static double quoted_distance(const char *a, const char *b) {
  double d = 1.0;
  const char *in, *in_end;
  for (const char *p = next_quote(a, &in, &in_end); p; p = next_quote(p, &in, &in_end)) {
    double q = distance_range(in, in_end, b, b + strlen(b)) + 0.1;
    if (q < d) d = q;
  }
  return d;
}

double mc_album_distance(const char *a, const char *b) {
  double d = mc_string_distance(a, b);
  /* Word order: rarer and riskier in track titles, so only here. */
  if (d > 0.05 && same_words(a, b)) d = 0.05;
  /* 「別名：おにぎりトラックス」 quoted in a longer title is the title. */
  if (d > 0.1) {
    double q = quoted_distance(a, b), r = quoted_distance(b, a);
    if (q < d) d = q;
    if (r < d) d = r;
  }
  return d;
}

/* --- Artists --- */

/* "Smith, John" -> "John Smith", as a person's MusicBrainz sort name turned around; NULL if
 * it has no ", ". */
static char *turned_sort_name(const char *sort) {
  const char *comma = sort ? strstr(sort, ", ") : NULL;
  if (!comma || comma == sort || !comma[2]) return NULL;
  size_t head = (size_t)(comma - sort), tail = strlen(comma + 2);
  char *out = malloc(head + tail + 2);
  if (!out) return NULL;
  memcpy(out, comma + 2, tail);
  out[tail] = ' ';
  memcpy(out + tail + 1, sort, head);
  out[head + tail + 1] = '\0';
  return out;
}

enum { FORM_OWN, FORM_SORT, FORM_SORT_TURNED, FORM_COUNT };

/* The credit with each artist's name in one form (their own name, sort name, sort name
 * turned around) where they have it, as credited where not; NULL if none has it. */
static char *credit_in_form(const mb_credit *c, int form) {
  char **names = calloc((size_t)c->count, sizeof *names);
  size_t len = 1;
  int any = 0;
  for (int i = 0; names && i < c->count; i++) {
    const mb_credit_part *p = &c->parts[i];
    const char *s = form == FORM_OWN ? p->artist_name : form == FORM_SORT ? p->sort_name : NULL;
    names[i] = form == FORM_SORT_TURNED ? turned_sort_name(p->sort_name) : mc_strdup(s);
    if (names[i] && strcmp(names[i], p->name) == 0) {
      free(names[i]); /* nothing new to compare */
      names[i] = NULL;
    }
    any |= names[i] != NULL;
    len += strlen(names[i] ? names[i] : p->name) + (p->join ? strlen(p->join) : 0);
  }
  char *out = names && any ? malloc(len) : NULL;
  if (out) {
    out[0] = '\0';
    for (int i = 0; i < c->count; i++) {
      strcat(out, names[i] ? names[i] : c->parts[i].name);
      if (c->parts[i].join) strcat(out, c->parts[i].join);
    }
  }
  for (int i = 0; names && i < c->count; i++) free(names[i]);
  free(names);
  return out;
}

/* How far an artist tag is from a credit: the best of the credit as shown and as its
 * artists' own names and sort names, so "Niwapnel" fits a release credited to "ニワプネル"
 * (sort name "Niwapnel") and "John Smith" one by "Smith, John". */
static double artist_distance(const char *artist, const char *credited, const mb_credit *c) {
  double d = mc_string_distance(artist, credited);
  for (int f = 0; d != 0.0 && artist && f < FORM_COUNT; f++) {
    char *s = credit_in_form(c, f);
    double e = mc_string_distance(artist, s);
    if (e >= 0 && (d < 0 || e < d)) d = e;
    free(s);
  }
  return d;
}

/* --- Tracks --- */

#define LENGTH_GRACE_MS 10000
#define LENGTH_MAX_MS 30000

typedef struct weighted {
  double sum, weight;
} weighted;

/* Adds a component; a negative distance means unknown and is skipped. */
static void add(weighted *w, double distance, double weight) {
  if (distance < 0) return;
  w->sum += distance * weight;
  w->weight += weight;
}

static double length_distance(int a_ms, int b_ms) {
  if (a_ms <= 0 || b_ms <= 0) return -1.0;
  int diff = abs(a_ms - b_ms) - LENGTH_GRACE_MS;
  if (diff <= 0) return 0.0;
  return diff >= LENGTH_MAX_MS - LENGTH_GRACE_MS
           ? 1.0
           : (double)diff / (LENGTH_MAX_MS - LENGTH_GRACE_MS);
}

double mc_track_distance(const mc_tags *file, const mb_track *track, int abs_index) {
  weighted w = {0, 0};
  add(&w, mc_string_distance(file->title, track->title), 3.0);
  add(&w, length_distance(file->duration_ms, track->length_ms), 2.0);
  if (file->track > 0) {
    /* Multi-disc albums are numbered either per disc or straight through. */
    int same = (file->track == track->number && (!file->disc || file->disc == track->disc)) ||
               file->track == abs_index + 1;
    add(&w, same ? 0.0 : 1.0, 1.0);
  }
  add(&w, artist_distance(file->artist, track->artist, &track->credit), 1.0);
  return w.weight > 0 ? w.sum / w.weight : 0.5;
}

/* --- Assignment --- */

/* Hungarian algorithm with potentials, O(n^2 m), for n <= m. a is n x m, row-major.
 * col_of_row[i] receives the column for row i. */
static mc_status hungarian(const double *a, int n, int m, int *col_of_row) {
  size_t sz = (size_t)m + 1;
  double *u = calloc((size_t)n + 1, sizeof *u), *v = calloc(sz, sizeof *v);
  double *minv = malloc(sz * sizeof *minv);
  int *p = calloc(sz, sizeof *p), *way = calloc(sz, sizeof *way);
  char *used = malloc(sz);
  mc_status st = MC_ERR_NOMEM;
  if (!u || !v || !minv || !p || !way || !used) goto done;

  for (int i = 1; i <= n; i++) {
    p[0] = i;
    int j0 = 0;
    for (int j = 0; j <= m; j++) minv[j] = DBL_MAX, used[j] = 0;
    do {
      used[j0] = 1;
      int i0 = p[j0], j1 = 0;
      double delta = DBL_MAX;
      for (int j = 1; j <= m; j++) {
        if (used[j]) continue;
        double cur = a[(size_t)(i0 - 1) * (size_t)m + (size_t)(j - 1)] - u[i0] - v[j];
        if (cur < minv[j]) minv[j] = cur, way[j] = j0;
        if (minv[j] < delta) delta = minv[j], j1 = j;
      }
      for (int j = 0; j <= m; j++) {
        if (used[j]) u[p[j]] += delta, v[j] -= delta;
        else minv[j] -= delta;
      }
      j0 = j1;
    } while (p[j0] != 0);
    do {
      int j1 = way[j0];
      p[j0] = p[j1];
      j0 = j1;
    } while (j0);
  }
  for (int j = 1; j <= m; j++)
    if (p[j]) col_of_row[p[j] - 1] = j - 1;
  st = MC_OK;

done:
  free(u);
  free(v);
  free(minv);
  free(p);
  free(way);
  free(used);
  return st;
}

mc_status mc_assign(const double *cost, int rows, int cols, int *out) {
  for (int i = 0; i < rows; i++) out[i] = -1;
  if (rows == 0 || cols == 0) return MC_OK;
  if (rows <= cols) return hungarian(cost, rows, cols, out);

  /* More rows than columns: solve the transpose, then invert the result. */
  double *t = malloc((size_t)rows * (size_t)cols * sizeof *t);
  int *row_of_col = malloc((size_t)cols * sizeof *row_of_col);
  mc_status st = MC_ERR_NOMEM;
  if (t && row_of_col) {
    for (int i = 0; i < rows; i++)
      for (int j = 0; j < cols; j++)
        t[(size_t)j * (size_t)rows + (size_t)i] = cost[(size_t)i * (size_t)cols + (size_t)j];
    st = hungarian(t, cols, rows, row_of_col);
    if (st == MC_OK)
      for (int j = 0; j < cols; j++) out[row_of_col[j]] = j;
  }
  free(t);
  free(row_of_col);
  return st;
}

/* --- Releases --- */

/* Above this, a file and a track are treated as unrelated even if the assignment paired them,
 * so a wrong title is never written to a file just to fill a slot. */
#define TRACK_MATCH_MAX 0.6

/* MusicBrainz's special purpose artist for compilations. */
#define VARIOUS_ARTISTS_ID "89ad4ac3-39f7-470e-963a-56509c546377"

/* "Various Artists", or as some releases credit it, "V.A.". */
static int is_various(const char *artist) {
  return artist && (mc_string_distance(artist, "Various Artists") == 0.0 ||
                    mc_string_distance(artist, "V.A.") == 0.0);
}

/* A compilation: Various Artists among the release's credits, alone or beside a DJ
 * ("DJ SHARPNEL & V.A."). Its album artist tag tends to name the label or the DJ, and the
 * tracks' artists are scored anyway. */
static int is_compilation(const mb_release *rel) {
  for (int i = 0; i < rel->credit.count; i++) {
    const mb_credit_part *p = &rel->credit.parts[i];
    if ((p->id && strcmp(p->id, VARIOUS_ARTISTS_ID) == 0) || is_various(p->name)) return 1;
  }
  return is_various(rel->artist);
}

/* Closer than this, an album artist tag names one of the release's labels. Below the 0.1 a
 * suffix costs, so "SHARPNELSOUND (m1dy)" doesn't: its artist is still scored. */
#define LABEL_MATCH_MAX 0.1

/* How close the album artist tag is to the nearest of the release's labels, or -1. */
static double label_distance(const mb_release *rel, const char *artist) {
  double best = rel->label_count ? -1.0 : mc_string_distance(artist, rel->label);
  for (int i = 0; i < rel->label_count; i++) {
    double d = mc_string_distance(artist, rel->labels[i]);
    if (d >= 0 && (best < 0 || d < best)) best = d;
  }
  return best;
}

static int has_audio(const mb_release *rel) {
  for (int j = 0; j < rel->track_count; j++)
    if (!rel->tracks[j].video) return 1;
  return 0;
}

int mc_track_left_out(const mb_release *rel, int i) {
  /* Audio files of an all-video release (a concert DVD's sound) are scored against it. */
  return i >= 0 && i < rel->track_count && rel->tracks[i].video && has_audio(rel);
}

/* Most common non-zero year among the files, or 0. */
static int files_year(const mc_tags *files, int n) {
  int best = 0, best_count = 0;
  for (int i = 0; i < n; i++) {
    if (!files[i].year) continue;
    int count = 0;
    for (int j = 0; j < n; j++) count += files[j].year == files[i].year;
    if (count > best_count) best = files[i].year, best_count = count;
  }
  return best;
}

static double year_distance(int file_year, const mb_release *rel) {
  if (!file_year || (!rel->year && !rel->original_year)) return -1.0;
  if (file_year == rel->year || file_year == rel->original_year) return 0.0;
  int diff = abs(file_year - (rel->original_year ? rel->original_year : rel->year));
  return diff >= 10 ? 1.0 : diff / 10.0;
}

mc_status mc_score_release(const mb_release *rel, const mc_tags *files, int file_count,
                           const char *album, const char *artist, int *map,
                           mc_release_score *out) {
  int n = file_count, m = rel->track_count;
  out->distance = 1.0;
  out->missing = m;
  out->extra = n;
  for (int i = 0; i < n; i++) map[i] = -1;
  if (n == 0 || m == 0) return MC_OK;

  double *cost = malloc((size_t)n * (size_t)m * sizeof *cost);
  if (!cost) return MC_ERR_NOMEM;
  int audio = has_audio(rel);
  /* A left-out track costs the most, so the pair is dropped below if it's made at all. */
  for (int i = 0; i < n; i++)
    for (int j = 0; j < m; j++)
      cost[(size_t)i * (size_t)m + (size_t)j] = audio && rel->tracks[j].video
                                                  ? 1.0
                                                  : mc_track_distance(&files[i], &rel->tracks[j], j);
  mc_status st = mc_assign(cost, n, m, map);
  if (st != MC_OK) {
    free(cost);
    return st;
  }

  for (int i = 0; i < n; i++)
    if (map[i] >= 0 && cost[(size_t)i * (size_t)m + (size_t)map[i]] > TRACK_MATCH_MAX) map[i] = -1;
  free(cost);
  return mc_score_release_map(rel, files, file_count, album, artist, map, out);
}

mc_status mc_score_release_map(const mb_release *rel, const mc_tags *files, int file_count,
                               const char *album, const char *artist, const int *map,
                               mc_release_score *out) {
  int n = file_count, m = rel->track_count;
  out->distance = 1.0;
  out->missing = m;
  out->extra = n;
  if (n == 0 || m == 0) return MC_OK;

  /* Left-out tracks don't count towards the tracklist; a file the user pairs with one does. */
  int audio = has_audio(rel), scored = m, matched = 0, matched_scored = 0;
  if (audio)
    for (int j = 0; j < m; j++) scored -= rel->tracks[j].video;
  double track_sum = 0;
  for (int i = 0; i < n; i++) {
    if (map[i] < 0 || map[i] >= m) continue;
    track_sum += mc_track_distance(&files[i], &rel->tracks[map[i]], map[i]);
    matched++;
    matched_scored += !(audio && rel->tracks[map[i]].video);
  }

  out->missing = scored - matched_scored;
  out->extra = n - matched;

  weighted w = {0, 0};
  /* Heavy enough that a wrong title or artist alone drops below the default threshold. */
  add(&w, mc_album_distance(album, rel->title), 5.0);
  /* An album artist tag that names the release's label (a doujin label's releases tagged
   * "SHARPNELSOUND") says nothing about who it's credited to: no part, as for a compilation. */
  double artist_d = artist_distance(artist, rel->artist, &rel->credit);
  double label_d = label_distance(rel, artist);
  int names_label = label_d >= 0 && label_d < LABEL_MATCH_MAX && (artist_d < 0 || label_d < artist_d);
  if (!is_compilation(rel) && !names_label) add(&w, artist_d, 5.0);
  add(&w, matched ? track_sum / matched : 1.0, 6.0);
  add(&w, (double)out->missing / scored, 10.0);
  add(&w, (double)out->extra / n, 6.0);
  add(&w, year_distance(files_year(files, n), rel), 0.5);
  out->distance = w.sum / w.weight;
  return MC_OK;
}

double mc_score_recording(const mb_release *rec, const mc_tags *file) {
  if (rec->track_count < 1) return 1.0;
  const mb_track *t = &rec->tracks[0];
  weighted w = {0, 0};
  double title = mc_string_distance(file->title, t->title);
  add(&w, title < 0 ? 1.0 : title, 3.0);
  add(&w, artist_distance(file->artist, t->artist, &t->credit), 3.0);
  add(&w, length_distance(file->duration_ms, t->length_ms), 2.0);
  return w.sum / w.weight;
}

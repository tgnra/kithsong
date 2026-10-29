#include "tagger/search_keys.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tagger/match.h"
#include "util/platform.h"

/* --- Text helpers --- */

static int is_space_at(const char *p, size_t *len) {
  if (*p == ' ' || *p == '\t') return (int)(*len = 1);
  if (strncmp(p, "\xE3\x80\x80", 3) == 0) return (int)(*len = 3); /* ideographic space */
  return 0;
}

/* Copy of [s, e) without surrounding spaces, or NULL if nothing is left. */
static char *dup_trim(const char *s, const char *e) {
  size_t k;
  while (s < e && is_space_at(s, &k)) s += k;
  while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
  while (e - s >= 3 && strncmp(e - 3, "\xE3\x80\x80", 3) == 0) e -= 3;
  if (s >= e) return NULL;
  char *out = malloc((size_t)(e - s) + 1);
  if (out) {
    memcpy(out, s, (size_t)(e - s));
    out[e - s] = '\0';
  }
  return out;
}

/* " - ", " – " or " — " at p: the separator between artist and title. Returns its length. */
static size_t dash_at(const char *p) {
  if (p[0] != ' ') return 0;
  if (p[1] == '-' && p[2] == ' ') return 3;
  if ((strncmp(p + 1, "\xE2\x80\x93", 3) == 0 || strncmp(p + 1, "\xE2\x80\x94", 3) == 0) && p[4] == ' ')
    return 5;
  return 0;
}

/* Bracket at p: 1 for an opening round one, 2 for an opening square one, -1 / -2 for closing,
 * 0 for none; *len receives its length. Full-width forms and 【】 count too. */
static int bracket_at(const char *p, size_t *len) {
  static const struct { const char *s; int kind; } table[] = {
    {"(", 1}, {")", -1}, {"[", 2}, {"]", -2},
    {"\xEF\xBC\x88", 1}, {"\xEF\xBC\x89", -1},  /* （ ） */
    {"\xEF\xBC\xBB", 2}, {"\xEF\xBC\xBD", -2},  /* ［ ］ */
    {"\xE3\x80\x90", 2}, {"\xE3\x80\x91", -2},  /* 【 】 */
  };
  for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
    size_t n = strlen(table[i].s);
    if (strncmp(p, table[i].s, n) == 0) {
      *len = n;
      return table[i].kind;
    }
  }
  *len = 1;
  return 0;
}

/* The end of the bracket group opened at p (just past its closing bracket), or NULL. */
static const char *group_end(const char *p) {
  int depth = 0;
  size_t len;
  while (*p) {
    int k = bracket_at(p, &len);
    if (k > 0) depth++;
    else if (k < 0 && --depth == 0) return p + len;
    p += len;
  }
  return NULL;
}

static int ends_with_ci(const char *s, size_t n, const char *suffix) {
  size_t m = strlen(suffix);
  if (m > n) return 0;
  for (size_t i = 0; i < m; i++)
    if (tolower((unsigned char)s[n - m + i]) != suffix[i]) return 0;
  return 1;
}

static int is_various_name(const char *s) {
  static const char *const names[] = {"v.a.", "va", "various artists", "various", NULL};
  if (!s) return 0;
  for (const char *const *v = names; *v; v++) {
    size_t n = strlen(*v);
    if (strlen(s) == n && ends_with_ci(s, n, *v)) return 1;
  }
  return 0;
}

/* Drops a trailing "and V.A." from an artist in a folder name: "The Speed Freak and V.A."
 * -> "The Speed Freak". A name that is only "V.A." becomes NULL. Sets *various either way. */
static char *strip_various(char *s, int *various) {
  static const char *const joins[] = {" and ", " & ", ", ", " + ", " with ", NULL};
  static const char *const vas[] = {"v.a.", "va", "various artists", "various", NULL};
  if (!s) return NULL;
  if (is_various_name(s)) {
    *various = 1;
    free(s);
    return NULL;
  }
  size_t n = strlen(s);
  for (const char *const *j = joins; *j; j++) {
    for (const char *const *v = vas; *v; v++) {
      char suffix[32];
      snprintf(suffix, sizeof suffix, "%s%s", *j, *v);
      if (ends_with_ci(s, n, suffix)) {
        *various = 1;
        char *out = dup_trim(s, s + n - strlen(suffix));
        free(s);
        return out;
      }
    }
  }
  return s;
}

static int same_text(const char *a, const char *b) {
  return a && b && mc_string_distance(a, b) == 0.0;
}

/* Splits "Label (Artist)" into its parts. Without brackets, all of it is the prefix. */
static void split_paren(const char *s, const char *e, char **prefix, char **paren, int *various) {
  *prefix = *paren = NULL;
  int depth = 0;
  size_t len;
  for (const char *p = s; p < e; p += len) {
    int k = bracket_at(p, &len);
    if (k == 1 && depth == 0 && p > s) {
      const char *end = group_end(p);
      if (end && end <= e) {
        size_t close_len = 1;
        const char *close = end - 1;
        /* The closing bracket may be a multi-byte one. */
        while (close > p && ((unsigned char)*close & 0xC0) == 0x80) close--;
        bracket_at(close, &close_len);
        *paren = strip_various(dup_trim(p + len, close), various);
        *prefix = dup_trim(s, p);
        return;
      }
    }
    if (k > 0) depth++;
    else if (k < 0 && depth > 0) depth--;
  }
  *prefix = dup_trim(s, e);
}

/* --- Catalogue numbers --- */

int mc_is_catno(const char *s, size_t len) {
  static const char *const words[] = {"flac", "mp3", "aac", "ogg", "opus", "wav", "alac", "web",
                                      "vinyl", "vol", "disc", "disk", "cd", "dvd", "remaster",
                                      "remastered", "edition", "deluxe", "hires", "mix", NULL};
  while (len && (*s == ' ')) s++, len--;
  while (len && s[len - 1] == ' ') len--;
  if (len < 3 || len > 24) return 0;
  int letters = 0, digits = 0;
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)s[i];
    if (isalpha(c) && c < 0x80) letters++;
    else if (isdigit(c)) digits++;
    else if (c != ' ' && c != '-' && c != '.' && c != '_') return 0;
  }
  if (!letters || digits < 3) return 0;

  /* Words that make it a format or quality note: "24bit", "FLAC 16-44", "320kbps". */
  for (size_t i = 0; i < len;) {
    while (i < len && !isalnum((unsigned char)s[i])) i++;
    size_t start = i;
    while (i < len && isalnum((unsigned char)s[i])) i++;
    size_t n = i - start;
    if (!n || n >= 16) continue;
    char w[16];
    for (size_t k = 0; k < n; k++) w[k] = (char)tolower((unsigned char)s[start + k]);
    w[n] = '\0';
    for (const char *const *word = words; *word; word++)
      if (strcmp(w, *word) == 0) return 0;
    if (ends_with_ci(w, n, "bit") || ends_with_ci(w, n, "khz") || ends_with_ci(w, n, "kbps"))
      return 0;
  }
  return 1;
}

static int add_unique(char **list, int count, int max, const char *s, size_t len) {
  if (count >= max) return count;
  for (int i = 0; i < count; i++) {
    if (strlen(list[i]) != len) continue;
    size_t k = 0;
    while (k < len && tolower((unsigned char)list[i][k]) == tolower((unsigned char)s[k])) k++;
    if (k == len) return count;
  }
  char *copy = dup_trim(s, s + len);
  if (copy) list[count++] = copy;
  return count;
}

int mc_catnos_add(char **list, int count, int max, const char *text) {
  if (!text) return count;
  const char *p = text;
  while (*p && count < max) {
    /* One piece: up to "/", "／", "," or ";". */
    const char *start = p, *end = p;
    size_t sep = 0;
    while (*end) {
      if (*end == '/' || *end == ',' || *end == ';') sep = 1;
      else if (strncmp(end, "\xEF\xBC\x8F", 3) == 0) sep = 3; /* ／ */
      if (sep) break;
      end++;
    }
    while (start < end && *start == ' ') start++;
    const char *e = end;
    while (e > start && e[-1] == ' ') e--;
    size_t len = (size_t)(e - start);
    if (mc_is_catno(start, len)) {
      count = add_unique(list, count, max, start, len);
      /* "SRPC-0008LTD": the edition suffix may not be in the database's number. */
      size_t letters = 0;
      while (letters < len && isalpha((unsigned char)start[len - 1 - letters])) letters++;
      if (letters >= 2 && letters <= 4 && letters < len && isdigit((unsigned char)start[len - 1 - letters]) &&
          mc_is_catno(start, len - letters))
        count = add_unique(list, count, max, start, len - letters);
    }
    p = *end ? end + (sep ? sep : 1) : end;
  }
  return count;
}

/* --- Folder names --- */

void mc_folder_name_parse(const char *name, mc_folder_name *out) {
  memset(out, 0, sizeof *out);
  if (!name || !*name) return;
  const char *end = name + strlen(name);
  size_t len;

  /* Catalogue numbers: in any [square] group. */
  for (const char *p = name; *p; p += len) {
    if (bracket_at(p, &len) != 2) continue;
    const char *g = group_end(p);
    if (!g) break;
    size_t close_len;
    const char *close = g - 1;
    while (close > p && ((unsigned char)*close & 0xC0) == 0x80) close--;
    bracket_at(close, &close_len);
    char *inner = dup_trim(p + len, close);
    out->catno_count = mc_catnos_add(out->catnos, out->catno_count, MC_KEYS_MAX, inner);
    free(inner);
    len = (size_t)(g - p);
  }

  /* "Prefix - Title", split at the first dash outside brackets. */
  const char *title = name, *split = NULL;
  int depth = 0;
  for (const char *p = name; *p; p += len) {
    int k = bracket_at(p, &len);
    if (k > 0) depth++;
    else if (k < 0 && depth > 0) depth--;
    else if (!k && depth == 0 && (len = dash_at(p)) > 0) {
      split = p;
      title = p + len;
      break;
    } else {
      len = 1;
    }
  }
  if (split) split_paren(name, split, &out->prefix, &out->paren, &out->various);

  /* The title ends at the first [group] ("[CAT-001] (C89)" follows it) ... */
  const char *title_end = end;
  depth = 0;
  for (const char *p = title; *p; p += len) {
    int k = bracket_at(p, &len);
    if (k == 2 && depth == 0 && p > title) {
      title_end = p;
      break;
    }
    if (k > 0) depth++;
    else if (k < 0 && depth > 0) depth--;
  }
  /* ... and loses a trailing year: "Album (2004)". */
  for (;;) {
    const char *e = title_end;
    while (e > title && e[-1] == ' ') e--;
    if (e - title >= 7 && e[-1] == ')' && e[-6] == '(' && isdigit((unsigned char)e[-5]) &&
        isdigit((unsigned char)e[-4]) && isdigit((unsigned char)e[-3]) && isdigit((unsigned char)e[-2]))
      title_end = e - 6;
    else
      break;
  }
  out->title = dup_trim(title, title_end);
}

/* --- Disc suffixes --- */

static int is_alnum_ascii(char c) {
  return (unsigned char)c < 0x80 && isalnum((unsigned char)c);
}

static const char *skip_disc_seps(const char *p) {
  while (*p == ' ' || *p == '_' || *p == '-' || *p == '.') p++;
  return p;
}

/* Whether p, to the end of the string, is "Disc 1", "CD2", "disk_3", "Disc 1 of 2" or
 * "Disc 1/2", maybe closed by ")" or "]". Returns the disc number, or 0. */
static int disc_tail(const char *p) {
  static const char *const words[] = {"disc", "disk", "cd", NULL};
  for (const char *const *w = words; *w; w++) {
    size_t n = strlen(*w), i = 0;
    while (i < n && tolower((unsigned char)p[i]) == (*w)[i]) i++;
    if (i < n) continue;
    const char *s = skip_disc_seps(p + n);
    int disc = 0, digits = 0;
    while (isdigit((unsigned char)*s) && digits < 3) disc = disc * 10 + (*s++ - '0'), digits++;
    if (!disc || isdigit((unsigned char)*s)) continue;
    while (*s == ' ') s++;
    if ((tolower((unsigned char)s[0]) == 'o' && tolower((unsigned char)s[1]) == 'f') || *s == '/') {
      s = skip_disc_seps(s + (*s == '/' ? 1 : 2));
      if (!isdigit((unsigned char)*s)) continue;
      while (isdigit((unsigned char)*s)) s++;
    }
    while (*s == ' ') s++;
    if (*s == ')' || *s == ']') s++;
    while (*s == ' ') s++;
    if (!*s) return disc;
  }
  return 0;
}

int mc_disc_suffix(const char *title, size_t *base_len) {
  if (!title) return 0;
  for (const char *p = title; *p; p++) {
    if (p > title && is_alnum_ascii(p[-1])) continue;
    int disc = disc_tail(p);
    if (!disc) continue;
    const char *e = p;
    for (;;) {
      if (e > title && strchr(" -_([~,:", e[-1])) e--;
      else if (e - title >= 3 && strncmp(e - 3, "\xE3\x80\x80", 3) == 0) e -= 3;
      else break;
    }
    if (e == title) return 0; /* nothing but the disc */
    *base_len = (size_t)(e - title);
    return disc;
  }
  return 0;
}

void mc_folder_name_clear(mc_folder_name *f) {
  free(f->prefix);
  free(f->paren);
  free(f->title);
  for (int i = 0; i < f->catno_count; i++) free(f->catnos[i]);
  memset(f, 0, sizeof *f);
}

/* --- Artists in file names and titles --- */

char *mc_main_artist(const char *artist) {
  static const char *const feats[] = {" feat. ", " feat ", " ft. ", " featuring ", " (feat. ", " (ft. ",
                                      NULL};
  if (!artist) return NULL;
  size_t n = strlen(artist), cut = n;
  for (size_t i = 0; i < n; i++) {
    for (const char *const *f = feats; *f; f++) {
      size_t m = strlen(*f), k = 0;
      while (k < m && i + k < n && tolower((unsigned char)artist[i + k]) == (*f)[k]) k++;
      if (k == m && i < cut) cut = i;
    }
  }
  return dup_trim(artist, artist + cut);
}

/* Splits text at its first artist/title dash into newly allocated halves. */
static int split_dash(const char *s, char **left, char **right) {
  *left = *right = NULL;
  for (const char *p = s; *p; p++) {
    size_t n = dash_at(p);
    if (!n) continue;
    *left = dup_trim(s, p);
    *right = dup_trim(p + n, p + strlen(p));
    if (*left && *right) return 1;
    free(*left);
    free(*right);
    *left = *right = NULL;
    return 0;
  }
  return 0;
}

static int all_digits(const char *s) {
  for (; *s; s++)
    if (!isdigit((unsigned char)*s)) return 0;
  return 1;
}

#define TITLE_NEAR 0.35 /* a file name half within this distance of the title tag is the title */

char *mc_filename_artist(const char *path, const char *title_tag) {
  if (!path) return NULL;
  const char *a = strrchr(path, '/'), *b = strrchr(path, '\\');
  const char *name = a > b ? a + 1 : b ? b + 1 : path;
  const char *dot = strrchr(name, '.');
  char *stem = dup_trim(name, dot && dot > name ? dot : name + strlen(name));
  if (!stem) return NULL;

  /* A leading track number: "03. ", "1-03 ", "07 - ". */
  const char *s = stem;
  if (isdigit((unsigned char)*s)) {
    const char *p = s;
    while (isdigit((unsigned char)*p)) p++;
    if (*p == '-' && isdigit((unsigned char)p[1])) {
      p++;
      while (isdigit((unsigned char)*p)) p++;
    }
    const char *q = p;
    while (*q == '.' || *q == ')' || *q == '_') q++;
    size_t d = dash_at(q);
    if (d) q += d;
    else if (*q == '-') q++;
    if (q > p && (*q == ' ' || d || q[-1] == ' ')) {
      while (*q == ' ') q++;
      s = q;
    } else if (*q == ' ') {
      while (*q == ' ') q++;
      s = q;
    }
  }

  char *left, *right, *artist = NULL;
  if (split_dash(s, &left, &right)) {
    if (!title_tag) {
      artist = left;
      left = NULL;
    } else {
      double dl = mc_string_distance(left, title_tag), dr = mc_string_distance(right, title_tag);
      if (dr >= 0 && dr <= TITLE_NEAR && (dl < 0 || dr <= dl)) {
        artist = left;
        left = NULL;
      } else if (dl >= 0 && dl <= TITLE_NEAR) {
        artist = right;
        right = NULL;
      }
    }
    free(left);
    free(right);
  }
  free(stem);
  if (artist && all_digits(artist)) {
    free(artist);
    artist = NULL;
  }
  return artist;
}

char *mc_title_artist(const char *title) {
  char *left, *right;
  if (!title || !split_dash(title, &left, &right)) return NULL;
  free(right);
  if (all_digits(left)) {
    free(left);
    return NULL;
  }
  return left;
}

/* --- Keys --- */

typedef struct vote {
  char *name;
  int count;
} vote;

#define MAX_VOTES 64

typedef struct votes {
  vote v[MAX_VOTES];
  int n;
} votes;

/* Counts name once for a file; seen holds the names this file has voted for so far. */
static void cast(votes *vs, char *name, char **seen, int *seen_n) {
  if (!name) return;
  for (int i = 0; i < *seen_n; i++)
    if (same_text(seen[i], name)) {
      free(name);
      return;
    }
  seen[(*seen_n)++] = name;
  for (int i = 0; i < vs->n; i++)
    if (same_text(vs->v[i].name, name)) {
      vs->v[i].count++;
      return;
    }
  if (vs->n < MAX_VOTES) vs->v[vs->n++] = (vote){mc_strdup(name), 1};
}

static int add_name(char **list, int count, const char *name, const char *const *exclude, int nexclude) {
  if (!name || !*name || count >= MC_KEYS_MAX || is_various_name(name)) return count;
  for (int i = 0; i < count; i++)
    if (same_text(list[i], name)) return count;
  for (int i = 0; i < nexclude; i++)
    if (same_text(exclude[i], name)) return count;
  char *copy = mc_strdup(name);
  if (copy) list[count++] = copy;
  return count;
}

/* Adds the parts of "A／B" or "A / B" as separate labels. */
static int add_labels(char **list, int count, const char *text, const char *const *exclude, int nexclude) {
  if (!text) return count;
  const char *p = text;
  while (*p) {
    const char *e = p;
    size_t sep = 0;
    while (*e && !sep) {
      if (*e == '/') sep = 1;
      else if (strncmp(e, "\xEF\xBC\x8F", 3) == 0) sep = 3;
      else e++;
    }
    char *part = dup_trim(p, e);
    count = add_name(list, count, part, exclude, nexclude);
    free(part);
    p = *e ? e + sep : e;
  }
  return count;
}

mc_status mc_search_keys_build(const char *folder, const mc_tags *files, const char *const *paths,
                               int n, const char *album, const char *artist, mc_search_keys *out) {
  memset(out, 0, sizeof *out);
  const char *name = NULL;
  if (folder) {
    const char *a = strrchr(folder, '/'), *b = strrchr(folder, '\\');
    name = a > b ? a + 1 : b ? b + 1 : folder;
  }
  mc_folder_name fn;
  mc_folder_name_parse(name, &fn);

  /* The album artist tag may have the same "Label (Artist)" shape as the folder. */
  char *aa_prefix = NULL, *aa_paren = NULL;
  int aa_various = 0;
  if (artist) split_paren(artist, artist + strlen(artist), &aa_prefix, &aa_paren, &aa_various);
  if (aa_paren && !aa_prefix) {
    free(aa_paren);
    aa_paren = NULL;
  }

  /* Votes for each file's artist: its tag, its file name, and, where the artist tag is
   * missing or just repeats the album artist, its title. */
  votes vs = {0};
  for (int i = 0; i < n; i++) {
    char *seen[3];
    int seen_n = 0;
    const mc_tags *t = &files[i];
    cast(&vs, mc_main_artist(t->artist), seen, &seen_n);
    char *fa = mc_filename_artist(paths ? paths[i] : NULL, t->title);
    cast(&vs, mc_main_artist(fa), seen, &seen_n);
    free(fa);
    if (!t->artist || same_text(t->artist, artist) || same_text(t->artist, aa_prefix)) {
      char *ta = mc_title_artist(t->title);
      cast(&vs, mc_main_artist(ta), seen, &seen_n);
      free(ta);
    }
    for (int k = 0; k < seen_n; k++) free(seen[k]);
  }
  /* Most votes first. */
  for (int i = 1; i < vs.n; i++) {
    vote v = vs.v[i];
    int j = i;
    while (j > 0 && vs.v[j - 1].count < v.count) {
      vs.v[j] = vs.v[j - 1];
      j--;
    }
    vs.v[j] = v;
  }
  int dominant = 0; /* artists on at least half of the files */
  while (dominant < vs.n && vs.v[dominant].count * 2 >= n && n > 0) dominant++;
  out->various = fn.various || aa_various || (n >= 3 && dominant == 0);

  const char *exclude[1] = {artist};
  int c = 0;
  c = add_name(out->artists, c, fn.paren, exclude, 1);
  c = add_name(out->artists, c, aa_paren, exclude, 1);
  for (int i = 0; i < dominant; i++) c = add_name(out->artists, c, vs.v[i].name, exclude, 1);
  out->artist_count = c;

  /* Labels: label tags, then names that stand where the artist would but aren't the
   * artist: the part before "(Artist)", or an album artist / folder prefix that differs
   * from the artist the tracks agree on (or from a compilation's). */
  const char *lab_tag = NULL;
  for (int i = 0; i < n && !lab_tag; i++) lab_tag = files[i].label;
  c = add_labels(out->labels, 0, lab_tag, (const char *const *)out->artists, out->artist_count);
  int has_artist = dominant > 0 || out->various;
  if (aa_prefix && (aa_paren || (has_artist && !(dominant && same_text(aa_prefix, vs.v[0].name)))))
    c = add_labels(out->labels, c, aa_prefix, (const char *const *)out->artists, out->artist_count);
  if (fn.prefix && (fn.paren || fn.various || (has_artist && !(dominant && same_text(fn.prefix, vs.v[0].name)))))
    c = add_labels(out->labels, c, fn.prefix, (const char *const *)out->artists, out->artist_count);
  out->label_count = c;

  /* Catalogue numbers: tags first, then [brackets] in the album tag ("Mix [SRPC-L007]"), then
   * the folder name. */
  c = 0;
  for (int i = 0; i < n; i++) c = mc_catnos_add(out->catnos, c, MC_KEYS_MAX, files[i].catalog);
  mc_folder_name in_album;
  mc_folder_name_parse(album, &in_album);
  for (int i = 0; i < in_album.catno_count; i++)
    c = mc_catnos_add(out->catnos, c, MC_KEYS_MAX, in_album.catnos[i]);
  mc_folder_name_clear(&in_album);
  for (int i = 0; i < fn.catno_count; i++) c = mc_catnos_add(out->catnos, c, MC_KEYS_MAX, fn.catnos[i]);
  out->catno_count = c;

  out->album = mc_strdup(album ? album : fn.title);
  if (album && fn.title) {
    double d = mc_string_distance(album, fn.title);
    if (d < 0 || d > 0.2) out->folder_album = mc_strdup(fn.title);
  }

  for (int i = 0; i < vs.n; i++) free(vs.v[i].name);
  free(aa_prefix);
  free(aa_paren);
  mc_folder_name_clear(&fn);
  return MC_OK;
}

void mc_search_keys_clear(mc_search_keys *k) {
  free(k->album);
  free(k->folder_album);
  for (int i = 0; i < k->artist_count; i++) free(k->artists[i]);
  for (int i = 0; i < k->label_count; i++) free(k->labels[i]);
  for (int i = 0; i < k->catno_count; i++) free(k->catnos[i]);
  memset(k, 0, sizeof *k);
}

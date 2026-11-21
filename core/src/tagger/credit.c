/* Splitting a credited string into its artists. See docs/tagging-rules.md, "Artist credits". */
#include "tagger/credit.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "library/library_internal.h"

/* Always split. Longer forms first, so " feat. " wins over " feat ". */
static const char *const k_feat[] = {" featuring ", " feat. ", " feat ", " ft. ", " ft ", NULL};

/* "(feat. B)" / "[feat. B]" closing the credit. */
static const char *const k_feat_open[] = {"featuring ", "feat. ", "feat ", "ft. ", "ft ", NULL};

/* Split only between known artists. ", & " and ", and " are the serial-comma forms of
 * " & " and " and ", so "A, B, and C" reads as three. */
static const char *const k_ambiguous[] = {", & ", ", and ", " & ", ", ", " and ", " x ",
                                          " vs. ", " vs ", " with ", " / ", "; ", NULL};

/* At most this many names between ambiguous separators in one stretch; past it a stretch
 * stays whole (the search below is exponential in the worst case). */
#define MAX_TOKENS 16

/* Length of the separator from list at s (case-insensitively), the longest that matches, or 0. */
static size_t sep_at(const char *s, const char *const *list) {
  size_t best = 0;
  for (const char *const *sep = list; *sep; sep++) {
    size_t m = strlen(*sep), k = 0;
    while (k < m && s[k] && tolower((unsigned char)s[k]) == (*sep)[k]) k++;
    if (k == m && m > best) best = m;
  }
  return best;
}

static int is_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* [*b, *e) without the whitespace around it. */
static void trim(const char **b, const char **e) {
  while (*b < *e && is_space(**b)) (*b)++;
  while (*e > *b && is_space((*e)[-1])) (*e)--;
}

static char *dup_range(const char *b, const char *e) {
  char *out = malloc((size_t)(e - b) + 1);
  if (!out) return NULL;
  memcpy(out, b, (size_t)(e - b));
  out[e - b] = '\0';
  return out;
}

typedef struct builder {
  mc_credit_split *out;
  int cap;
  int nomem;
} builder;

/* Appends a part; join is the separator after it, or NULL. */
static void push(builder *bd, const char *b, const char *e, const char *jb, const char *je) {
  if (bd->nomem) return;
  mc_credit_split *s = bd->out;
  if (s->count == bd->cap) {
    int cap = bd->cap ? bd->cap * 2 : 4;
    mc_credit_split_part *more = realloc(s->parts, (size_t)cap * sizeof *more);
    if (!more) {
      bd->nomem = 1;
      return;
    }
    s->parts = more;
    bd->cap = cap;
  }
  mc_credit_split_part *p = &s->parts[s->count];
  p->name = dup_range(b, e);
  p->join = jb ? dup_range(jb, je) : NULL;
  if (!p->name || (jb && !p->join)) {
    free(p->name);
    free(p->join);
    bd->nomem = 1;
    return;
  }
  s->count++;
}

/* One stretch between "feat." separators, cut at its ambiguous separators. */
typedef struct stretch {
  const char *tok_b[MAX_TOKENS], *tok_e[MAX_TOKENS]; /* names, trimmed */
  const char *sep_b[MAX_TOKENS], *sep_e[MAX_TOKENS]; /* the separator after each */
  int n;
  int cut[MAX_TOKENS + 1]; /* chosen groups: cut[k] is where group k starts */
  int groups;
  char failed[MAX_TOKENS + 1]; /* no split of tokens i.. into known groups */
} stretch;

static int known_range(mc_known_artist_fn known, void *user, const char *b, const char *e) {
  char *name = dup_range(b, e);
  if (!name) return 0;
  int k = known(user, name) == 1;
  free(name);
  return k;
}

/* Splits tokens i.. into groups that are each a known artist, longest group first, so a
 * known name with a separator in it ("Earth, Wind & Fire") is kept whole. */
static int split_from(stretch *st, int i, mc_known_artist_fn known, void *user) {
  if (i == st->n) return 1;
  if (st->failed[i]) return 0;
  for (int j = st->n - 1; j >= i; j--) {
    if (i == 0 && j == st->n - 1) continue; /* the whole stretch: checked by the caller */
    if (!known_range(known, user, st->tok_b[i], st->tok_e[j])) continue;
    int g = st->groups;
    st->cut[st->groups++] = i;
    if (split_from(st, j + 1, known, user)) return 1;
    st->groups = g;
  }
  st->failed[i] = 1;
  return 0;
}

/* Adds the stretch [b, e) as one or more parts; the last takes join [jb, je). */
static void add_stretch(builder *bd, const char *b, const char *e, const char *jb,
                        const char *je, mc_known_artist_fn known, void *user) {
  stretch st;
  st.n = 0;
  st.groups = 0;
  memset(st.failed, 0, sizeof st.failed);
  int whole = !known;
  const char *tb = b;
  for (const char *p = b; !whole && p <= e; p++) {
    size_t m = p < e ? sep_at(p, k_ambiguous) : 0;
    if (p < e && (!m || p + m > e)) continue;
    if (st.n == MAX_TOKENS) {
      whole = 1;
      break;
    }
    const char *nb = tb, *ne = p;
    trim(&nb, &ne);
    if (nb == ne) whole = 1; /* "A & & B": a part is never empty */
    st.tok_b[st.n] = nb;
    st.tok_e[st.n] = ne;
    st.sep_b[st.n] = p;
    st.sep_e[st.n] = p + m;
    st.n++;
    if (p == e) break;
    p += m - 1;
    tb = p + 1;
  }
  if (!whole && st.n > 1 && !known_range(known, user, b, e) && split_from(&st, 0, known, user)) {
    for (int g = 0; g < st.groups; g++) {
      int first = st.cut[g], last = g + 1 < st.groups ? st.cut[g + 1] - 1 : st.n - 1;
      if (g + 1 < st.groups)
        push(bd, st.tok_b[first], st.tok_e[last], st.sep_b[last], st.sep_e[last]);
      else
        push(bd, st.tok_b[first], st.tok_e[last], jb, je);
    }
    return;
  }
  push(bd, b, e, jb, je);
}

/* Where a closing "(feat. B)" / "[feat. B]" starts in [b, e), or NULL; *inner gets B's start. */
static const char *bracket_feat(const char *b, const char *e, const char **inner) {
  if (e - b < 3 || (e[-1] != ')' && e[-1] != ']')) return NULL;
  char open = e[-1] == ')' ? '(' : '[';
  for (const char *p = e - 2; p > b; p--) {
    if (*p != open) continue;
    size_t m = sep_at(p + 1, k_feat_open);
    if (!m) return NULL; /* the last bracket isn't a feat. one */
    *inner = p + 1 + m;
    return p;
  }
  return NULL;
}

mc_status mc_credit_split_text(const char *credit, mc_known_artist_fn known, void *user,
                               mc_credit_split *out) {
  if (!out) return MC_ERR_INVALID_ARG;
  memset(out, 0, sizeof *out);
  if (!credit) return MC_ERR_INVALID_ARG;
  builder bd = {out, 0, 0};
  const char *b = credit, *e = credit + strlen(credit);
  trim(&b, &e);

  /* The trailing bracket first: "A (feat. B)" is A, then B, with " (feat. " between. */
  const char *inner = NULL, *br = bracket_feat(b, e, &inner);
  const char *main_e = e, *tail_b = NULL, *tail_e = NULL;
  if (br) {
    const char *mb = b, *me = br, *ib = inner, *ie = e - 1;
    trim(&mb, &me);
    trim(&ib, &ie);
    if (mb < me && ib < ie) {
      main_e = me;
      tail_b = ib;
      tail_e = ie;
    }
  }

  /* Then "feat."-style separators, each stretch between them split on its own. */
  const char *sb = b;
  int empty = 0;
  for (const char *p = b; p < main_e && !empty; p++) {
    size_t m = sep_at(p, k_feat);
    if (!m || p + m > main_e) continue;
    const char *nb = sb, *ne = p;
    trim(&nb, &ne);
    if (nb == ne) empty = 1;
    else add_stretch(&bd, nb, ne, p, p + m, known, user);
    p += m - 1;
    sb = p + 1;
  }
  const char *nb = sb, *ne = main_e;
  trim(&nb, &ne);
  if (nb == ne) empty = 1;
  if (empty) {
    /* "A feat. feat. B": no clean split, so the credit is one artist. */
    mc_credit_split_clear(out);
    bd = (builder){out, 0, 0};
    if (b < e) push(&bd, b, e, NULL, NULL);
  } else {
    add_stretch(&bd, nb, ne, tail_b ? main_e : NULL, tail_b ? tail_b : NULL, known, user);
    if (tail_b) {
      /* The join is " (feat. " as written: from the end of the main part to B. */
      add_stretch(&bd, tail_b, tail_e, NULL, NULL, known, user);
    }
  }
  if (bd.nomem) {
    mc_credit_split_clear(out);
    return MC_ERR_NOMEM;
  }
  return MC_OK;
}

void mc_credit_split_clear(mc_credit_split *s) {
  if (!s) return;
  for (int i = 0; i < s->count; i++) {
    free(s->parts[i].name);
    free(s->parts[i].join);
  }
  free(s->parts);
  s->parts = NULL;
  s->count = 0;
}

int mc_credit_has_separator(const char *name) {
  if (!name) return 0;
  for (const char *p = name; *p; p++)
    if (sep_at(p, k_feat) || sep_at(p, k_ambiguous)) return 1;
  const char *b = name, *e = name + strlen(name), *inner;
  return bracket_feat(b, e, &inner) != NULL;
}

int mc_credit_known_in_library(void *lib, const char *name) {
  if (!lib || !name || !*name) return 0;
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_library_db(lib),
                         "SELECT name, mb_artistid FROM artists WHERE name = ?1 COLLATE NOCASE",
                         -1, &stmt, NULL) != SQLITE_OK)
    return 0;
  sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
  int known = 0;
  while (!known && sqlite3_step(stmt) == SQLITE_ROW) {
    /* Older imports made rows like "A & B" from whole credits; those are one artist only if
     * MusicBrainz says so. */
    const char *id = (const char *)sqlite3_column_text(stmt, 1);
    known = (id && *id) || !mc_credit_has_separator((const char *)sqlite3_column_text(stmt, 0));
  }
  sqlite3_finalize(stmt);
  return known;
}

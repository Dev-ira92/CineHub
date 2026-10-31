/* shape.c - spajanje arapskih/farsi slova i RTL redoslijed (pojednostavljeno).
 *
 * Pokriva arapsko pismo + farsi slova (p, c, zh, k, g, y), ligature lam-alef,
 * ZWNJ (nulte sirine, prekida spajanje), farsi/arapske cifre i mijesanje s
 * latinicom. Ne pokriva harakate (skidaju se) ni punu Unicode BiDi.
 */
#include "text.h"
#include <string.h>

/* join: 0 = ne spaja se, 1 = spaja se samo sa prethodnim (zdesna),
 *       2 = spaja se na obje strane. Oblici: izolovani, zavrsni,
 *       pocetni, srednji (Presentation Forms-A/B). */
typedef struct { uint16_t base, iso, fin, ini, med; uint8_t join; } letter_t;

static const letter_t L[] = {
    {0x0621, 0xFE80, 0,      0,      0,      0},
    {0x0622, 0xFE81, 0xFE82, 0,      0,      1},
    {0x0623, 0xFE83, 0xFE84, 0,      0,      1},
    {0x0624, 0xFE85, 0xFE86, 0,      0,      1},
    {0x0625, 0xFE87, 0xFE88, 0,      0,      1},
    {0x0626, 0xFE89, 0xFE8A, 0xFE8B, 0xFE8C, 2},
    {0x0627, 0xFE8D, 0xFE8E, 0,      0,      1},
    {0x0628, 0xFE8F, 0xFE90, 0xFE91, 0xFE92, 2},
    {0x0629, 0xFE93, 0xFE94, 0,      0,      1},
    {0x062A, 0xFE95, 0xFE96, 0xFE97, 0xFE98, 2},
    {0x062B, 0xFE99, 0xFE9A, 0xFE9B, 0xFE9C, 2},
    {0x062C, 0xFE9D, 0xFE9E, 0xFE9F, 0xFEA0, 2},
    {0x062D, 0xFEA1, 0xFEA2, 0xFEA3, 0xFEA4, 2},
    {0x062E, 0xFEA5, 0xFEA6, 0xFEA7, 0xFEA8, 2},
    {0x062F, 0xFEA9, 0xFEAA, 0,      0,      1},
    {0x0630, 0xFEAB, 0xFEAC, 0,      0,      1},
    {0x0631, 0xFEAD, 0xFEAE, 0,      0,      1},
    {0x0632, 0xFEAF, 0xFEB0, 0,      0,      1},
    {0x0633, 0xFEB1, 0xFEB2, 0xFEB3, 0xFEB4, 2},
    {0x0634, 0xFEB5, 0xFEB6, 0xFEB7, 0xFEB8, 2},
    {0x0635, 0xFEB9, 0xFEBA, 0xFEBB, 0xFEBC, 2},
    {0x0636, 0xFEBD, 0xFEBE, 0xFEBF, 0xFEC0, 2},
    {0x0637, 0xFEC1, 0xFEC2, 0xFEC3, 0xFEC4, 2},
    {0x0638, 0xFEC5, 0xFEC6, 0xFEC7, 0xFEC8, 2},
    {0x0639, 0xFEC9, 0xFECA, 0xFECB, 0xFECC, 2},
    {0x063A, 0xFECD, 0xFECE, 0xFECF, 0xFED0, 2},
    {0x0641, 0xFED1, 0xFED2, 0xFED3, 0xFED4, 2},
    {0x0642, 0xFED5, 0xFED6, 0xFED7, 0xFED8, 2},
    {0x0643, 0xFED9, 0xFEDA, 0xFEDB, 0xFEDC, 2},
    {0x0644, 0xFEDD, 0xFEDE, 0xFEDF, 0xFEE0, 2},
    {0x0645, 0xFEE1, 0xFEE2, 0xFEE3, 0xFEE4, 2},
    {0x0646, 0xFEE5, 0xFEE6, 0xFEE7, 0xFEE8, 2},
    {0x0647, 0xFEE9, 0xFEEA, 0xFEEB, 0xFEEC, 2},
    {0x0648, 0xFEED, 0xFEEE, 0,      0,      1},
    {0x0649, 0xFEEF, 0xFEF0, 0,      0,      1},
    {0x064A, 0xFEF1, 0xFEF2, 0xFEF3, 0xFEF4, 2},
    /* farsi */
    {0x067E, 0xFB56, 0xFB57, 0xFB58, 0xFB59, 2},   /* p  */
    {0x0686, 0xFB7A, 0xFB7B, 0xFB7C, 0xFB7D, 2},   /* ch */
    {0x0698, 0xFB8A, 0xFB8B, 0,      0,      1},   /* zh */
    {0x06A9, 0xFB8E, 0xFB8F, 0xFB90, 0xFB91, 2},   /* k  */
    {0x06AF, 0xFB92, 0xFB93, 0xFB94, 0xFB95, 2},   /* g  */
    {0x06CC, 0xFBFC, 0xFBFD, 0xFBFE, 0xFBFF, 2},   /* y  */
};
#define NL ((int)(sizeof L / sizeof L[0]))

static const letter_t *lookup(uint32_t cp)
{
    for (int i = 0; i < NL; i++)
        if (L[i].base == cp)
            return &L[i];
    return NULL;
}

/* ---------------- UTF-8 ---------------- */

static int decode(const char *s, uint32_t *out, int cap)
{
    const unsigned char *p = (const unsigned char *)s;
    int n = 0;

    while (*p && n < cap) {
        uint32_t cp;
        int      extra;
        if (*p < 0x80)      { cp = *p;        extra = 0; }
        else if (*p < 0xC0) { p++; continue; }              /* zalutali bajt */
        else if (*p < 0xE0) { cp = *p & 0x1F; extra = 1; }
        else if (*p < 0xF0) { cp = *p & 0x0F; extra = 2; }
        else                { cp = *p & 0x07; extra = 3; }
        p++;
        while (extra-- > 0) {
            if ((*p & 0xC0) != 0x80) { cp = 0xFFFD; break; }
            cp = (cp << 6) | (*p & 0x3F);
            p++;
        }
        out[n++] = cp;
    }
    return n;
}

/* ---------------- spajanje slova ---------------- */

static int skip_cp(uint32_t c)
{
    return (c >= 0x064B && c <= 0x065F) || c == 0x0670 ||   /* harakati */
           c == 0x200D || c == 0x200E || c == 0x200F || c == 0xFEFF ||
           c == 0x0640;                                      /* kashida */
}

/* in[]: logicki redoslijed. out[]: oblici, ZWNJ jos prisutan. */
static int shape(const uint32_t *in, int n, uint32_t *out)
{
    int m = 0;

    for (int i = 0; i < n; i++) {
        const letter_t *cur = lookup(in[i]);
        if (!cur) {
            out[m++] = in[i];
            continue;
        }

        const letter_t *prev = (i > 0) ? lookup(in[i - 1]) : NULL;
        const letter_t *next = (i + 1 < n) ? lookup(in[i + 1]) : NULL;
        int prev_ok = prev && prev->join == 2 && cur->join >= 1;

        /* lam + alef -> ligatura */
        if (in[i] == 0x0644 && next &&
            (in[i + 1] == 0x0622 || in[i + 1] == 0x0623 ||
             in[i + 1] == 0x0625 || in[i + 1] == 0x0627)) {
            static const uint16_t lig[4][2] = {
                {0xFEF5, 0xFEF6}, {0xFEF7, 0xFEF8},
                {0xFEF9, 0xFEFA}, {0xFEFB, 0xFEFC} };
            int k = in[i + 1] == 0x0622 ? 0 : in[i + 1] == 0x0623 ? 1 :
                    in[i + 1] == 0x0625 ? 2 : 3;
            out[m++] = prev_ok ? lig[k][1] : lig[k][0];
            i++;                           /* alef je potrosen */
            continue;
        }

        int next_ok = cur->join == 2 && next && next->join >= 1;
        uint16_t g;

        if (prev_ok && next_ok)      g = cur->med;
        else if (prev_ok)            g = cur->fin;
        else if (next_ok)            g = cur->ini;
        else                         g = cur->iso;
        out[m++] = g ? g : cur->iso;
    }
    return m;
}

/* ---------------- smjer ---------------- */

enum { T_L = 0, T_R = 1, T_N = 2 };

static int is_digit_cp(uint32_t c)
{
    return (c >= '0' && c <= '9') || (c >= 0x0660 && c <= 0x0669) ||
           (c >= 0x06F0 && c <= 0x06F9);
}

static int is_neutral(uint32_t c)
{
    if (c < 0x80)
        return !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                 (c >= 'A' && c <= 'Z'));
    return c == 0x00A0 || c == 0x00AB || c == 0x00BB || c == 0x060C ||
           c == 0x061B || c == 0x061F || (c >= 0x066A && c <= 0x066D) ||
           (c >= 0x2000 && c <= 0x206F) || c == 0x3000 || c == 0x200C;
}

static int is_rtl_cp(uint32_t c)
{
    return (c >= 0x0590 && c <= 0x08FF) || (c >= 0xFB1D && c <= 0xFDFF) ||
           (c >= 0xFE70 && c <= 0xFEFF);
}

static int ctype_of(uint32_t c)
{
    if (is_digit_cp(c))
        return T_L;
    if (is_neutral(c))
        return T_N;
    return is_rtl_cp(c) ? T_R : T_L;
}

static uint32_t mirror(uint32_t c)
{
    switch (c) {
    case '(': return ')';  case ')': return '(';
    case '[': return ']';  case ']': return '[';
    case '{': return '}';  case '}': return '{';
    case '<': return '>';  case '>': return '<';
    case 0xAB: return 0xBB; case 0xBB: return 0xAB;
    default: return c;
    }
}

static void rev(uint32_t *a, uint8_t *t, int i, int j)   /* [i, j) */
{
    for (j--; i < j; i++, j--) {
        uint32_t c = a[i]; a[i] = a[j]; a[j] = c;
        uint8_t  x = t[i]; t[i] = t[j]; t[j] = x;
    }
}

int text_visual(const char *utf8, uint32_t *out, int cap, int dir,
                int *is_rtl)
{
    uint32_t a[TEXT_MAX], b[TEXT_MAX];
    uint8_t  t[TEXT_MAX];
    int      n = decode(utf8, a, TEXT_MAX);
    int      k = 0;

    for (int i = 0; i < n; i++)             /* skini harakate i sl. */
        if (!skip_cp(a[i]))
            a[k++] = a[i];
    n = shape(a, k, b);

    k = 0;                                  /* skini ZWNJ poslije spajanja */
    for (int i = 0; i < n; i++)
        if (b[i] != 0x200C && b[i] != 0x200B)
            b[k++] = b[i];
    n = k;

    for (int i = 0; i < n; i++)
        t[i] = (uint8_t)ctype_of(b[i]);

    int base = dir;
    if (base < 0) {
        base = 0;
        for (int i = 0; i < n; i++) {
            if (is_digit_cp(b[i]) || t[i] == T_N)
                continue;
            base = (t[i] == T_R);
            break;
        }
    }
    if (is_rtl)
        *is_rtl = base;

    /* neutralni znakovi preuzimaju smjer susjeda ako su isti, inace osnovni */
    for (int i = 0; i < n; ) {
        if (t[i] != T_N) { i++; continue; }
        int j = i;
        while (j < n && t[j] == T_N)
            j++;
        int l = i > 0 ? t[i - 1] : base;
        int r = j < n ? t[j] : base;
        int res = (l == r) ? l : base;
        for (int q = i; q < j; q++)
            t[q] = (uint8_t)res;
        i = j;
    }

    if (base) {                             /* RTL: okreni sve, pa vrati L nizove */
        rev(b, t, 0, n);
        for (int i = 0; i < n; ) {
            if (t[i] != T_L) { i++; continue; }
            int j = i;
            while (j < n && t[j] == T_L)
                j++;
            rev(b, t, i, j);
            i = j;
        }
        for (int i = 0; i < n; i++)
            if (t[i] == T_R)
                b[i] = mirror(b[i]);
    } else {                                /* LTR: okreni samo R nizove */
        for (int i = 0; i < n; ) {
            if (t[i] != T_R) { i++; continue; }
            int j = i;
            while (j < n && t[j] == T_R)
                j++;
            rev(b, t, i, j);
            for (int q = i; q < j; q++)
                b[q] = mirror(b[q]);
            i = j;
        }
    }

    if (n > cap)
        n = cap;
    memcpy(out, b, (size_t)n * sizeof *out);
    return n;
}

int text_has_nonascii(const char *s)
{
    for (; *s; s++)
        if ((unsigned char)*s >= 0x80)
            return 1;
    return 0;
}

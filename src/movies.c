/* movies.c - minimalni parser za movies.json.
 *
 * Ocekuje niz objekata sa string/broj vrijednostima. Ne treba nikakvu
 * biblioteku. Podrzava escape-ove (\n \" \\ \/ \uXXXX) i UTF-8.
 */
#include "movies.h"
#include "common.h"
#include <ctype.h>

typedef struct { const char *p, *end; } cur_t;

static void skip_ws(cur_t *c)
{
    while (c->p < c->end && isspace((unsigned char)*c->p))
        c->p++;
}

static int put_utf8(char *d, size_t cap, size_t *n, unsigned cp)
{
    char t[4];
    int  k;
    if (cp < 0x80)        { t[0] = (char)cp; k = 1; }
    else if (cp < 0x800)  { t[0] = (char)(0xC0 | cp >> 6);
                            t[1] = (char)(0x80 | (cp & 63)); k = 2; }
    else if (cp < 0x10000){ t[0] = (char)(0xE0 | cp >> 12);
                            t[1] = (char)(0x80 | ((cp >> 6) & 63));
                            t[2] = (char)(0x80 | (cp & 63)); k = 3; }
    else                  { t[0] = (char)(0xF0 | cp >> 18);
                            t[1] = (char)(0x80 | ((cp >> 12) & 63));
                            t[2] = (char)(0x80 | ((cp >> 6) & 63));
                            t[3] = (char)(0x80 | (cp & 63)); k = 4; }
    if (*n + (size_t)k >= cap)
        return -1;
    memcpy(d + *n, t, (size_t)k);
    *n += (size_t)k;
    return 0;
}

static int hex4(const char *s, unsigned *out)
{
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char ch = s[i];
        v <<= 4;
        if (ch >= '0' && ch <= '9')      v |= (unsigned)(ch - '0');
        else if (ch >= 'a' && ch <= 'f') v |= (unsigned)(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') v |= (unsigned)(ch - 'A' + 10);
        else return -1;
    }
    *out = v;
    return 0;
}

/* Cita JSON string u dst (skracuje ako ne stane). 0 = ok. */
static int read_string(cur_t *c, char *dst, size_t cap)
{
    size_t n = 0;

    if (c->p >= c->end || *c->p != '"')
        return -1;
    c->p++;
    while (c->p < c->end && *c->p != '"') {
        unsigned char ch = (unsigned char)*c->p++;
        if (ch == '\\') {
            if (c->p >= c->end)
                return -1;
            char e = *c->p++;
            switch (e) {
            case 'n': ch = '\n'; break;
            case 't': ch = '\t'; break;
            case 'r': ch = '\r'; break;
            case 'b': ch = '\b'; break;
            case 'f': ch = '\f'; break;
            case 'u': {
                unsigned cp;
                if (c->end - c->p < 4 || hex4(c->p, &cp) != 0)
                    return -1;
                c->p += 4;
                /* surrogate par */
                if (cp >= 0xD800 && cp < 0xDC00 && c->end - c->p >= 6 &&
                    c->p[0] == '\\' && c->p[1] == 'u') {
                    unsigned lo;
                    if (hex4(c->p + 2, &lo) == 0 && lo >= 0xDC00 && lo < 0xE000) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        c->p += 6;
                    }
                }
                put_utf8(dst, cap, &n, cp);
                continue;
            }
            default: ch = (unsigned char)e; break;  /* \" \\ \/ */
            }
        }
        if (n + 1 < cap)
            dst[n++] = (char)ch;
    }
    if (c->p >= c->end)
        return -1;
    c->p++;                       /* zatvarajuci navodnik */
    dst[n] = '\0';
    return 0;
}

/* Preskace vrijednost koja nas ne zanima (broj, true/false/null, string). */
static int skip_value(cur_t *c)
{
    skip_ws(c);
    if (c->p >= c->end)
        return -1;
    if (*c->p == '"') {
        char tmp[16];
        /* citamo do kraja stringa u mali bafer - sadrzaj se odbacuje */
        return read_string(c, tmp, sizeof tmp);
    }
    while (c->p < c->end && *c->p != ',' && *c->p != '}' &&
           !isspace((unsigned char)*c->p))
        c->p++;
    return 0;
}

static int is_ascii(const char *s)
{
    for (; *s; s++)
        if ((unsigned char)*s >= 128)
            return 0;
    return 1;
}

static int cat_index(movies_t *mv, const char *label)
{
    for (int i = 0; i < mv->n_cats; i++)
        if (strcmp(mv->cats[i], label) == 0)
            return i;
    if (mv->n_cats >= 32)
        return 0;
    snprintf(mv->cats[mv->n_cats], M_CAT, "%s", label);
    return mv->n_cats++;
}

static int cmp_title(const void *a, const void *b)
{
    return natural_cmp(((const movie_t *)a)->title, ((const movie_t *)b)->title);
}

int movies_parse(movies_t *mv, const char *json, size_t len)
{
    cur_t c = { json, json + len };
    int   cap = 16;
    char  key[64];
    char  tmp_cat_en[M_CAT];

    memset(mv, 0, sizeof *mv);
    mv->m    = calloc((size_t)cap, sizeof(movie_t));
    mv->cats = calloc(32, M_CAT);
    if (!mv->m || !mv->cats)
        goto fail;
    snprintf(mv->cats[0], M_CAT, "ALL");
    mv->n_cats = 1;

    /* UTF-8 BOM */
    if (len >= 3 && (unsigned char)json[0] == 0xEF &&
        (unsigned char)json[1] == 0xBB && (unsigned char)json[2] == 0xBF)
        c.p += 3;

    skip_ws(&c);
    if (c.p >= c.end || *c.p != '[')
        goto fail;
    c.p++;

    for (;;) {
        skip_ws(&c);
        if (c.p >= c.end)
            goto fail;
        if (*c.p == ']')
            break;
        if (*c.p == ',') { c.p++; continue; }
        if (*c.p != '{')
            goto fail;
        c.p++;

        if (mv->n == cap) {
            cap *= 2;
            movie_t *nm = realloc(mv->m, (size_t)cap * sizeof(movie_t));
            if (!nm)
                goto fail;
            mv->m = nm;
        }
        movie_t *m = &mv->m[mv->n];
        memset(m, 0, sizeof *m);
        tmp_cat_en[0] = '\0';

        for (;;) {
            skip_ws(&c);
            if (c.p >= c.end)
                goto fail;
            if (*c.p == '}') { c.p++; break; }
            if (*c.p == ',') { c.p++; continue; }
            if (read_string(&c, key, sizeof key) != 0)
                goto fail;
            skip_ws(&c);
            if (c.p >= c.end || *c.p != ':')
                goto fail;
            c.p++;
            skip_ws(&c);

            char  *dst = NULL;
            size_t cap_d = 0;
            if      (!strcmp(key, "title"))       { dst = m->title;      cap_d = M_TITLE; }
            else if (!strcmp(key, "category"))    { dst = m->category;   cap_d = M_CAT; }
            else if (!strcmp(key, "category_en")) { dst = tmp_cat_en;    cap_d = M_CAT; }
            else if (!strcmp(key, "poster_url"))  { dst = m->poster_url; cap_d = M_URL; }
            else if (!strcmp(key, "video_url"))   { dst = m->video_url;  cap_d = M_URL; }
            else if (!strcmp(key, "description")) { dst = m->desc;       cap_d = M_DESC; }

            if (dst && c.p < c.end && *c.p == '"') {
                if (read_string(&c, dst, cap_d) != 0)
                    goto fail;
            } else if (skip_value(&c) != 0) {
                goto fail;
            }
        }

        if (m->title[0] == '\0')
            continue;             /* objekat bez naslova se preskace */

        /* Oznaka kategorije: category_en > ASCII category > "Group N". */
        const char *lab = tmp_cat_en[0] ? tmp_cat_en : m->category;
        if (!lab[0])
            lab = "Other";
        if (is_ascii(lab)) {
            snprintf(m->cat_label, M_CAT, "%s", lab);
        } else {
            int found = -1;
            /* ista farsi kategorija -> ista oznaka */
            for (int i = 0; i < mv->n; i++)
                if (strcmp(mv->m[i].category, lab) == 0) {
                    found = i;
                    break;
                }
            if (found >= 0)
                snprintf(m->cat_label, M_CAT, "%s", mv->m[found].cat_label);
            else
                snprintf(m->cat_label, M_CAT, "Group %d", mv->n_cats);
        }
        cat_index(mv, m->cat_label);
        mv->n++;
    }

    if (mv->n == 0)
        goto fail;
    qsort(mv->m, (size_t)mv->n, sizeof(movie_t), cmp_title);
    return mv->n;

fail:
    movies_free(mv);
    return -1;
}

void movies_free(movies_t *mv)
{
    for (int i = 0; i < mv->n; i++) {
        free(mv->m[i].px);
        if (mv->m[i].tex)
            SDL_DestroyTexture(mv->m[i].tex);
    }
    free(mv->m);
    free(mv->cats);
    memset(mv, 0, sizeof *mv);
}

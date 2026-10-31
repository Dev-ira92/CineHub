/* text.c - TTF tekst preko stb_truetype, sa kesom glifova u SDL teksturama.
 * Oblikovanje (spajanje slova, RTL) radi shape.c.
 */
#include "text.h"
#include "common.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#define CACHE_SLOTS 2048
#define CACHE_MAX   1200          /* kad se dostigne, kes se prazni */
#define FONT_MAX    (32 * 1024 * 1024)

typedef struct {
    int          used;
    uint32_t     cp;
    int          px;
    SDL_Texture *tex;             /* NULL za razmak / prazan glif */
    int          xoff, yoff, w, h, adv;
} glyph_t;

struct text_font {
    SDL_Renderer  *r;
    unsigned char *data;
    stbtt_fontinfo info;
    glyph_t       *cache;
    int            n_cached;
};

text_font_t *font_open(SDL_Renderer *r, const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return NULL;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (n < 1024 || n > FONT_MAX) {
        fclose(fp);
        return NULL;
    }

    text_font_t *f = calloc(1, sizeof *f);
    if (!f) {
        fclose(fp);
        return NULL;
    }
    f->r = r;
    f->data = malloc((size_t)n);
    f->cache = calloc(CACHE_SLOTS, sizeof(glyph_t));
    if (!f->data || !f->cache || fread(f->data, 1, (size_t)n, fp) != (size_t)n) {
        fclose(fp);
        font_close(f);
        return NULL;
    }
    fclose(fp);

    int off = stbtt_GetFontOffsetForIndex(f->data, 0);
    if (off < 0 || !stbtt_InitFont(&f->info, f->data, off)) {
        ERR("font: %s nije valjan TTF", path);
        font_close(f);
        return NULL;
    }
    LOG("font ucitan: %s (%ld B)", path, n);
    return f;
}

static void cache_clear(text_font_t *f)
{
    for (int i = 0; i < CACHE_SLOTS; i++) {
        if (f->cache[i].tex)
            SDL_DestroyTexture(f->cache[i].tex);
        memset(&f->cache[i], 0, sizeof(glyph_t));
    }
    f->n_cached = 0;
}

void font_close(text_font_t *f)
{
    if (!f)
        return;
    if (f->cache)
        cache_clear(f);
    free(f->cache);
    free(f->data);
    free(f);
}

static glyph_t *get_glyph(text_font_t *f, uint32_t cp, int px)
{
    if (f->n_cached >= CACHE_MAX)
        cache_clear(f);

    uint32_t h = (cp * 2654435761u + (uint32_t)px * 40503u) % CACHE_SLOTS;
    glyph_t *g = NULL;

    for (int i = 0; i < CACHE_SLOTS; i++) {
        glyph_t *c = &f->cache[(h + (uint32_t)i) % CACHE_SLOTS];
        if (!c->used) {
            g = c;
            break;
        }
        if (c->cp == cp && c->px == px)
            return c;
    }
    if (!g)
        return NULL;

    g->used = 1;
    g->cp = cp;
    g->px = px;
    f->n_cached++;

    int glyph = stbtt_FindGlyphIndex(&f->info, (int)cp);
    if (glyph == 0 && cp != ' ')
        glyph = stbtt_FindGlyphIndex(&f->info, '?');

    float scale = stbtt_ScaleForPixelHeight(&f->info, (float)px);
    int adv = 0, lsb = 0;
    stbtt_GetGlyphHMetrics(&f->info, glyph, &adv, &lsb);
    g->adv = (int)(adv * scale + 0.5f);
    if (cp == ' ' && g->adv < px / 4)
        g->adv = px / 4;

    int w = 0, hh = 0, xo = 0, yo = 0;
    unsigned char *bm = glyph ? stbtt_GetGlyphBitmap(&f->info, scale, scale,
                                                     glyph, &w, &hh, &xo, &yo)
                              : NULL;
    if (bm && w > 0 && hh > 0) {
        uint32_t *px32 = malloc((size_t)w * hh * sizeof(uint32_t));
        if (px32) {
            for (int i = 0; i < w * hh; i++)
                px32[i] = 0x00FFFFFFu | ((uint32_t)bm[i] << 24);
            g->tex = SDL_CreateTexture(f->r, SDL_PIXELFORMAT_ABGR8888,
                                       SDL_TEXTUREACCESS_STATIC, w, hh);
            if (g->tex) {
                SDL_UpdateTexture(g->tex, NULL, px32, w * 4);
                SDL_SetTextureBlendMode(g->tex, SDL_BLENDMODE_BLEND);
            }
            free(px32);
        }
        g->w = w;
        g->h = hh;
        g->xoff = xo;
        g->yoff = yo;
    }
    if (bm)
        stbtt_FreeBitmap(bm, NULL);
    return g;
}

int font_width(text_font_t *f, int px, const char *utf8, int dir)
{
    uint32_t v[TEXT_MAX];
    int      rtl, n = text_visual(utf8, v, TEXT_MAX, dir, &rtl);
    int      w = 0;

    for (int i = 0; i < n; i++) {
        glyph_t *g = get_glyph(f, v[i], px);
        if (g)
            w += g->adv;
    }
    return w;
}

int font_draw(text_font_t *f, int box_x, int y, int box_w, int px,
              SDL_Color c, const char *utf8, int dir)
{
    uint32_t v[TEXT_MAX];
    int      rtl, n = text_visual(utf8, v, TEXT_MAX, dir, &rtl);
    int      asc = 0, desc = 0, gap = 0;

    stbtt_GetFontVMetrics(&f->info, &asc, &desc, &gap);
    float scale    = stbtt_ScaleForPixelHeight(&f->info, (float)px);
    int   baseline = y + (int)(asc * scale + 0.5f);

    int width = 0;
    for (int i = 0; i < n; i++) {
        glyph_t *g = get_glyph(f, v[i], px);
        if (g)
            width += g->adv;
    }

    int x = box_x;
    if (rtl && box_w > 0)
        x = box_x + box_w - width;

    for (int i = 0; i < n; i++) {
        glyph_t *g = get_glyph(f, v[i], px);
        if (!g)
            continue;
        if (g->tex) {
            SDL_Rect dst = { x + g->xoff, baseline + g->yoff, g->w, g->h };
            SDL_SetTextureColorMod(g->tex, c.r, c.g, c.b);
            SDL_SetTextureAlphaMod(g->tex, c.a);
            SDL_RenderCopy(f->r, g->tex, NULL, &dst);
        }
        x += g->adv;
    }
    return width;
}

int font_draw_wrapped(text_font_t *f, int box_x, int y, int box_w, int px,
                      int line_h, int max_lines, SDL_Color c, const char *utf8)
{
    uint32_t tmp[TEXT_MAX];
    int      para_rtl = 0;
    text_visual(utf8, tmp, TEXT_MAX, -1, &para_rtl);

    char line[1100], trial[2200];
    int  lines = 0;
    line[0] = '\0';

    const char *p = utf8;
    while (lines < max_lines) {
        while (*p == ' ')
            p++;
        const char *e = p;
        while (*e && *e != ' ')
            e++;
        size_t wl = (size_t)(e - p);
        int    last = (*p == '\0');

        if (!last && wl > 0 && wl < sizeof line / 2) {
            if (line[0])
                snprintf(trial, sizeof trial, "%s %.*s", line, (int)wl, p);
            else
                snprintf(trial, sizeof trial, "%.*s", (int)wl, p);

            if (line[0] && font_width(f, px, trial, para_rtl) > box_w) {
                /* rijec ne staje: ispisi tekuci red, rijec ide u sljedeci */
                font_draw(f, box_x, y + lines * line_h, box_w, px, c, line,
                          para_rtl);
                lines++;
                line[0] = '\0';
                continue;                     /* ponovo probaj istu rijec */
            }
            snprintf(line, sizeof line, "%s", trial);
            p = e;
            continue;
        }
        if (!last && wl > 0) {                /* nenormalno duga rijec */
            p = e;
            continue;
        }
        if (line[0] && lines < max_lines) {
            font_draw(f, box_x, y + lines * line_h, box_w, px, c, line,
                      para_rtl);
            lines++;
        }
        break;
    }
    return lines;
}

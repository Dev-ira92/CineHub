/* ui.c */
#include "ui.h"
#include "common.h"

#include <stdarg.h>

/* font8x8_basic.h dovlaci scripts/deps.sh (public domain, dhepper/font8x8).
 * Definise: unsigned char font8x8_basic[128][8]; bit 0 = levi piksel. */
#include "font8x8_basic.h"

#define GLYPH_W 8
#define GLYPH_H 8
#define ATLAS_COLS 16
#define ATLAS_ROWS 8

/* Tema "Cinema": topla crna i zlato. */
const SDL_Color COL_BG     = {  12,  10,   7, 255 };
const SDL_Color COL_PANEL  = {  30,  26,  18, 255 };
const SDL_Color COL_SEL    = {  58,  46,  16, 255 };
const SDL_Color COL_TEXT   = { 243, 236, 221, 255 };
const SDL_Color COL_DIM    = { 156, 146, 124, 255 };
const SDL_Color COL_ACCENT = { 212, 175,  55, 255 };   /* zlato */
const SDL_Color COL_HUD    = {   0,   0,   0, 185 };
const SDL_Color COL_LINE   = { 212, 175,  55, 120 };

/* Gradijent pozadine: topla tamna smedja pri vrhu, skoro crno pri dnu. */
static SDL_Texture *make_background(SDL_Renderer *r)
{
    enum { BW = 4, BH = 256 };
    static uint32_t px[BW * BH];
    const int top[3] = { 30, 24, 14 }, bot[3] = { 4, 4, 3 };

    for (int y = 0; y < BH; y++) {
        double t = (double)y / (BH - 1);
        t = t * t * (3.0 - 2.0 * t);             /* glatko */
        uint32_t c[3];
        for (int k = 0; k < 3; k++)
            c[k] = (uint32_t)(top[k] + (bot[k] - top[k]) * t + 0.5);
        uint32_t v = 0xFF000000u | (c[2] << 16) | (c[1] << 8) | c[0];  /* ABGR */
        for (int x = 0; x < BW; x++)
            px[y * BW + x] = v;
    }
    SDL_Texture *t = SDL_CreateTexture(r, SDL_PIXELFORMAT_ABGR8888,
                                       SDL_TEXTUREACCESS_STATIC, BW, BH);
    if (t)
        SDL_UpdateTexture(t, NULL, px, BW * 4);
    return t;
}

int ui_init(ui_t *ui, SDL_Renderer *r, int w, int h)
{
    memset(ui, 0, sizeof(*ui));
    ui->r        = r;
    ui->screen_w = w;
    ui->screen_h = h;

    int aw = ATLAS_COLS * GLYPH_W;
    int ah = ATLAS_ROWS * GLYPH_H;

    /* Atlas se gradi jednom, u RAM-u, pa se otprema na GPU. Beli glifovi
     * na providnoj pozadini - boja se kasnije dobija color modulacijom. */
    uint32_t *px = calloc((size_t)aw * ah, sizeof(uint32_t));
    if (!px)
        return -1;

    for (int ch = 0; ch < 128; ch++) {
        int gx = (ch % ATLAS_COLS) * GLYPH_W;
        int gy = (ch / ATLAS_COLS) * GLYPH_H;

        for (int row = 0; row < GLYPH_H; row++) {
            unsigned char bits = font8x8_basic[ch][row];
            for (int col = 0; col < GLYPH_W; col++) {
                if (bits & (1u << col))
                    px[(size_t)(gy + row) * aw + (gx + col)] = 0xFFFFFFFFu;
            }
        }
    }

    ui->font = SDL_CreateTexture(r, SDL_PIXELFORMAT_ABGR8888,
                                 SDL_TEXTUREACCESS_STATIC, aw, ah);
    if (!ui->font) {
        ERR("atlas fonta: %s", SDL_GetError());
        free(px);
        return -1;
    }
    SDL_UpdateTexture(ui->font, NULL, px, aw * 4);
    SDL_SetTextureBlendMode(ui->font, SDL_BLENDMODE_BLEND);
    free(px);

    ui->bg = make_background(r);                 /* ako ne uspije: ravna boja */

    return 0;
}

void ui_shutdown(ui_t *ui)
{
    if (ui->font)
        SDL_DestroyTexture(ui->font);
    ui->font = NULL;
    if (ui->bg)
        SDL_DestroyTexture(ui->bg);
    ui->bg = NULL;
}

void ui_set_color(ui_t *ui, SDL_Color c)
{
    SDL_SetRenderDrawColor(ui->r, c.r, c.g, c.b, c.a);
}

void ui_fill_rect(ui_t *ui, int x, int y, int w, int h, SDL_Color c)
{
    SDL_Rect rc = { x, y, w, h };
    SDL_SetRenderDrawBlendMode(ui->r, SDL_BLENDMODE_BLEND);
    ui_set_color(ui, c);
    SDL_RenderFillRect(ui->r, &rc);
}

int ui_text_width(int scale, const char *s)
{
    return (int)strlen(s) * GLYPH_W * scale;
}

void ui_text(ui_t *ui, int x, int y, int scale, SDL_Color c, const char *fmt, ...)
{
    char    buf[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    SDL_SetTextureColorMod(ui->font, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(ui->font, c.a);

    int cx = x;
    for (const unsigned char *p = (const unsigned char *)buf; *p; p++) {
        unsigned char ch = *p;

        /* Font pokriva samo ASCII. Nasa slova iz UTF-8 bi se raspala na
         * dva bajta smeca, pa se sve iznad 0x7F crta kao '?'. */
        if (ch >= 128)
            ch = '?';

        if (ch != ' ') {
            SDL_Rect src = { (ch % ATLAS_COLS) * GLYPH_W,
                             (ch / ATLAS_COLS) * GLYPH_H,
                             GLYPH_W, GLYPH_H };
            SDL_Rect dst = { cx, y, GLYPH_W * scale, GLYPH_H * scale };
            SDL_RenderCopy(ui->r, ui->font, &src, &dst);
        }
        cx += GLYPH_W * scale;
    }
}

void ui_ellipsize(char *dst, size_t dstlen, const char *src, int max_chars)
{
    size_t n = strlen(src);

    if (max_chars < 4 || n <= (size_t)max_chars) {
        snprintf(dst, dstlen, "%s", src);
        return;
    }
    int keep = max_chars - 3;
    snprintf(dst, dstlen, "%.*s...", keep, src);
}

/* ---------------- tema ---------------- */

void ui_background(ui_t *ui)
{
    if (ui->bg) {
        SDL_Rect dst = { 0, 0, ui->screen_w, ui->screen_h };
        SDL_RenderCopy(ui->r, ui->bg, NULL, &dst);
    } else {
        SDL_SetRenderDrawColor(ui->r, COL_BG.r, COL_BG.g, COL_BG.b, 255);
        SDL_RenderClear(ui->r);
    }
}

/* Filmska traka: tamna pruga sa zlatnim rupicama i tankom linijom ispod. */
void ui_filmstrip(ui_t *ui, int y, int h)
{
    const SDL_Color base = { 6, 5, 4, 255 };
    const SDL_Color hole = { 212, 175, 55, 85 };

    ui_fill_rect(ui, 0, y, ui->screen_w, h, base);
    for (int x = 18; x + 16 < ui->screen_w; x += 40)
        ui_fill_rect(ui, x, y + 6, 16, h - 12, hole);
    ui_fill_rect(ui, 0, y + h, ui->screen_w, 2, COL_LINE);
}

static void border(ui_t *ui, int x, int y, int w, int h, int t, SDL_Color c)
{
    ui_fill_rect(ui, x, y, w, t, c);
    ui_fill_rect(ui, x, y + h - t, w, t, c);
    ui_fill_rect(ui, x, y + t, t, h - 2 * t, c);
    ui_fill_rect(ui, x + w - t, y + t, t, h - 2 * t, c);
}

/* Zlatni okvir sa mekim sjajem sa vanjske strane (izabrani poster). */
void ui_glow_frame(ui_t *ui, int x, int y, int w, int h)
{
    const SDL_Color g1 = { 212, 175, 55, 36 }, g2 = { 212, 175, 55, 90 };

    border(ui, x - 12, y - 12, w + 24, h + 24, 6, g1);
    border(ui, x - 6, y - 6, w + 12, h + 12, 6, g2);
    border(ui, x, y, w, h, 4, COL_ACCENT);
}

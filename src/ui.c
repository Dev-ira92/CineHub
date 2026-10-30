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

const SDL_Color COL_BG     = {  18,  18,  20, 255 };
const SDL_Color COL_PANEL  = {  30,  30,  34, 255 };
const SDL_Color COL_SEL    = {  56,  92, 148, 255 };
const SDL_Color COL_TEXT   = { 232, 232, 236, 255 };
const SDL_Color COL_DIM    = { 140, 140, 148, 255 };
const SDL_Color COL_ACCENT = { 120, 176, 255, 255 };
const SDL_Color COL_HUD    = {   0,   0,   0, 170 };

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

    return 0;
}

void ui_shutdown(ui_t *ui)
{
    if (ui->font)
        SDL_DestroyTexture(ui->font);
    ui->font = NULL;
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

/* ui.h - crtanje. Bez SDL_ttf: koristi se ugradjeni 8x8 bitmap font,
 * skaliran po potrebi. Dovoljno za listu fajlova i broj stranice, a
 * uklanja jednu cross-kompajliranu zavisnost.
 */
#ifndef UI_H
#define UI_H

#include <SDL2/SDL.h>

typedef struct {
    SDL_Renderer *r;
    SDL_Texture  *font;   /* atlas 16x8 glifova po 8x8 piksela */
    int           screen_w, screen_h;
} ui_t;

/* Paleta. Jedno mjesto za sve ekrane - browser, citac i splash. */
extern const SDL_Color COL_BG;
extern const SDL_Color COL_PANEL;
extern const SDL_Color COL_SEL;
extern const SDL_Color COL_TEXT;
extern const SDL_Color COL_DIM;
extern const SDL_Color COL_ACCENT;
extern const SDL_Color COL_HUD;

int  ui_init(ui_t *ui, SDL_Renderer *r, int w, int h);
void ui_shutdown(ui_t *ui);

void ui_set_color(ui_t *ui, SDL_Color c);
void ui_fill_rect(ui_t *ui, int x, int y, int w, int h, SDL_Color c);

/* Ispisuje tekst; `scale` je celobrojni faktor uvecanja glifa. */
void ui_text(ui_t *ui, int x, int y, int scale, SDL_Color c, const char *fmt, ...);
int  ui_text_width(int scale, const char *s);

/* Skracuje predugacak string na `max_chars` sa "..." na kraju. */
void ui_ellipsize(char *dst, size_t dstlen, const char *src, int max_chars);

#endif /* UI_H */

/* text.h - TTF tekst (farsi/arapski) preko stb_truetype.
 *
 * text_visual(): UTF-8 -> redoslijed za crtanje (spajanje slova + RTL).
 *                Cista funkcija, ne treba SDL ni stb.
 * font_*:        crtanje tog teksta u SDL rendereru, sa kesom glifova.
 */
#ifndef TEXT_H
#define TEXT_H

#include <SDL2/SDL.h>
#include <stdint.h>

#define TEXT_MAX 512             /* najvise kodnih tacaka po stringu */

/* dir: -1 auto, 0 LTR, 1 RTL. Vraca broj kodnih tacaka u `out`
 * (vizuelni redoslijed, sleva nadesno). *is_rtl = osnovni smjer. */
int  text_visual(const char *utf8, uint32_t *out, int cap, int dir,
                 int *is_rtl);

typedef struct text_font text_font_t;

/* NULL ako fajl ne postoji ili nije validan TTF. */
text_font_t *font_open(SDL_Renderer *r, const char *path);
void         font_close(text_font_t *f);

/* Sirina jednog reda u pikselima (visina slova `px`). */
int  font_width(text_font_t *f, int px, const char *utf8, int dir);

/* Crta jedan red unutar kutije [box_x, box_x+box_w). RTL se poravnava
 * udesno, LTR ulijevo. y = gornja ivica reda. Vraca sirinu reda. */
int  font_draw(text_font_t *f, int box_x, int y, int box_w, int px,
               SDL_Color c, const char *utf8, int dir);

/* Prelama po rijecima. Vraca broj nacrtanih redova. */
int  font_draw_wrapped(text_font_t *f, int box_x, int y, int box_w, int px,
                       int line_h, int max_lines, SDL_Color c,
                       const char *utf8);

/* 1 ako string sadrzi bajt >= 0x80. */
int  text_has_nonascii(const char *s);

#endif

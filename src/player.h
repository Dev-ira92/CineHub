/* player.h - ugradjeni plejer, faza 1: samo video (bez zvuka), softverski
 * dekod preko FFmpeg-a. Dijagnosticki natpis pokazuje koliko brzo kon-
 * zola dekodira (faza 2 dodaje zvuk, faza 3 kontrole i titlove).
 *
 * Niti: demux (mreza + MKV) -> red paketa -> dekod + pretvaranje u RGBA ->
 * red slika -> glavna nit (player_update) ih prikazuje na vrijeme.
 */
#ifndef PLAYER_H
#define PLAYER_H

#include <SDL2/SDL.h>
#include "ui.h"

typedef struct player player_t;

/* url: "http://...", "https://..." ili putanja fajla. Otvaranje ide u niti,
 * pa se odmah vraca; stanje se vidi u player_draw. NULL ako nema memorije. */
player_t *player_open(SDL_Renderer *r, const char *url, const char *title,
                      int disp_w, int disp_h);
void      player_close(player_t *p);

/* Glavna nit, svaki kadar: prikazuje sliku kad joj dodje vrijeme. */
void      player_update(player_t *p);

/* Crta cijeli ekran (video + natpis). */
void      player_draw(player_t *p, ui_t *ui);

/* SDL_CONTROLLER_BUTTON_*. Vraca 1 kad treba zatvoriti plejer. */
int       player_button(player_t *p, int button);

#endif

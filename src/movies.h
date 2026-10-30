/* movies.h - ucitavanje movies.json (niz ravnih objekata) */
#ifndef MOVIES_H
#define MOVIES_H

#include <SDL2/SDL.h>

#define M_TITLE   128
#define M_CAT     64
#define M_URL     512
#define M_DESC    1024

#define POSTER_W  240
#define POSTER_H  360

typedef struct {
    char title[M_TITLE];
    char category[M_CAT];     /* ono sto pise u JSON-u (moze biti farsi) */
    char cat_label[M_CAT];    /* ASCII oznaka za prikaz */
    char poster_url[M_URL];
    char video_url[M_URL];
    char desc[M_DESC];

    /* Poster: nit za preuzimanje puni px, glavna nit pravi teksturu. */
    SDL_atomic_t pstate;      /* 0 ceka, 1 px spreman, 2 tekstura, 3 greska */
    unsigned char *px;        /* POSTER_W*POSTER_H*4 RGBA */
    SDL_Texture  *tex;
} movie_t;

typedef struct {
    movie_t *m;
    int      n;
    char   (*cats)[M_CAT];    /* [0] = "ALL" */
    int      n_cats;
} movies_t;

/* Parsira JSON iz memorije. Vraca broj filmova ili -1. */
int  movies_parse(movies_t *mv, const char *json, size_t len);
void movies_free(movies_t *mv);

#endif

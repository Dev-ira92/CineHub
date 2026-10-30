/* common.h - zajednicke definicije */
#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>

#define APP_NAME    "Cine Hub"
#define APP_VERSION "0.2"

/* Payload nema stdout na ekranu; elfldr preusmerava stderr na svoj socket,
 * pa je fprintf(stderr) najpouzdaniji kanal za debug tokom razvoja. */
#define LOG(fmt, ...)  fprintf(stderr, "[cine] " fmt "\n", ##__VA_ARGS__)
#define ERR(fmt, ...)  fprintf(stderr, "[cine][err] " fmt "\n", ##__VA_ARGS__)

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

/* Poredjenje imena fajlova tako da "page2.jpg" dodje pre "page10.jpg".
 * Bez ovoga se stranice stripa citaju u pogresnom redosledu. */
int natural_cmp(const char *a, const char *b);

/* Vraca pokazivac na ekstenziju (bez tacke) ili "" ako je nema. */
const char *path_ext(const char *path);

/* Vraca pokazivac na ime fajla unutar putanje. */
const char *path_base(const char *path);

/* Vraca 1 ako putanja pocinje sa http:// (bez obzira na velicinu slova). */
int  is_url(const char *path);

/* Dekodira %XX sekvence. Nepotpun escape se prepisuje doslovno. */
void url_decode(char *dst, size_t dstlen, const char *src);

#endif /* COMMON_H */

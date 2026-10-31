/* main.c - Cine Hub v0.4: mreza postera + kategorije + detalji + import JSON
 * + farsi tekst (TTF).
 *
 * Pokrece se kroz websrv (Homebrew Loader), NE kroz elfldr:9021.
 * Reprodukcija videa jos nije dodata (faza 3).
 */
#include <stdarg.h>
#include <SDL2/SDL.h>
#include <curl/curl.h>
#include <dirent.h>
#include <sys/stat.h>

#include "common.h"
#include "ui.h"
#include "movies.h"
#include "text.h"
#include "stb_image.h"

#ifndef APP_DIR
#define APP_DIR      "/data/homebrew/Cinema"
#endif
#define IMPORT_DIR   APP_DIR "/imports"
#define JSON_LOCAL   APP_DIR "/movies.json"
#define JSON_BACKUP  APP_DIR "/movies.json.bak"
#define JSON_URLFILE APP_DIR "/movies_url.txt"
#define FONT_FILE    APP_DIR "/font.ttf"

#define COLS      6
#define CELL_W    (POSTER_W + 40)
#define CELL_H    (POSTER_H + 70)
#define GRID_X    60
#define GRID_Y    150
#define MAX_DL    (12 * 1024 * 1024)
#define MAX_JSON  (8 * 1024 * 1024)

#define MAX_IMP   64
#define IMP_ROW_H 52
#define IMP_TOP   190

typedef enum { SCR_GRID, SCR_DETAIL, SCR_IMPORT } screen_t;

typedef struct {
    char path[256];
    long size;
} imp_t;

typedef struct {
    ui_t        ui;
    text_font_t *font;          /* NULL ako nema font.ttf */
    movies_t    mv;
    int         have;           /* ima ucitanih filmova */
    int        *vis;            /* indeksi filmova u trenutnoj kategoriji */
    int         n_vis;
    int         cat;
    int         sel;            /* pozicija u vis[] */
    int         top_row;
    screen_t    screen;
    int         rows_fit;
    int         running;
    SDL_Thread *th;             /* nit za postere */

    /* import */
    imp_t       imp[MAX_IMP];
    int         n_imp, imp_sel, imp_top, imp_rows;
    char        msg[160];
    uint32_t    msg_until;

    /* ponavljanje d-pada */
    int         held_btn;
    uint32_t    held_since, held_last;
} app_t;

/* Zajednicka zastavica za zaustavljanje niti za postere i curl prenosa. */
static SDL_atomic_t g_stop;

static void set_msg(app_t *a, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a->msg, sizeof a->msg, fmt, ap);
    va_end(ap);
    a->msg_until = SDL_GetTicks() + 6000;
    LOG("%s", a->msg);
}

/* ---------------- HTTP ---------------- */

typedef struct { unsigned char *d; size_t n, cap; int over; } membuf_t;

static size_t on_data(char *p, size_t sz, size_t nm, void *ud)
{
    membuf_t *b = ud;
    size_t    n = sz * nm;

    if (b->n + n > MAX_DL) {
        b->over = 1;
        return 0;                       /* curl prekida prenos */
    }
    if (b->n + n + 1 > b->cap) {
        size_t nc = (b->cap ? b->cap * 2 : 65536);
        while (nc < b->n + n + 1)
            nc *= 2;
        unsigned char *nd = realloc(b->d, nc);
        if (!nd)
            return 0;
        b->d = nd;
        b->cap = nc;
    }
    memcpy(b->d + b->n, p, n);
    b->n += n;
    b->d[b->n] = 0;
    return n;
}

/* Vraca != 0 da prekine prenos (curl) kad se trazi zaustavljanje. */
static int on_progress(void *ud, curl_off_t dt, curl_off_t dn,
                       curl_off_t ut, curl_off_t un)
{
    return SDL_AtomicGet(&g_stop);
}

/* 0 = ok. Pozivalac oslobadja out->d. */
static int http_get(const char *url, membuf_t *out)
{
    memset(out, 0, sizeof *out);
    CURL *c = curl_easy_init();
    if (!c)
        return -1;
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 40L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, on_progress);
    /* Na konzoli nema pouzdanog CA bundle-a. Za postere i spisak filmova
     * se provjera certifikata gasi; nikakvi podaci o korisniku se ne salju. */
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    CURLcode rc = curl_easy_perform(c);
    long     http = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);
    curl_easy_cleanup(c);

    if (rc != CURLE_OK || http >= 400 || out->n == 0) {
        free(out->d);
        memset(out, 0, sizeof *out);
        return -1;
    }
    return 0;
}

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > MAX_JSON) {
        fclose(f);
        return NULL;
    }
    char *b = malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        b = NULL;
    }
    fclose(f);
    if (b) {
        b[n] = 0;
        *len = (size_t)n;
    }
    return b;
}

/* ---------------- poster ---------------- */

/* Smanjuje RGBA na POSTER_W x POSTER_H (prosjek 2x2 uzorka po pikselu). */
static unsigned char *scale_poster(const unsigned char *s, int sw, int sh)
{
    unsigned char *o = malloc((size_t)POSTER_W * POSTER_H * 4);
    if (!o)
        return NULL;

    for (int y = 0; y < POSTER_H; y++) {
        for (int x = 0; x < POSTER_W; x++) {
            unsigned acc[4] = { 0, 0, 0, 0 };
            for (int k = 0; k < 4; k++) {
                int fx = (int)(((long long)(x * 2 + (k & 1)) * 2 + 1) * sw /
                               (POSTER_W * 4));
                int fy = (int)(((long long)(y * 2 + (k >> 1)) * 2 + 1) * sh /
                               (POSTER_H * 4));
                if (fx >= sw) fx = sw - 1;
                if (fy >= sh) fy = sh - 1;
                const unsigned char *p = s + ((size_t)fy * sw + fx) * 4;
                for (int ch = 0; ch < 4; ch++)
                    acc[ch] += p[ch];
            }
            unsigned char *d = o + ((size_t)y * POSTER_W + x) * 4;
            for (int ch = 0; ch < 4; ch++)
                d[ch] = (unsigned char)(acc[ch] / 4);
        }
    }
    return o;
}

static int poster_thread(void *ud)
{
    movies_t *mv = ud;

    for (int i = 0; i < mv->n && !SDL_AtomicGet(&g_stop); i++) {
        movie_t *m = &mv->m[i];
        membuf_t b;

        if (!m->poster_url[0] || http_get(m->poster_url, &b) != 0) {
            SDL_AtomicSet(&m->pstate, 3);
            continue;
        }
        int w = 0, h = 0, comp = 0;
        unsigned char *img = stbi_load_from_memory(b.d, (int)b.n, &w, &h,
                                                   &comp, 4);
        free(b.d);
        if (!img || w < 2 || h < 2) {
            if (img)
                stbi_image_free(img);
            ERR("poster %d: dekodiranje nije uspjelo", i);
            SDL_AtomicSet(&m->pstate, 3);
            continue;
        }
        m->px = scale_poster(img, w, h);
        stbi_image_free(img);
        SDL_AtomicSet(&m->pstate, m->px ? 1 : 3);
    }
    return 0;
}

/* Glavna nit: pretvara spremne piksele u teksture (najvise 2 po kadru). */
static void upload_posters(app_t *a, SDL_Renderer *r)
{
    int budget = 2;

    for (int i = 0; i < a->mv.n && budget > 0; i++) {
        movie_t *m = &a->mv.m[i];
        if (SDL_AtomicGet(&m->pstate) != 1)
            continue;
        m->tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32,
                                   SDL_TEXTUREACCESS_STATIC,
                                   POSTER_W, POSTER_H);
        if (m->tex)
            SDL_UpdateTexture(m->tex, NULL, m->px, POSTER_W * 4);
        free(m->px);
        m->px = NULL;
        SDL_AtomicSet(&m->pstate, m->tex ? 2 : 3);
        budget--;
    }
}

static void stop_posters(app_t *a)
{
    SDL_AtomicSet(&g_stop, 1);
    if (a->th)
        SDL_WaitThread(a->th, NULL);
    a->th = NULL;
    SDL_AtomicSet(&g_stop, 0);
}

/* ---------------- filter ---------------- */

static void rebuild_vis(app_t *a)
{
    a->n_vis = 0;
    for (int i = 0; i < a->mv.n; i++)
        if (a->cat == 0 ||
            strcmp(a->mv.m[i].cat_label, a->mv.cats[a->cat]) == 0)
            a->vis[a->n_vis++] = i;
    a->sel = 0;
    a->top_row = 0;
}

static void scroll_to_sel(app_t *a)
{
    int row = a->sel / COLS;
    if (row < a->top_row)
        a->top_row = row;
    if (row >= a->top_row + a->rows_fit)
        a->top_row = row - a->rows_fit + 1;
}

/* Preuzima nm (vlasnistvo prelazi na app), pokrece postere. */
static void install_movies(app_t *a, movies_t *nm)
{
    stop_posters(a);
    movies_free(&a->mv);
    free(a->vis);
    a->mv  = *nm;
    a->vis = malloc(sizeof(int) * (size_t)a->mv.n);
    a->cat = 0;
    a->have = a->vis != NULL;
    if (a->have) {
        rebuild_vis(a);
        a->th = SDL_CreateThread(poster_thread, "posters", &a->mv);
    }
}

/* ---------------- import ---------------- */

static int has_json_ext(const char *name)
{
    size_t n = strlen(name);
    return n > 5 && strcasecmp(name + n - 5, ".json") == 0;
}

static void scan_dir(app_t *a, const char *dir)
{
    DIR *dp = opendir(dir);
    if (!dp)
        return;

    struct dirent *de;
    while ((de = readdir(dp)) != NULL && a->n_imp < MAX_IMP) {
        if (de->d_name[0] == '.' || !has_json_ext(de->d_name))
            continue;

        imp_t *it = &a->imp[a->n_imp];
        struct stat st;
        snprintf(it->path, sizeof it->path, "%s/%s", dir, de->d_name);
        if (stat(it->path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0)
            continue;
        it->size = (long)st.st_size;
        a->n_imp++;
    }
    closedir(dp);
}

static void scan_imports(app_t *a)
{
    a->n_imp = 0;
    a->imp_sel = 0;
    a->imp_top = 0;
    scan_dir(a, IMPORT_DIR);
    for (int i = 0; i < 8; i++) {
        char d[24];
        snprintf(d, sizeof d, "/mnt/usb%d", i);
        scan_dir(a, d);
    }
}

static void do_import(app_t *a, const char *path)
{
    size_t len = 0;
    char  *js  = read_file(path, &len);

    if (!js) {
        set_msg(a, "Cannot read file (empty or over 8 MB)");
        return;
    }

    movies_t nm;
    int      n = movies_parse(&nm, js, len);
    if (n <= 0) {
        free(js);
        set_msg(a, "Invalid JSON or no movies with a title");
        return;
    }

    /* Stari movies.json se cuva kao .bak, pa se greska moze vratiti. */
    rename(JSON_LOCAL, JSON_BACKUP);
    FILE *f = fopen(JSON_LOCAL, "wb");
    int   saved = f && fwrite(js, 1, len, f) == len;
    if (f)
        fclose(f);
    free(js);

    install_movies(a, &nm);
    a->screen = SCR_GRID;
    if (saved)
        set_msg(a, "Imported %d movies (old list kept as movies.json.bak)", n);
    else
        set_msg(a, "Imported %d movies but could not save to %s", n, JSON_LOCAL);
}

static void open_import(app_t *a)
{
    scan_imports(a);
    a->screen = SCR_IMPORT;
}

/* ---------------- crtanje ---------------- */

static void frame(ui_t *ui, int x, int y, int w, int h, int t, SDL_Color c)
{
    ui_fill_rect(ui, x, y, w, t, c);
    ui_fill_rect(ui, x, y + h - t, w, t, c);
    ui_fill_rect(ui, x, y, t, h, c);
    ui_fill_rect(ui, x + w - t, y, t, h, c);
}

static void clear(ui_t *ui)
{
    SDL_SetRenderDrawColor(ui->r, COL_BG.r, COL_BG.g, COL_BG.b, 255);
    SDL_RenderClear(ui->r);
}

static void draw_msg(app_t *a, int y)
{
    if (a->msg[0] && SDL_GetTicks() < a->msg_until)
        ui_text(&a->ui, 60, y, 2, COL_ACCENT, "%s", a->msg);
}

/* Naziv kategorije za prikaz: izvorni (farsi) ako ima fonta. */
static const char *cat_display(app_t *a, int cat)
{
    if (cat == 0 || !a->font)
        return a->mv.cats[cat];
    for (int i = 0; i < a->mv.n; i++)
        if (strcmp(a->mv.m[i].cat_label, a->mv.cats[cat]) == 0 &&
            a->mv.m[i].category[0])
            return a->mv.m[i].category;
    return a->mv.cats[cat];
}

/* Skracuje UTF-8 string tako da stane u maxw piksela (rez na granici znaka). */
static void fit_text(app_t *a, int px, char *dst, size_t cap, const char *src,
                     int maxw)
{
    snprintf(dst, cap, "%s", src);
    if (font_width(a->font, px, dst, -1) <= maxw)
        return;
    size_t n = strlen(dst);
    while (n > 0) {
        n--;
        while (n > 0 && ((unsigned char)dst[n] & 0xC0) == 0x80)
            n--;                          /* ne rezi usred UTF-8 znaka */
        dst[n] = '\0';
        char t[256];
        snprintf(t, sizeof t, "%s...", dst);
        if (font_width(a->font, px, t, -1) <= maxw) {
            snprintf(dst, cap, "%s", t);
            return;
        }
    }
}

static void draw_grid(app_t *a)
{
    ui_t *ui = &a->ui;

    clear(ui);
    ui_text(ui, 60, 40, 5, COL_ACCENT, "CINE HUB");
    {
        const char *cn = cat_display(a, a->cat);
        int x1 = 60 + ui_text_width(2, "L1/R1: category   ");
        char cnt[32];
        snprintf(cnt, sizeof cnt, "(%d/%d)", a->n_vis ? a->sel + 1 : 0,
                 a->n_vis);
        ui_text(ui, 60, 100, 2, COL_DIM, "L1/R1: category   ");
        int wname;
        if (a->font && text_has_nonascii(cn)) {
            wname = font_draw(a->font, x1, 94, 0, 28, COL_DIM, cn, -1);
        } else {
            ui_text(ui, x1, 100, 2, COL_DIM, "%s", cn);
            wname = ui_text_width(2, cn);
        }
        ui_text(ui, x1 + wname + 24, 100, 2, COL_DIM, "%s", cnt);
    }

    int first = a->top_row * COLS;
    int last  = MIN(a->n_vis, (a->top_row + a->rows_fit) * COLS);

    for (int p = first; p < last; p++) {
        movie_t *m = &a->mv.m[a->vis[p]];
        int col = p % COLS;
        int row = p / COLS - a->top_row;
        int x = GRID_X + col * (CELL_W + 8);
        int y = GRID_Y + row * CELL_H;
        int sel = (p == a->sel);

        if (sel)
            ui_fill_rect(ui, x - 14, y - 14, POSTER_W + 28, CELL_H - 20,
                         COL_SEL);

        if (m->tex) {
            SDL_Rect dst = { x, y, POSTER_W, POSTER_H };
            SDL_RenderCopy(ui->r, m->tex, NULL, &dst);
        } else {
            ui_fill_rect(ui, x, y, POSTER_W, POSTER_H, COL_PANEL);
            ui_text(ui, x + 12, y + POSTER_H / 2 - 8, 2, COL_DIM,
                    SDL_AtomicGet(&m->pstate) == 3 ? "no poster" : "loading");
        }
        if (sel)
            frame(ui, x - 4, y - 4, POSTER_W + 8, POSTER_H + 8, 4,
                  COL_ACCENT);

        if (a->font && text_has_nonascii(m->title)) {
            char t[160];
            fit_text(a, 26, t, sizeof t, m->title, POSTER_W);
            font_draw(a->font, x, y + POSTER_H + 8, POSTER_W, 26,
                      sel ? COL_TEXT : COL_DIM, t, -1);
        } else {
            char t[40];
            ui_ellipsize(t, sizeof t, m->title, POSTER_W / 16);
            ui_text(ui, x, y + POSTER_H + 12, 2, sel ? COL_TEXT : COL_DIM,
                    "%s", t);
        }
    }

    draw_msg(a, ui->screen_h - 84);
    ui_text(ui, 60, ui->screen_h - 44, 2, COL_DIM,
            "D-pad: move   X: open   Triangle: import JSON   O: exit");
}

static void draw_detail(app_t *a)
{
    ui_t *ui = &a->ui;
    movie_t *m = &a->mv.m[a->vis[a->sel]];

    clear(ui);

    SDL_Rect dst = { 100, 150, POSTER_W * 2, POSTER_H * 2 };
    if (m->tex)
        SDL_RenderCopy(ui->r, m->tex, NULL, &dst);
    else
        ui_fill_rect(ui, dst.x, dst.y, dst.w, dst.h, COL_PANEL);

    int x  = 100 + POSTER_W * 2 + 80;
    int bw = ui->screen_w - x - 60;
    char t[160];

    if (a->font && text_has_nonascii(m->title)) {
        fit_text(a, 54, t, sizeof t, m->title, bw);
        font_draw(a->font, x, 150, bw, 54, COL_TEXT, t, -1);
    } else {
        ui_ellipsize(t, sizeof t, m->title, bw / 32);
        ui_text(ui, x, 160, 4, COL_TEXT, "%s", t);
    }

    if (a->font && m->category[0] && text_has_nonascii(m->category))
        font_draw(a->font, x, 232, bw, 32, COL_ACCENT, m->category, -1);
    else
        ui_text(ui, x, 240, 2, COL_ACCENT, "%s", m->cat_label);

    if (m->desc[0] && a->font) {
        font_draw_wrapped(a->font, x, 300, bw, 30, 44, 10, COL_DIM, m->desc);
    } else if (m->desc[0] && !text_has_nonascii(m->desc)) {
        char line[96];
        int  y = 300, n = (int)strlen(m->desc), pos = 0;
        while (pos < n && y < 700) {
            snprintf(line, sizeof line, "%.*s", 60, m->desc + pos);
            ui_text(ui, x, y, 2, COL_DIM, "%s", line);
            pos += 60;
            y += 28;
        }
    } else if (m->desc[0]) {
        ui_text(ui, x, 300, 2, COL_DIM, "(description: put font.ttf in %s)",
                APP_DIR);
    }

    char u[96];
    ui_ellipsize(u, sizeof u, m->video_url[0] ? m->video_url : "-", 60);
    ui_text(ui, x, 780, 2, COL_DIM, "Video: %s", u);
    ui_text(ui, x, 840, 3, COL_ACCENT, "Playback: not added yet");
    ui_text(ui, 60, ui->screen_h - 44, 2, COL_DIM, "O: back");
}

static void draw_import(app_t *a)
{
    ui_t *ui = &a->ui;

    clear(ui);
    ui_text(ui, 60, 40, 5, COL_ACCENT, "IMPORT JSON");
    ui_text(ui, 60, 110, 2, COL_DIM, "Looking in: %s  and  /mnt/usb0..7",
            IMPORT_DIR);
    ui_text(ui, 60, 140, 2, COL_DIM,
            "Replaces your current movie list (old one is kept as .bak)");

    if (a->n_imp == 0) {
        ui_text(ui, 60, IMP_TOP + 20, 3, COL_TEXT, "No .json files found");
        ui_text(ui, 60, IMP_TOP + 80, 2, COL_DIM,
                "Copy a .json to the USB root or to %s", IMPORT_DIR);
    }

    int last = MIN(a->n_imp, a->imp_top + a->imp_rows);
    for (int i = a->imp_top; i < last; i++) {
        int y = IMP_TOP + (i - a->imp_top) * IMP_ROW_H;
        int sel = (i == a->imp_sel);
        if (sel)
            ui_fill_rect(ui, 50, y - 8, ui->screen_w - 100, IMP_ROW_H - 4,
                         COL_SEL);
        char p[100];
        ui_ellipsize(p, sizeof p, a->imp[i].path, 90);
        ui_text(ui, 60, y, 2, sel ? COL_TEXT : COL_DIM, "%s", p);
        ui_text(ui, ui->screen_w - 260, y, 2, COL_DIM, "%ld KB",
                (a->imp[i].size + 1023) / 1024);
    }

    draw_msg(a, ui->screen_h - 84);
    ui_text(ui, 60, ui->screen_h - 44, 2, COL_DIM,
            "Up/Down: choose   X: import   O: back");
}

static void draw_empty(app_t *a)
{
    ui_t *ui = &a->ui;

    clear(ui);
    ui_text(ui, 60, 40, 5, COL_ACCENT, "CINE HUB");
    ui_text(ui, 60, 140, 3, COL_TEXT, "No movie list yet");
    ui_text(ui, 60, 200, 2, COL_DIM, "Put movies.json in: %s", APP_DIR);
    ui_text(ui, 60, 240, 2, COL_DIM, "or press Triangle to import a .json");
    draw_msg(a, ui->screen_h - 84);
    ui_text(ui, 60, ui->screen_h - 44, 2, COL_DIM,
            "Triangle: import JSON   O: exit");
}

/* ---------------- unos ---------------- */

static void move_sel(app_t *a, int d)
{
    if (!a->n_vis)
        return;
    int s = a->sel + d;
    if (s < 0 || s >= a->n_vis)
        return;
    a->sel = s;
    scroll_to_sel(a);
}

static void change_cat(app_t *a, int d)
{
    a->cat = (a->cat + d + a->mv.n_cats) % a->mv.n_cats;
    rebuild_vis(a);
}

static void import_move(app_t *a, int d)
{
    int s = a->imp_sel + d;
    if (s < 0 || s >= a->n_imp)
        return;
    a->imp_sel = s;
    if (s < a->imp_top)
        a->imp_top = s;
    if (s >= a->imp_top + a->imp_rows)
        a->imp_top = s - a->imp_rows + 1;
}

static void on_button(app_t *a, int b)
{
    if (a->screen == SCR_IMPORT) {
        switch (b) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:   import_move(a, -1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: import_move(a, 1);  break;
        case SDL_CONTROLLER_BUTTON_A:
            if (a->n_imp)
                do_import(a, a->imp[a->imp_sel].path);
            break;
        case SDL_CONTROLLER_BUTTON_B:
            a->screen = SCR_GRID;
            break;
        default:
            break;
        }
        return;
    }
    if (!a->have) {
        if (b == SDL_CONTROLLER_BUTTON_Y)
            open_import(a);
        else if (b == SDL_CONTROLLER_BUTTON_B)
            a->running = 0;
        return;
    }
    if (a->screen == SCR_DETAIL) {
        if (b == SDL_CONTROLLER_BUTTON_B)
            a->screen = SCR_GRID;
        return;
    }
    switch (b) {
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  move_sel(a, -1);    break;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: move_sel(a, 1);     break;
    case SDL_CONTROLLER_BUTTON_DPAD_UP:    move_sel(a, -COLS); break;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  move_sel(a, COLS);  break;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  change_cat(a, -1); break;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: change_cat(a, 1);  break;
    case SDL_CONTROLLER_BUTTON_A:
        if (a->n_vis)
            a->screen = SCR_DETAIL;
        break;
    case SDL_CONTROLLER_BUTTON_Y:
        open_import(a);
        break;
    case SDL_CONTROLLER_BUTTON_B:
        a->running = 0;
        break;
    default:
        break;
    }
}

static int is_dpad(int b)
{
    return b == SDL_CONTROLLER_BUTTON_DPAD_UP ||
           b == SDL_CONTROLLER_BUTTON_DPAD_DOWN ||
           b == SDL_CONTROLLER_BUTTON_DPAD_LEFT ||
           b == SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
}

/* ---------------- ucitavanje spiska ---------------- */

static int load_movies(movies_t *mv)
{
    size_t len = 0;
    char  *js = NULL;

    /* Neobavezno: movies_url.txt sa jednim URL-om; ako uspije, ima prednost. */
    char *u = read_file(JSON_URLFILE, &len);
    if (u) {
        char *e = u + strcspn(u, "\r\n");
        *e = '\0';
        membuf_t b;
        if (u[0] && http_get(u, &b) == 0) {
            js = (char *)b.d;
            len = b.n;
            LOG("spisak filmova sa mreze (%zu B)", len);
        } else {
            ERR("movies_url.txt: preuzimanje nije uspjelo, koristim lokalni");
        }
        free(u);
    }
    if (!js) {
        js = read_file(JSON_LOCAL, &len);
        if (!js)
            ERR("nema %s", JSON_LOCAL);
    }
    if (!js)
        return -1;

    int n = movies_parse(mv, js, len);
    free(js);
    if (n < 0)
        ERR("movies.json se ne moze parsirati");
    return n;
}

/* ---------------- main ---------------- */

int main(int argc, char **argv)
{
    app_t a;
    memset(&a, 0, sizeof a);
    a.running = 1;
    a.held_btn = -1;

    LOG("start %s 0.4", APP_NAME);

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        ERR("SDL_Init VIDEO: %s", SDL_GetError());
        return 1;
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0)
        ERR("SDL_Init GAMECONTROLLER: %s", SDL_GetError());

    SDL_Window *win = SDL_CreateWindow(APP_NAME, SDL_WINDOWPOS_UNDEFINED,
                                       SDL_WINDOWPOS_UNDEFINED, 1920, 1080,
                                       SDL_WINDOW_FULLSCREEN_DESKTOP);
    if (!win) {
        ERR("SDL_CreateWindow: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    /* Na PS5 akcelerisani renderer ne postoji (vidi ComicReader SETUP.md). */
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) {
        ERR("SDL_CreateRenderer: %s", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }

    int sw = 1920, sh = 1080;
    SDL_GetRendererOutputSize(ren, &sw, &sh);
    if (ui_init(&a.ui, ren, sw, sh) != 0)
        return 1;
    a.font = font_open(ren, FONT_FILE);
    if (!a.font)
        LOG("nema fonta %s - farsi tekst ce biti '?'", FONT_FILE);
    a.rows_fit = MAX(1, (sh - GRID_Y - 60) / CELL_H);
    a.imp_rows = MAX(1, (sh - IMP_TOP - 120) / IMP_ROW_H);

    /* Prvi kadar odmah, da ekran ne ostane crn dok se spisak ucitava. */
    clear(&a.ui);
    ui_text(&a.ui, 60, 40, 5, COL_ACCENT, "CINE HUB");
    ui_text(&a.ui, 60, 120, 2, COL_DIM, "Loading movie list...");
    SDL_RenderPresent(ren);

    movies_t first;
    if (load_movies(&first) > 0)
        install_movies(&a, &first);

    SDL_GameController *gc = NULL;
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i) && (gc = SDL_GameControllerOpen(i)))
            break;
    if (!gc)
        ERR("kontroler nije nadjen");

    while (a.running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                a.running = 0;
            } else if (ev.type == SDL_CONTROLLERBUTTONDOWN) {
                int b = ev.cbutton.button;
                on_button(&a, b);
                if (is_dpad(b)) {
                    a.held_btn = b;
                    a.held_since = a.held_last = SDL_GetTicks();
                }
            } else if (ev.type == SDL_CONTROLLERBUTTONUP) {
                if (ev.cbutton.button == a.held_btn)
                    a.held_btn = -1;
            } else if (ev.type == SDL_CONTROLLERDEVICEADDED && !gc) {
                gc = SDL_GameControllerOpen(ev.cdevice.which);
            } else if (ev.type == SDL_KEYDOWN &&
                       ev.key.keysym.sym == SDLK_ESCAPE) {
                a.running = 0;
            }
        }

        /* Ponavljanje d-pada: 350 ms zadrska, pa svakih 90 ms. */
        if (a.held_btn >= 0) {
            uint32_t now = SDL_GetTicks();
            if (now - a.held_since > 350 && now - a.held_last > 90) {
                on_button(&a, a.held_btn);
                a.held_last = now;
            }
        }

        if (a.have)
            upload_posters(&a, ren);

        if (a.screen == SCR_IMPORT)
            draw_import(&a);
        else if (!a.have)
            draw_empty(&a);
        else if (a.screen == SCR_GRID)
            draw_grid(&a);
        else
            draw_detail(&a);

        SDL_RenderPresent(ren);
        SDL_Delay(16);
    }

    stop_posters(&a);
    movies_free(&a.mv);
    free(a.vis);
    font_close(a.font);
    ui_shutdown(&a.ui);
    if (gc)
        SDL_GameControllerClose(gc);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    curl_global_cleanup();
    LOG("kraj");
    (void)argc; (void)argv;
    return 0;
}

/* player.c - ugradjeni plejer, faza 2: video + zvuk (vidi player.h).
 *
 * Zvuk: FFmpeg dekodira, libswresample pretvara u 48 kHz 16-bit stereo,
 * a nit za izlaz ga salje u sceAudioOut. Sat zvuka je glavni sat: slike se
 * prikazuju prema njemu, a kad kasne, preskacu se. Ako zvuka nema ili izlaz
 * ne radi, koristi se zid-sat kao u fazi 1. */
#include <errno.h>
#include <stdarg.h>
#include <curl/curl.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>

#include "player.h"
#include "common.h"

#ifndef PLAYER_THREADS
#define PLAYER_THREADS 6          /* niti dekodera FFmpeg-a */
#endif

#define HTTP_CHUNK   (4 * 1024 * 1024)
#define AVIO_BUF     65536
#define PQ_MAX       256          /* paketa u redu */
#define PQ_BYTES     (32 * 1024 * 1024)
#define FQ_N         6            /* slika (RGBA) u redu */
#define START_FRAMES 3            /* toliko slika prije starta sata */

#define A_RATE       48000
#define A_RING_FR    (A_RATE)     /* 1 s zvuka u prstenu */
#define A_START_FR   (A_RATE / 5) /* 0.2 s zvuka prije starta */
#define AQ_MAX       512
#define AQ_BYTES     (8 * 1024 * 1024)

enum { ST_OPENING = 0, ST_PLAYING, ST_ENDED, ST_ERROR };

/* ---------------------------------------------------------- audio izlaz
 * sceAudioOut dolazi iz SDK-ovih stubova (-lSceAudioOut). Deklaracije su
 * "weak" pa se program linka i bez biblioteke; tada zvuka jednostavno nema. */

extern int sceAudioOutInit(void) __attribute__((weak));
extern int sceAudioOutOpen(int32_t user, int32_t type, int32_t index,
                           uint32_t len, uint32_t freq, uint32_t param)
    __attribute__((weak));
extern int sceAudioOutOutput(int32_t handle, const void *buf)
    __attribute__((weak));

static int  g_ao_port = -2;          /* -2 = jos nije pokusano */
static int  g_ao_grain;
static char g_ao_err[100];

/* Port se otvara jednom za cijeli proces i koristi se za sve filmove. */
static int ao_open_once(void)
{
    static const uint32_t grains[] = { 1024, 256 };

    if (g_ao_port != -2)
        return g_ao_port;
    if (!sceAudioOutInit || !sceAudioOutOpen || !sceAudioOutOutput) {
        snprintf(g_ao_err, sizeof g_ao_err, "audio library not linked");
        return g_ao_port = -1;
    }
    sceAudioOutInit();
    int h = -1;
    for (size_t i = 0; i < sizeof grains / sizeof *grains; i++) {
        /* korisnik 0xff = sistem, glavni port, 16-bit stereo (kao VLC-PS5) */
        h = sceAudioOutOpen(0xff, 0, 0, grains[i], A_RATE, 1);
        if (h >= 0) {
            g_ao_grain = (int)grains[i];
            LOG("audio out: port %#x, grain %d", h, g_ao_grain);
            return g_ao_port = h;
        }
    }
    snprintf(g_ao_err, sizeof g_ao_err, "sceAudioOutOpen failed (%#x)",
             (unsigned)h);
    return g_ao_port = -1;
}

/* ---------------------------------------------------------------- HTTP */

typedef struct {
    CURL     *c;
    char      url[1024];
    int64_t   size;               /* -1 nepoznato */
    int64_t   pos;                /* logicka pozicija citanja */
    uint8_t  *buf;                /* prozor kesa */
    int64_t   buf_start;
    int       buf_len;
    int       want, fill, full;   /* tekuci zahtjev */
    int64_t   hdr_total;
    SDL_atomic_t *stop;
    char      err[160];
} httpsrc_t;

static size_t hs_header(char *p, size_t sz, size_t nm, void *ud)
{
    httpsrc_t *s = ud;
    size_t     n = sz * nm;

    if (n > 15 && !strncasecmp(p, "content-range:", 14)) {
        char tmp[200];
        snprintf(tmp, sizeof tmp, "%.*s", (int)(n < 199 ? n : 199), p);
        const char *slash = strrchr(tmp, '/');
        if (slash && slash[1] >= '0' && slash[1] <= '9')
            s->hdr_total = atoll(slash + 1);
    }
    return n;
}

static size_t hs_body(char *p, size_t sz, size_t nm, void *ud)
{
    httpsrc_t *s = ud;
    size_t     n = sz * nm;

    if (s->fill + n > (size_t)s->want) {
        n = (size_t)(s->want - s->fill);
        s->full = 1;                       /* dosta: prekini prenos */
        if (n)
            memcpy(s->buf + s->fill, p, n);
        s->fill += (int)n;
        return 0;
    }
    memcpy(s->buf + s->fill, p, n);
    s->fill += (int)n;
    return n;
}

static int hs_progress(void *ud, curl_off_t a, curl_off_t b, curl_off_t c,
                       curl_off_t d)
{
    httpsrc_t *s = ud;
    return SDL_AtomicGet(s->stop);
}

/* Ucitava prozor od `start`. 0 = ok (buf_len moze biti 0 = kraj), -1 = greska. */
static int hs_fetch(httpsrc_t *s, int64_t start)
{
    int64_t want = HTTP_CHUNK;
    if (s->size >= 0) {
        if (start >= s->size) {
            s->buf_start = start;
            s->buf_len = 0;
            return 0;
        }
        if (start + want > s->size)
            want = s->size - start;
    }
    char range[64];
    snprintf(range, sizeof range, "%lld-%lld", (long long)start,
             (long long)(start + want - 1));

    s->want = (int)want;
    s->fill = 0;
    s->full = 0;
    s->hdr_total = -1;
    curl_easy_setopt(s->c, CURLOPT_RANGE, range);
    CURLcode rc = curl_easy_perform(s->c);
    long code = 0;
    curl_easy_getinfo(s->c, CURLINFO_RESPONSE_CODE, &code);

    if (code == 416) {                    /* iza kraja */
        s->buf_start = start;
        s->buf_len = 0;
        return 0;
    }
    if (rc != CURLE_OK && !(s->full && rc == CURLE_WRITE_ERROR)) {
        snprintf(s->err, sizeof s->err, "network error (%d, HTTP %ld)", (int)rc,
                 code);
        return -1;
    }
    if (code == 200) {
        /* Server ne podrzava Range. Prihvatljivo samo ako je cijeli (mali)
         * fajl stigao u jednom odgovoru. */
        if (start == 0 && !s->full && rc == CURLE_OK) {
            s->size = s->fill;
        } else {
            snprintf(s->err, sizeof s->err,
                     "server does not support Range requests");
            return -1;
        }
    }
    if (code != 200 && code != 206) {
        snprintf(s->err, sizeof s->err, "HTTP error %ld", code);
        return -1;
    }
    if (s->hdr_total > 0)
        s->size = s->hdr_total;
    s->buf_start = start;
    s->buf_len = s->fill;
    return 0;
}

static int hs_open(httpsrc_t *s, const char *url, SDL_atomic_t *stop)
{
    memset(s, 0, sizeof *s);
    snprintf(s->url, sizeof s->url, "%s", url);
    s->size = -1;
    s->stop = stop;
    s->buf = av_malloc(HTTP_CHUNK);
    s->c = curl_easy_init();
    if (!s->buf || !s->c) {
        snprintf(s->err, sizeof s->err, "out of memory");
        return -1;
    }
    curl_easy_setopt(s->c, CURLOPT_URL, s->url);
    curl_easy_setopt(s->c, CURLOPT_WRITEFUNCTION, hs_body);
    curl_easy_setopt(s->c, CURLOPT_WRITEDATA, s);
    curl_easy_setopt(s->c, CURLOPT_HEADERFUNCTION, hs_header);
    curl_easy_setopt(s->c, CURLOPT_HEADERDATA, s);
    curl_easy_setopt(s->c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(s->c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(s->c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(s->c, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(s->c, CURLOPT_LOW_SPEED_TIME, 25L);
    curl_easy_setopt(s->c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(s->c, CURLOPT_XFERINFOFUNCTION, hs_progress);
    curl_easy_setopt(s->c, CURLOPT_XFERINFODATA, s);
    curl_easy_setopt(s->c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(s->c, CURLOPT_SSL_VERIFYHOST, 0L);
    return hs_fetch(s, 0);                 /* uzima i ukupnu velicinu */
}

static void hs_close(httpsrc_t *s)
{
    if (s->c)
        curl_easy_cleanup(s->c);
    av_free(s->buf);
    s->c = NULL;
    s->buf = NULL;
}

static int hs_read(void *opaque, uint8_t *buf, int size)
{
    httpsrc_t *s = opaque;

    if (SDL_AtomicGet(s->stop))
        return AVERROR_EXIT;
    if (s->size >= 0 && s->pos >= s->size)
        return AVERROR_EOF;
    if (!(s->pos >= s->buf_start && s->pos < s->buf_start + s->buf_len)) {
        if (hs_fetch(s, s->pos) != 0)
            return AVERROR(EIO);
        if (s->buf_len == 0)
            return AVERROR_EOF;
    }
    int64_t avail = s->buf_start + s->buf_len - s->pos;
    int     n = size < avail ? size : (int)avail;
    memcpy(buf, s->buf + (s->pos - s->buf_start), (size_t)n);
    s->pos += n;
    return n;
}

static int64_t hs_seek(void *opaque, int64_t off, int whence)
{
    httpsrc_t *s = opaque;

    if (whence & AVSEEK_SIZE)
        return s->size;
    whence &= ~AVSEEK_FORCE;
    switch (whence) {
    case SEEK_SET: s->pos = off; break;
    case SEEK_CUR: s->pos += off; break;
    case SEEK_END:
        if (s->size < 0)
            return -1;
        s->pos = s->size + off;
        break;
    default:
        return -1;
    }
    if (s->pos < 0)
        s->pos = 0;
    return s->pos;
}

/* -------------------------------------------------------------- plejer */

typedef struct {
    AVPacket **pk;
    int        cap, head, count;
    int64_t    bytes, max_bytes;
} pqueue_t;

struct player {
    SDL_Renderer *r;
    int           disp_w, disp_h;
    char          url[1024], title[160];

    SDL_atomic_t  stop;
    SDL_atomic_t  state;
    char          err[200];          /* upisuje se prije state = ST_ERROR */

    AVFormatContext *fmt;
    AVIOContext     *avio;
    httpsrc_t        src;
    int              use_src;
    AVCodecContext  *dec;
    int              vstream;
    AVRational       tb;
    double           duration;       /* s, 0 = nepoznato */
    char             codec[24];
    int              src_w, src_h;
    double           src_fps;

    SDL_Thread *th_demux, *th_dec;

    SDL_mutex *pq_m;                 /* jedan lokot za oba reda paketa */
    SDL_cond  *pq_cv;
    pqueue_t   vq, aq;
    int        demux_eof;

    /* zvuk */
    int              astream;        /* -1 = nema */
    volatile int     audio_off;      /* iskljucen (nema izlaza, greska...) */
    char             audio_note[100];
    char             a_codec[24];
    AVCodecContext  *adec;
    AVRational       atb;
    struct SwrContext *swr;
    char             a_info[64];
    SDL_Thread      *th_adec, *th_aout;
    SDL_mutex       *ar_m;           /* prsten PCM-a */
    SDL_cond        *ar_cv;
    int16_t         *ring;           /* A_RING_FR stereo okvira */
    int              ar_rd, ar_wr, ar_fill;
    double           ar_rd_pts, ar_wr_pts;
    int              a_have_first, a_started, a_done;
    double           a_lat;
    SDL_atomic_t     a_pause;
    int              underruns;
    double           av_diff;        /* video - zvuk, s (EMA) */
    SDL_atomic_t     behind;         /* video kasni: dekoder preskace B-slike */
    double           late_ema;       /* koliko slike kasne iza zvuka, s */

    SDL_mutex *fq_m;
    SDL_cond  *fq_cv;
    struct { uint8_t *px; double pts; } fq[FQ_N];
    int        fq_head, fq_count;
    int        out_w, out_h;         /* postavlja nit dekodera */
    int        decode_done;

    /* glavna nit */
    SDL_Texture *tex;
    int          clock_started, paused, overlay;
    double       clock_base, pause_began, last_pts;
    int          shown, dropped;
    double       fps;
    double       fps_t0;
    int          fps_n0;

    /* mjerenja (nit dekodera) */
    double       dec_ms, conv_ms;
};

static double now_s(void)
{
    return (double)SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency();
}

static void ff_log(void *avcl, int level, const char *fmt, va_list vl)
{
    if (level > AV_LOG_WARNING)
        return;
    char line[512];
    vsnprintf(line, sizeof line, fmt, vl);
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
        line[--n] = 0;
    if (n)
        fprintf(stderr, "[cine][ff] %s\n", line);
}

static void set_error(player_t *p, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->err, sizeof p->err, fmt, ap);
    va_end(ap);
    ERR("player: %s", p->err);
    SDL_AtomicSet(&p->state, ST_ERROR);
}

static void set_error_av(player_t *p, const char *what, int rc)
{
    char buf[100];
    av_strerror(rc, buf, sizeof buf);
    set_error(p, "%s: %s", what, buf);
}

static int interrupted(void *opaque)
{
    player_t *p = opaque;
    return SDL_AtomicGet(&p->stop);
}

/* ---- redovi paketa ---- */

static int pq_init(pqueue_t *q, int cap, int64_t max_bytes)
{
    q->pk = calloc((size_t)cap, sizeof *q->pk);
    q->cap = cap;
    q->max_bytes = max_bytes;
    return q->pk ? 0 : -1;
}

static void pq_free(pqueue_t *q)
{
    while (q->count > 0) {
        AVPacket *k = q->pk[q->head];
        q->head = (q->head + 1) % q->cap;
        q->count--;
        av_packet_free(&k);
    }
    free(q->pk);
    q->pk = NULL;
}

static int pq_push(player_t *p, pqueue_t *q, AVPacket *pkt)
{
    SDL_LockMutex(p->pq_m);
    while ((q->count >= q->cap || q->bytes > q->max_bytes) &&
           !SDL_AtomicGet(&p->stop))
        SDL_CondWaitTimeout(p->pq_cv, p->pq_m, 50);
    if (SDL_AtomicGet(&p->stop)) {
        SDL_UnlockMutex(p->pq_m);
        return -1;
    }
    q->pk[(q->head + q->count) % q->cap] = pkt;
    q->count++;
    q->bytes += pkt->size;
    SDL_CondBroadcast(p->pq_cv);
    SDL_UnlockMutex(p->pq_m);
    return 0;
}

/* NULL = kraj toka (ili zaustavljanje). */
static AVPacket *pq_pop(player_t *p, pqueue_t *q)
{
    AVPacket *pkt = NULL;

    SDL_LockMutex(p->pq_m);
    while (q->count == 0 && !p->demux_eof && !SDL_AtomicGet(&p->stop))
        SDL_CondWaitTimeout(p->pq_cv, p->pq_m, 50);
    if (q->count > 0 && !SDL_AtomicGet(&p->stop)) {
        pkt = q->pk[q->head];
        q->head = (q->head + 1) % q->cap;
        q->count--;
        q->bytes -= pkt->size;
        SDL_CondBroadcast(p->pq_cv);
    }
    SDL_UnlockMutex(p->pq_m);
    return pkt;
}

/* ---- dekod ---- */

static int choose_out_size(player_t *p, const AVFrame *f)
{
    double sar = 1.0;
    if (f->sample_aspect_ratio.num > 0 && f->sample_aspect_ratio.den > 0)
        sar = (double)f->sample_aspect_ratio.num / f->sample_aspect_ratio.den;
    double dw = f->width * sar, dh = f->height;
    double k = (double)p->disp_w / dw;
    if ((double)p->disp_h / dh < k)
        k = (double)p->disp_h / dh;
    int w = ((int)(dw * k)) & ~1, h = ((int)(dh * k)) & ~1;
    if (w < 16 || h < 16)
        return -1;
    for (int i = 0; i < FQ_N; i++) {
        p->fq[i].px = av_malloc((size_t)w * h * 4);
        if (!p->fq[i].px)
            return -1;
    }
    SDL_LockMutex(p->fq_m);
    p->out_w = w;
    p->out_h = h;
    SDL_UnlockMutex(p->fq_m);
    return 0;
}

static int dec_thread(void *ud)
{
    player_t *p = ud;
    AVFrame  *f = av_frame_alloc();
    struct SwsContext *sws = NULL;
    int last_w = 0, last_h = 0, last_fmt = -1, last_range = -1;
    double next_pts = 0;
    int    draining = 0;

    if (!f)
        goto done;

    for (;;) {
        double t0;
        int    got = 0, rc;

        if (!draining) {
            AVPacket *pkt = pq_pop(p, &p->vq);
            t0 = now_s();                  /* ne racunaj cekanje na mrezu */
            if (!pkt) {
                if (SDL_AtomicGet(&p->stop))
                    break;
                draining = 1;
                avcodec_send_packet(p->dec, NULL);
            } else {
                /* Ako video kasni za zvukom, preskoci nereferentne slike
                 * (B-slike): dekod postaje bitno brzi, a sat se hvata. */
                enum AVDiscard want = SDL_AtomicGet(&p->behind) ? AVDISCARD_NONREF
                                                                : AVDISCARD_DEFAULT;
                if (p->dec->skip_frame != want)
                    p->dec->skip_frame = want;
                rc = avcodec_send_packet(p->dec, pkt);
                av_packet_free(&pkt);
                if (rc < 0 && rc != AVERROR(EAGAIN)) {
                    fprintf(stderr, "[cine] decode: send_packet %d\n", rc);
                    continue;
                }
            }
        }

        if (draining)
            t0 = now_s();
        while (!SDL_AtomicGet(&p->stop)) {
            rc = avcodec_receive_frame(p->dec, f);
            if (rc < 0)
                break;
            got++;
            double t1 = now_s();

            if (p->out_w == 0 && choose_out_size(p, f) != 0) {
                set_error(p, "cannot allocate the picture buffers");
                av_frame_unref(f);
                goto done;
            }

            /* slobodno mjesto u redu slika */
            int slot;
            SDL_LockMutex(p->fq_m);
            while (p->fq_count >= FQ_N && !SDL_AtomicGet(&p->stop))
                SDL_CondWaitTimeout(p->fq_cv, p->fq_m, 20);
            slot = (p->fq_head + p->fq_count) % FQ_N;
            SDL_UnlockMutex(p->fq_m);
            if (SDL_AtomicGet(&p->stop)) {
                av_frame_unref(f);
                break;
            }

            double t2 = now_s();
            int range = f->color_range == AVCOL_RANGE_JPEG;
            if (f->width != last_w || f->height != last_h ||
                f->format != last_fmt || range != last_range) {
                sws = sws_getCachedContext(sws, f->width, f->height,
                                           (enum AVPixelFormat)f->format,
                                           p->out_w, p->out_h, AV_PIX_FMT_RGBA,
                                           SWS_FAST_BILINEAR, NULL, NULL, NULL);
                if (!sws) {
                    set_error(p, "swscale cannot convert this picture format");
                    av_frame_unref(f);
                    goto done;
                }
                const int *co = sws_getCoefficients(
                    f->height >= 720 ? SWS_CS_ITU709 : SWS_CS_ITU601);
                sws_setColorspaceDetails(sws, co, range,
                                         sws_getCoefficients(SWS_CS_DEFAULT), 1,
                                         0, 1 << 16, 1 << 16);
                last_w = f->width;
                last_h = f->height;
                last_fmt = f->format;
                last_range = range;
                p->src_w = f->width;
                p->src_h = f->height;
            }
            uint8_t *dst[4] = { p->fq[slot].px, NULL, NULL, NULL };
            int      dst_stride[4] = { p->out_w * 4, 0, 0, 0 };
            sws_scale(sws, (const uint8_t *const *)f->data, f->linesize, 0,
                      f->height, dst, dst_stride);
            double t3 = now_s();

            double pts;
            if (f->best_effort_timestamp != AV_NOPTS_VALUE)
                pts = f->best_effort_timestamp * av_q2d(p->tb);
            else
                pts = next_pts;
            next_pts = pts + 1.0 / 24.0;
            p->fq[slot].pts = pts;

            SDL_LockMutex(p->fq_m);
            p->fq_count++;
            SDL_CondBroadcast(p->fq_cv);
            SDL_UnlockMutex(p->fq_m);

            p->conv_ms = p->conv_ms * 0.9 + (t3 - t2) * 1000.0 * 0.1;
            p->dec_ms = p->dec_ms * 0.9 + (t1 - t0) * 1000.0 * 0.1;
            t0 = now_s();
            av_frame_unref(f);
        }

        if (draining)
            break;
        (void)got;
    }

done:
    sws_freeContext(sws);
    av_frame_free(&f);
    SDL_LockMutex(p->fq_m);
    p->decode_done = 1;
    SDL_CondBroadcast(p->fq_cv);
    SDL_UnlockMutex(p->fq_m);
    return 0;
}

/* ---- zvuk ---- */

/* Pise `frames` okvira u prsten (src == NULL = tisina). Blokira dok nema
 * mjesta. Poziva se s zakljucanim ar_m; vraca 0, ili -1 pri zaustavljanju. */
static int ring_write_locked(player_t *p, const int16_t *src, int frames)
{
    while (frames > 0) {
        while (p->ar_fill >= A_RING_FR && !SDL_AtomicGet(&p->stop))
            SDL_CondWaitTimeout(p->ar_cv, p->ar_m, 20);
        if (SDL_AtomicGet(&p->stop))
            return -1;
        int n = A_RING_FR - p->ar_fill;
        if (n > frames)
            n = frames;
        int first = A_RING_FR - p->ar_wr;
        if (first > n)
            first = n;
        if (src) {
            memcpy(p->ring + (size_t)p->ar_wr * 2, src, (size_t)first * 4);
            if (n > first)
                memcpy(p->ring, src + (size_t)first * 2, (size_t)(n - first) * 4);
            src += (size_t)n * 2;
        } else {
            memset(p->ring + (size_t)p->ar_wr * 2, 0, (size_t)first * 4);
            if (n > first)
                memset(p->ring, 0, (size_t)(n - first) * 4);
        }
        p->ar_wr = (p->ar_wr + n) % A_RING_FR;
        p->ar_fill += n;
        p->ar_wr_pts += (double)n / A_RATE;
        frames -= n;
    }
    return 0;
}

/* Dodaje dekodirani zvuk cije je prvi uzorak u trenutku pts (s). Poravnava
 * po vremenu: rupu popuni tisinom, a zakasnjeli pocetak odbaci. */
static void audio_push(player_t *p, const int16_t *pcm, int frames, double pts)
{
    SDL_LockMutex(p->ar_m);
    if (!p->a_have_first) {
        p->ar_rd_pts = p->ar_wr_pts = pts;
        p->a_have_first = 1;
    }
    double gap = pts - p->ar_wr_pts;
    if (gap > 2.0 || gap < -2.0) {                /* skok: kreni ispocetka */
        p->ar_rd = p->ar_wr = p->ar_fill = 0;
        p->ar_rd_pts = p->ar_wr_pts = pts;
        gap = 0;
    }
    if (gap > 0.04) {
        if (ring_write_locked(p, NULL, (int)(gap * A_RATE)) != 0)
            goto out;
    } else if (gap < -0.04) {
        int drop = (int)(-gap * A_RATE);
        if (drop >= frames)
            goto out;
        pcm += (size_t)drop * 2;
        frames -= drop;
    }
    ring_write_locked(p, pcm, frames);
out:
    SDL_UnlockMutex(p->ar_m);
}

static int init_swr(player_t *p, const AVFrame *f, AVChannelLayout *cur_layout,
                    int *cur_fmt, int *cur_rate)
{
    AVChannelLayout in, out;

    if (f->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
        av_channel_layout_default(&in, f->ch_layout.nb_channels);
    else
        av_channel_layout_copy(&in, &f->ch_layout);
    if (p->swr && *cur_fmt == f->format && *cur_rate == f->sample_rate &&
        av_channel_layout_compare(cur_layout, &in) == 0) {
        av_channel_layout_uninit(&in);
        return 0;
    }
    swr_free(&p->swr);
    av_channel_layout_default(&out, 2);
    int rc = swr_alloc_set_opts2(&p->swr, &out, AV_SAMPLE_FMT_S16, A_RATE, &in,
                                 (enum AVSampleFormat)f->format,
                                 f->sample_rate, 0, NULL);
    if (rc >= 0) {
        av_opt_set_double(p->swr, "rematrix_maxval", 1.0, 0);  /* bez klipinga */
        rc = swr_init(p->swr);
    }
    av_channel_layout_uninit(&out);
    if (rc < 0) {
        swr_free(&p->swr);
        av_channel_layout_uninit(&in);
        return -1;
    }
    av_channel_layout_uninit(cur_layout);
    av_channel_layout_copy(cur_layout, &in);
    av_channel_layout_uninit(&in);
    *cur_fmt = f->format;
    *cur_rate = f->sample_rate;
    snprintf(p->a_info, sizeof p->a_info, "%dch %d Hz -> stereo %d Hz",
             f->ch_layout.nb_channels, f->sample_rate, A_RATE);
    return 0;
}

static int adec_thread(void *ud)
{
    player_t *p = ud;
    AVFrame  *f = av_frame_alloc();
    AVChannelLayout cur = { 0 };
    int      cur_fmt = -1, cur_rate = 0, draining = 0, cap = 0;
    int16_t *out = NULL;

    if (!f)
        goto done;

    for (;;) {
        if (!draining) {
            AVPacket *pkt = pq_pop(p, &p->aq);
            if (!pkt) {
                if (SDL_AtomicGet(&p->stop))
                    break;
                draining = 1;
                avcodec_send_packet(p->adec, NULL);
            } else if (p->audio_off) {          /* iskljucen: samo prazni red */
                av_packet_free(&pkt);
                continue;
            } else {
                int rc = avcodec_send_packet(p->adec, pkt);
                av_packet_free(&pkt);
                if (rc < 0 && rc != AVERROR(EAGAIN))
                    continue;
            }
        }
        while (!SDL_AtomicGet(&p->stop) && !p->audio_off &&
               avcodec_receive_frame(p->adec, f) >= 0) {
            if (f->nb_samples > 0 && init_swr(p, f, &cur, &cur_fmt, &cur_rate) == 0) {
                int need = swr_get_out_samples(p->swr, f->nb_samples) + 64;
                if (need > cap) {
                    av_free(out);
                    out = av_malloc((size_t)need * 4);
                    cap = out ? need : 0;
                }
                if (out) {
                    uint8_t *dst[1] = { (uint8_t *)out };
                    int n = swr_convert(p->swr, dst, cap,
                                        (const uint8_t *const *)f->extended_data,
                                        f->nb_samples);
                    int64_t ts = f->pts != AV_NOPTS_VALUE ? f->pts
                                                          : f->best_effort_timestamp;
                    double pts;
                    SDL_LockMutex(p->ar_m);
                    pts = ts != AV_NOPTS_VALUE ? ts * av_q2d(p->atb) : p->ar_wr_pts;
                    SDL_UnlockMutex(p->ar_m);
                    if (n > 0)
                        audio_push(p, out, n, pts);
                }
            }
            av_frame_unref(f);
        }
        if (draining)
            break;
    }

done:
    av_free(out);
    av_channel_layout_uninit(&cur);
    av_frame_free(&f);
    SDL_LockMutex(p->ar_m);
    p->a_done = 1;
    SDL_CondBroadcast(p->ar_cv);
    SDL_UnlockMutex(p->ar_m);
    return 0;
}

/* Nit koja salje zvuk u sceAudioOut. sceAudioOutOutput blokira dok uredjaj
 * ne prihvati sljedece zrno, pa ovo istovremeno daje takt satu zvuka. */
static int aout_thread(void *ud)
{
    player_t *p = ud;
    int       grain = g_ao_grain, port = g_ao_port;
    int16_t  *buf = av_malloc((size_t)grain * 4);
    double    t_begin = now_s();

    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH);
    if (!buf) {
        p->audio_off = 1;
        snprintf(p->audio_note, sizeof p->audio_note, "out of memory");
        return 0;
    }

    while (!SDL_AtomicGet(&p->stop)) {
        SDL_LockMutex(p->ar_m);
        if (!p->a_started) {
            int video_ready = p->fq_count >= START_FRAMES || p->decode_done ||
                              now_s() - t_begin > 3.0;
            if (p->ar_fill >= A_START_FR && video_ready) {
                p->a_started = 1;
            } else if (p->a_done && p->ar_fill == 0) {
                p->audio_off = 1;               /* nema zvuka: zid-sat */
                snprintf(p->audio_note, sizeof p->audio_note,
                         "no decodable audio");
                SDL_UnlockMutex(p->ar_m);
                break;
            } else if (now_s() - t_begin > 8.0) {
                p->audio_off = 1;
                snprintf(p->audio_note, sizeof p->audio_note,
                         "audio did not start in time");
                SDL_UnlockMutex(p->ar_m);
                break;
            } else {
                SDL_UnlockMutex(p->ar_m);
                SDL_Delay(5);
                continue;
            }
        }

        if (SDL_AtomicGet(&p->a_pause)) {       /* pauza: tisina, sat stoji */
            SDL_UnlockMutex(p->ar_m);
            memset(buf, 0, (size_t)grain * 4);
            sceAudioOutOutput(port, buf);
            continue;
        }

        int n = p->ar_fill < grain ? p->ar_fill : grain;
        int first = A_RING_FR - p->ar_rd;
        if (first > n)
            first = n;
        memcpy(buf, p->ring + (size_t)p->ar_rd * 2, (size_t)first * 4);
        if (n > first)
            memcpy(buf + (size_t)first * 2, p->ring, (size_t)(n - first) * 4);
        if (n < grain) {
            memset(buf + (size_t)n * 2, 0, (size_t)(grain - n) * 4);
            if (!p->a_done)
                p->underruns++;
        }
        p->ar_rd = (p->ar_rd + n) % A_RING_FR;
        p->ar_fill -= n;
        p->ar_rd_pts += (double)grain / A_RATE;   /* sat ide u realnom vremenu */
        if (p->ar_fill == 0)
            p->ar_wr_pts = p->ar_rd_pts;
        SDL_CondBroadcast(p->ar_cv);
        SDL_UnlockMutex(p->ar_m);

        sceAudioOutOutput(port, buf);
    }
    av_free(buf);
    return 0;
}

/* Bira audio traku (prednost: farsi), otvara dekoder i prsten. Ako ne ide,
 * postavlja audio_off i razlog u audio_note; video tada ide sa zid-satom. */
static int is_persian(const AVStream *st)
{
    AVDictionaryEntry *e = av_dict_get(st->metadata, "language", NULL, 0);
    return e && (!strcasecmp(e->value, "per") || !strcasecmp(e->value, "fas") ||
                 !strcasecmp(e->value, "fa"));
}

static void audio_fail(player_t *p, const char *why)
{
    p->audio_off = 1;
    p->astream = -1;
    snprintf(p->audio_note, sizeof p->audio_note, "%s", why);
    LOG("audio off: %s", why);
}

static void open_audio(player_t *p)
{
    p->astream = -1;
    if (ao_open_once() < 0) {
        audio_fail(p, g_ao_err);
        return;
    }
    int best = -1;
    for (unsigned i = 0; i < p->fmt->nb_streams; i++) {
        const AVStream *st = p->fmt->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
            avcodec_find_decoder(st->codecpar->codec_id) && is_persian(st)) {
            best = (int)i;
            break;
        }
    }
    const AVCodec *codec = NULL;
    if (best < 0)
        best = av_find_best_stream(p->fmt, AVMEDIA_TYPE_AUDIO, -1, p->vstream,
                                   &codec, 0);
    if (best < 0) {
        audio_fail(p, "no audio track");
        return;
    }
    const AVStream *st = p->fmt->streams[best];
    codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) {
        audio_fail(p, "no decoder for the audio track");
        return;
    }
    p->adec = avcodec_alloc_context3(codec);
    if (!p->adec || avcodec_parameters_to_context(p->adec, st->codecpar) < 0 ||
        avcodec_open2(p->adec, codec, NULL) < 0) {
        avcodec_free_context(&p->adec);
        audio_fail(p, "cannot open the audio decoder");
        return;
    }
    p->ring = av_malloc((size_t)A_RING_FR * 4);
    if (!p->ring) {
        avcodec_free_context(&p->adec);
        audio_fail(p, "out of memory");
        return;
    }
    p->atb = st->time_base;
    p->astream = best;
    p->a_lat = 1.5 * g_ao_grain / A_RATE;
    snprintf(p->a_codec, sizeof p->a_codec, "%s", codec->name);
    LOG("player: audio track %d (%s), grain %d", best, codec->name, g_ao_grain);
}

/* ---- demux ---- */

static int demux_thread(void *ud)
{
    player_t *p = ud;
    int rc;

    p->fmt = avformat_alloc_context();
    if (!p->fmt) {
        set_error(p, "out of memory");
        return 0;
    }
    p->fmt->interrupt_callback.callback = interrupted;
    p->fmt->interrupt_callback.opaque = p;

    p->use_src = !strncasecmp(p->url, "http://", 7) ||
                 !strncasecmp(p->url, "https://", 8);
    if (p->use_src) {
        if (hs_open(&p->src, p->url, &p->stop) != 0) {
            set_error(p, "%s", p->src.err[0] ? p->src.err : "cannot connect");
            return 0;
        }
        uint8_t *iobuf = av_malloc(AVIO_BUF);
        p->avio = iobuf ? avio_alloc_context(iobuf, AVIO_BUF, 0, &p->src,
                                             hs_read, NULL, hs_seek) : NULL;
        if (!p->avio) {
            av_free(iobuf);
            set_error(p, "out of memory");
            return 0;
        }
        p->fmt->pb = p->avio;
        p->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    }

    rc = avformat_open_input(&p->fmt, p->url, NULL, NULL);
    if (rc < 0) {
        set_error_av(p, p->src.err[0] ? p->src.err : "cannot open", rc);
        return 0;
    }
    rc = avformat_find_stream_info(p->fmt, NULL);
    if (rc < 0) {
        set_error_av(p, "cannot read the stream info", rc);
        return 0;
    }
    const AVCodec *codec = NULL;
    p->vstream = av_find_best_stream(p->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (p->vstream < 0 || !codec) {
        set_error(p, "no video stream (or no decoder) in this file");
        return 0;
    }
    AVStream *st = p->fmt->streams[p->vstream];
    p->tb = st->time_base;
    if (p->fmt->duration > 0)
        p->duration = (double)p->fmt->duration / AV_TIME_BASE;
    snprintf(p->codec, sizeof p->codec, "%s", codec->name);
    if (st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0)
        p->src_fps = av_q2d(st->avg_frame_rate);

    p->dec = avcodec_alloc_context3(codec);
    if (!p->dec || avcodec_parameters_to_context(p->dec, st->codecpar) < 0) {
        set_error(p, "cannot set up the decoder");
        return 0;
    }
    p->dec->thread_count = PLAYER_THREADS;
    p->dec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    rc = avcodec_open2(p->dec, codec, NULL);
    if (rc < 0) {
        set_error_av(p, "cannot open the decoder", rc);
        return 0;
    }
    p->src_w = p->dec->width;
    p->src_h = p->dec->height;
    LOG("player: %s %dx%d, %.0f s", p->codec, p->src_w, p->src_h, p->duration);

    open_audio(p);

    p->th_dec = SDL_CreateThread(dec_thread, "dec", p);
    if (!p->th_dec) {
        set_error(p, "cannot start the decoder thread");
        return 0;
    }
    if (p->astream >= 0) {
        p->th_adec = SDL_CreateThread(adec_thread, "adec", p);
        p->th_aout = SDL_CreateThread(aout_thread, "aout", p);
        if (!p->th_adec || !p->th_aout)
            p->audio_off = 1;
    }
    SDL_AtomicSet(&p->state, ST_PLAYING);

    AVPacket *pkt = av_packet_alloc();
    while (pkt && !SDL_AtomicGet(&p->stop)) {
        rc = av_read_frame(p->fmt, pkt);
        if (rc < 0) {
            if (rc != AVERROR_EOF && !SDL_AtomicGet(&p->stop))
                fprintf(stderr, "[cine] player: read error %d\n", rc);
            break;
        }
        pqueue_t *q = NULL;
        if (pkt->stream_index == p->vstream)
            q = &p->vq;
        else if (p->astream >= 0 && !p->audio_off &&
                 pkt->stream_index == p->astream)
            q = &p->aq;
        if (!q) {
            av_packet_unref(pkt);
            continue;
        }
        if (pq_push(p, q, pkt) != 0) {
            av_packet_free(&pkt);
            pkt = NULL;
            break;
        }
        pkt = av_packet_alloc();
    }
    av_packet_free(&pkt);

    SDL_LockMutex(p->pq_m);
    p->demux_eof = 1;
    SDL_CondBroadcast(p->pq_cv);
    SDL_UnlockMutex(p->pq_m);
    return 0;
}

/* ---- API ---- */

player_t *player_open(SDL_Renderer *r, const char *url, const char *title,
                      int disp_w, int disp_h)
{
    static int inited;
    if (!inited) {
        av_log_set_level(AV_LOG_WARNING);
        av_log_set_callback(ff_log);
        inited = 1;
    }

    player_t *p = calloc(1, sizeof *p);
    if (!p)
        return NULL;
    p->r = r;
    p->disp_w = disp_w;
    p->disp_h = disp_h;
    p->overlay = 1;
    snprintf(p->url, sizeof p->url, "%s", url);
    snprintf(p->title, sizeof p->title, "%s", title ? title : "");
    p->astream = -1;
    p->pq_m = SDL_CreateMutex();
    p->pq_cv = SDL_CreateCond();
    p->fq_m = SDL_CreateMutex();
    p->fq_cv = SDL_CreateCond();
    p->ar_m = SDL_CreateMutex();
    p->ar_cv = SDL_CreateCond();
    int ok = p->pq_m && p->pq_cv && p->fq_m && p->fq_cv && p->ar_m && p->ar_cv &&
             pq_init(&p->vq, PQ_MAX, PQ_BYTES) == 0 &&
             pq_init(&p->aq, AQ_MAX, AQ_BYTES) == 0;
    SDL_AtomicSet(&p->state, ST_OPENING);
    if (ok)
        p->th_demux = SDL_CreateThread(demux_thread, "demux", p);
    if (!ok || !p->th_demux) {
        player_close(p);
        return NULL;
    }
    return p;
}

void player_close(player_t *p)
{
    if (!p)
        return;
    SDL_AtomicSet(&p->stop, 1);
    if (p->pq_cv)
        SDL_CondBroadcast(p->pq_cv);
    if (p->fq_cv)
        SDL_CondBroadcast(p->fq_cv);
    if (p->ar_cv)
        SDL_CondBroadcast(p->ar_cv);
    if (p->th_demux)
        SDL_WaitThread(p->th_demux, NULL);   /* tek tada su th_* stabilni */
    if (p->th_dec)
        SDL_WaitThread(p->th_dec, NULL);
    if (p->th_adec)
        SDL_WaitThread(p->th_adec, NULL);
    if (p->th_aout)
        SDL_WaitThread(p->th_aout, NULL);

    pq_free(&p->vq);
    pq_free(&p->aq);
    swr_free(&p->swr);
    avcodec_free_context(&p->adec);
    av_free(p->ring);
    for (int i = 0; i < FQ_N; i++)
        av_free(p->fq[i].px);
    if (p->tex)
        SDL_DestroyTexture(p->tex);
    avcodec_free_context(&p->dec);
    if (p->fmt)
        avformat_close_input(&p->fmt);
    if (p->avio) {
        av_freep(&p->avio->buffer);
        avio_context_free(&p->avio);
    }
    if (p->use_src)
        hs_close(&p->src);
    if (p->pq_m)
        SDL_DestroyMutex(p->pq_m);
    if (p->pq_cv)
        SDL_DestroyCond(p->pq_cv);
    if (p->fq_m)
        SDL_DestroyMutex(p->fq_m);
    if (p->fq_cv)
        SDL_DestroyCond(p->fq_cv);
    if (p->ar_m)
        SDL_DestroyMutex(p->ar_m);
    if (p->ar_cv)
        SDL_DestroyCond(p->ar_cv);
    free(p);
}

static void fq_pop(player_t *p)
{
    SDL_LockMutex(p->fq_m);
    p->fq_head = (p->fq_head + 1) % FQ_N;
    p->fq_count--;
    SDL_CondBroadcast(p->fq_cv);
    SDL_UnlockMutex(p->fq_m);
}

void player_update(player_t *p)
{
    if (!p || SDL_AtomicGet(&p->state) != ST_PLAYING)
        return;

    double now = now_s();

    SDL_LockMutex(p->fq_m);
    int w = p->out_w, h = p->out_h;
    SDL_UnlockMutex(p->fq_m);
    if (!p->tex && w > 0) {
        p->tex = SDL_CreateTexture(p->r, SDL_PIXELFORMAT_RGBA32,
                                   SDL_TEXTUREACCESS_STREAMING, w, h);
        if (!p->tex) {
            set_error(p, "cannot create the video texture");
            return;
        }
    }
    if (p->paused)
        return;

    for (;;) {
        SDL_LockMutex(p->fq_m);
        int      n = p->fq_count, done = p->decode_done;
        double   pts = n ? p->fq[p->fq_head].pts : 0;
        double   pts2 = n > 1 ? p->fq[(p->fq_head + 1) % FQ_N].pts : 0;
        uint8_t *px = n ? p->fq[p->fq_head].px : NULL;
        SDL_UnlockMutex(p->fq_m);

        if (n == 0) {
            if (done && SDL_AtomicGet(&p->state) == ST_PLAYING)
                SDL_AtomicSet(&p->state, ST_ENDED);
            break;
        }
        double t;
        if (p->astream >= 0 && !p->audio_off) {
            /* sat zvuka je glavni */
            SDL_LockMutex(p->ar_m);
            int started = p->a_started;
            t = p->ar_rd_pts - p->a_lat;
            SDL_UnlockMutex(p->ar_m);
            if (!started)
                break;                      /* zvuk jos nije krenuo */
        } else {
            if (!p->clock_started) {
                if (n < START_FRAMES && !done)
                    break;                  /* skupljamo zalihu slika */
                p->clock_base = now - pts;
                p->clock_started = 1;
            }
            t = now - p->clock_base;
            if (t - pts > 0.5) {            /* veliki zastoj: ne odbacuj sve */
                p->clock_base = now - pts;
                t = pts;
            }
        }
        if (pts > t + 0.002)
            break;                          /* jos nije vrijeme */
        if (p->astream >= 0 && !p->audio_off) {
            p->late_ema = p->late_ema * 0.9 + (t - pts) * 0.1;
            if (p->late_ema > 0.25)
                SDL_AtomicSet(&p->behind, 1);
            else if (p->late_ema < 0.06)
                SDL_AtomicSet(&p->behind, 0);
        }
        if (n >= 2 && pts2 <= t) {          /* kasni: preskoci */
            fq_pop(p);
            p->dropped++;
            continue;
        }
        if (p->tex)
            SDL_UpdateTexture(p->tex, NULL, px, w * 4);
        fq_pop(p);
        p->shown++;
        p->last_pts = pts;
        p->av_diff = p->av_diff * 0.9 + (pts - t) * 0.1;
        break;
    }

    if (now - p->fps_t0 >= 1.0) {
        p->fps = (p->shown - p->fps_n0) / (now - p->fps_t0);
        p->fps_t0 = now;
        p->fps_n0 = p->shown;
    }
}

static void fmt_time(char *dst, size_t cap, double s)
{
    int t = s < 0 ? 0 : (int)s;
    snprintf(dst, cap, "%d:%02d:%02d", t / 3600, (t / 60) % 60, t % 60);
}

void player_draw(player_t *p, ui_t *ui)
{
    SDL_SetRenderDrawColor(ui->r, 0, 0, 0, 255);
    SDL_RenderClear(ui->r);
    if (!p)
        return;

    int st = SDL_AtomicGet(&p->state);

    if (st == ST_ERROR) {
        ui_text(ui, 60, 60, 4, COL_ACCENT, "Cannot play this video");
        ui_text(ui, 60, 140, 2, COL_TEXT, "%s", p->err);
        ui_text(ui, 60, ui->screen_h - 44, 2, COL_DIM, "O: back");
        return;
    }
    if (st == ST_OPENING) {
        ui_text(ui, 60, 60, 4, COL_ACCENT, "Opening...");
        ui_text(ui, 60, 140, 2, COL_DIM, "%s", p->title);
        ui_text(ui, 60, ui->screen_h - 44, 2, COL_DIM, "O: cancel");
        return;
    }

    if (p->tex && p->out_w > 0) {
        SDL_Rect dst = { (ui->screen_w - p->out_w) / 2,
                         (ui->screen_h - p->out_h) / 2, p->out_w, p->out_h };
        SDL_RenderCopy(ui->r, p->tex, NULL, &dst);
    }
    if (st == ST_ENDED)
        ui_text(ui, 60, ui->screen_h / 2, 4, COL_ACCENT, "The end - O: back");

    if (p->overlay) {
        char a[24], b[24];
        fmt_time(a, sizeof a, p->last_pts);
        fmt_time(b, sizeof b, p->duration);
        SDL_LockMutex(p->fq_m);
        int qf = p->fq_count;
        SDL_UnlockMutex(p->fq_m);

        ui_fill_rect(ui, 0, 0, ui->screen_w, 190, COL_HUD);
        ui_text(ui, 30, 14, 2, COL_TEXT,
                "%s %dx%d @ %.2f   %s / %s%s", p->codec, p->src_w, p->src_h,
                p->src_fps, a, b, p->paused ? "   [PAUSED]" : "");
        ui_text(ui, 30, 50, 2, COL_ACCENT,
                "shown %.1f fps   decode %.1f ms   convert %.1f ms%s",
                p->fps, p->dec_ms, p->conv_ms,
                SDL_AtomicGet(&p->behind) ? "   [CATCHING UP: skipping B-frames]" : "");
        ui_text(ui, 30, 86, 2, COL_DIM,
                "dropped %d   frames queued %d/%d   packets %d   threads %d",
                p->dropped, qf, FQ_N, p->vq.count, PLAYER_THREADS);
        if (p->astream >= 0 && !p->audio_off) {
            SDL_LockMutex(p->ar_m);
            int ring_ms = (int)(1000.0 * p->ar_fill / A_RATE);
            SDL_UnlockMutex(p->ar_m);
            ui_text(ui, 30, 118, 2, COL_ACCENT,
                    "audio %s %s   a/v %+d ms   underruns %d   buffer %d ms",
                    p->a_codec, p->a_info, (int)(p->av_diff * 1000.0),
                    p->underruns, ring_ms);
        } else {
            ui_text(ui, 30, 118, 2, COL_DIM, "audio: off (%s)",
                    p->audio_note[0] ? p->audio_note : "starting");
        }
        ui_text(ui, 30, 154, 2, COL_DIM,
                "X: pause   Triangle: hide this   O: stop");
    }
}

int player_button(player_t *p, int button)
{
    if (!p)
        return 1;
    switch (button) {
    case SDL_CONTROLLER_BUTTON_B:
        return 1;
    case SDL_CONTROLLER_BUTTON_A:
        if (SDL_AtomicGet(&p->state) == ST_PLAYING) {
            double now = now_s();
            p->paused = !p->paused;
            SDL_AtomicSet(&p->a_pause, p->paused);
            if (p->paused)
                p->pause_began = now;
            else
                p->clock_base += now - p->pause_began;
        }
        break;
    case SDL_CONTROLLER_BUTTON_Y:
        p->overlay = !p->overlay;
        break;
    default:
        break;
    }
    return 0;
}

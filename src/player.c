/* player.c - ugradjeni plejer, faza 1 (vidi player.h). */
#include <errno.h>
#include <stdarg.h>
#include <curl/curl.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
#include <libswscale/swscale.h>

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

enum { ST_OPENING = 0, ST_PLAYING, ST_ENDED, ST_ERROR };

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

    SDL_mutex *pq_m;
    SDL_cond  *pq_cv;
    AVPacket  *pq[PQ_MAX];
    int        pq_head, pq_count;
    int64_t    pq_bytes;
    int        demux_eof;

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

/* ---- red paketa ---- */

static int pq_push(player_t *p, AVPacket *pkt)
{
    SDL_LockMutex(p->pq_m);
    while ((p->pq_count >= PQ_MAX || p->pq_bytes > PQ_BYTES) &&
           !SDL_AtomicGet(&p->stop))
        SDL_CondWaitTimeout(p->pq_cv, p->pq_m, 50);
    if (SDL_AtomicGet(&p->stop)) {
        SDL_UnlockMutex(p->pq_m);
        return -1;
    }
    p->pq[(p->pq_head + p->pq_count) % PQ_MAX] = pkt;
    p->pq_count++;
    p->pq_bytes += pkt->size;
    SDL_CondBroadcast(p->pq_cv);
    SDL_UnlockMutex(p->pq_m);
    return 0;
}

/* NULL = kraj toka (ili zaustavljanje). */
static AVPacket *pq_pop(player_t *p)
{
    AVPacket *pkt = NULL;

    SDL_LockMutex(p->pq_m);
    while (p->pq_count == 0 && !p->demux_eof && !SDL_AtomicGet(&p->stop))
        SDL_CondWaitTimeout(p->pq_cv, p->pq_m, 50);
    if (p->pq_count > 0 && !SDL_AtomicGet(&p->stop)) {
        pkt = p->pq[p->pq_head];
        p->pq_head = (p->pq_head + 1) % PQ_MAX;
        p->pq_count--;
        p->pq_bytes -= pkt->size;
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
            AVPacket *pkt = pq_pop(p);
            t0 = now_s();                  /* ne racunaj cekanje na mrezu */
            if (!pkt) {
                if (SDL_AtomicGet(&p->stop))
                    break;
                draining = 1;
                avcodec_send_packet(p->dec, NULL);
            } else {
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

    p->th_dec = SDL_CreateThread(dec_thread, "dec", p);
    if (!p->th_dec) {
        set_error(p, "cannot start the decoder thread");
        return 0;
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
        if (pkt->stream_index != p->vstream) {
            av_packet_unref(pkt);
            continue;
        }
        if (pq_push(p, pkt) != 0) {
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
    p->pq_m = SDL_CreateMutex();
    p->pq_cv = SDL_CreateCond();
    p->fq_m = SDL_CreateMutex();
    p->fq_cv = SDL_CreateCond();
    SDL_AtomicSet(&p->state, ST_OPENING);
    p->th_demux = SDL_CreateThread(demux_thread, "demux", p);
    if (!p->pq_m || !p->pq_cv || !p->fq_m || !p->fq_cv || !p->th_demux) {
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
    if (p->th_demux)
        SDL_WaitThread(p->th_demux, NULL);   /* tek tada je th_dec stabilan */
    if (p->th_dec)
        SDL_WaitThread(p->th_dec, NULL);

    while (p->pq_count > 0) {
        AVPacket *k = p->pq[p->pq_head];
        p->pq_head = (p->pq_head + 1) % PQ_MAX;
        p->pq_count--;
        av_packet_free(&k);
    }
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
        if (!p->clock_started) {
            if (n < START_FRAMES && !done)
                break;                      /* skupljamo zalihu slika */
            p->clock_base = now - pts;
            p->clock_started = 1;
        }
        double t = now - p->clock_base;
        if (t - pts > 0.5) {                /* veliki zastoj: ne odbacuj sve */
            p->clock_base = now - pts;
            t = pts;
        }
        if (pts > t + 0.002)
            break;                          /* jos nije vrijeme */
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

        ui_fill_rect(ui, 0, 0, ui->screen_w, 150, COL_HUD);
        ui_text(ui, 30, 14, 2, COL_TEXT,
                "%s %dx%d @ %.2f   %s / %s%s", p->codec, p->src_w, p->src_h,
                p->src_fps, a, b, p->paused ? "   [PAUSED]" : "");
        ui_text(ui, 30, 50, 2, COL_ACCENT,
                "shown %.1f fps   decode %.1f ms   convert %.1f ms",
                p->fps, p->dec_ms, p->conv_ms);
        ui_text(ui, 30, 86, 2, COL_DIM,
                "dropped %d   frames queued %d/%d   packets %d   threads %d",
                p->dropped, qf, FQ_N, p->pq_count, PLAYER_THREADS);
        ui_text(ui, 30, 118, 2, COL_DIM,
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

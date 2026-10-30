/* common.c */
#include "common.h"
#include <ctype.h>

int natural_cmp(const char *a, const char *b)
{
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            /* Preskoci vodece nule na obe strane. */
            while (*a == '0') a++;
            while (*b == '0') b++;

            /* Duzi niz cifara = veci broj. */
            const char *sa = a, *sb = b;
            while (isdigit((unsigned char)*a)) a++;
            while (isdigit((unsigned char)*b)) b++;

            long la = a - sa, lb = b - sb;
            if (la != lb)
                return la < lb ? -1 : 1;

            int c = strncmp(sa, sb, (size_t)la);
            if (c)
                return c;
        } else {
            int ca = tolower((unsigned char)*a);
            int cb = tolower((unsigned char)*b);
            if (ca != cb)
                return ca < cb ? -1 : 1;
            a++;
            b++;
        }
    }
    if (*a)
        return 1;
    if (*b)
        return -1;
    return 0;
}

const char *path_ext(const char *path)
{
    const char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '/');
    if (!dot || (slash && dot < slash))
        return "";
    return dot + 1;
}

const char *path_base(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

int is_url(const char *path)
{
    return path && !strncasecmp(path, "http://", 7);
}

void url_decode(char *dst, size_t dstlen, const char *src)
{
    size_t o = 0;

    if (!dstlen)
        return;

    for (const char *p = src; *p && o + 1 < dstlen; p++) {
        if (*p == '%' && isxdigit((unsigned char)p[1]) &&
                         isxdigit((unsigned char)p[2])) {
            char hex[3] = { p[1], p[2], '\0' };
            dst[o++] = (char)strtol(hex, NULL, 16);
            p += 2;
        } else {
            dst[o++] = *p;
        }
    }
    dst[o] = '\0';
}

#include "core/url_codec.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *url_encode(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char *out = malloc(len * 3 + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            out[o++] = (char)c;
        } else {
            o += (size_t)snprintf(out + o, 4, "%%%02X", c);
        }
    }
    out[o] = '\0';
    return out;
}

void url_decode_into(char *dst, size_t dstcap, const char *src) {
    if (!dst || dstcap == 0) return;
    if (!src) src = "";
    size_t j = 0;
    for (size_t i = 0; src[i] && j < dstcap - 1; i++) {
        if (src[i] == '%' && isxdigit((unsigned char)src[i + 1]) &&
            isxdigit((unsigned char)src[i + 2])) {
            char hex[3] = { src[i + 1], src[i + 2], 0 };
            dst[j++] = (char)strtol(hex, NULL, 16);
            i += 2;
            continue;
        }
        if (src[i] == '+') { dst[j++] = ' '; continue; }
        dst[j++] = src[i];
    }
    dst[j] = '\0';
}

char *url_decode(const char *src) {
    if (!src) return NULL;
    size_t len = strlen(src);
    char *dst = malloc(len + 1);
    if (!dst) return NULL;
    url_decode_into(dst, len + 1, src);
    return dst;
}

char *url_query_param(const char *query, const char *name) {
    if (!query || !name || !*name) return NULL;
    char pat[128];
    snprintf(pat, sizeof(pat), "%s=", name);
    size_t plen = strlen(pat);

    const char *p = query;
    for (;;) {
        p = strstr(p, pat);
        if (!p) return NULL;
        if (p == query || p[-1] == '&' || p[-1] == '?') break;
        p += plen;
    }
    p += plen;

    const char *end = strchr(p, '&');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    char *raw = malloc(len + 1);
    if (!raw) return NULL;
    memcpy(raw, p, len);
    raw[len] = '\0';
    char *dec = url_decode(raw);
    free(raw);
    return dec;
}

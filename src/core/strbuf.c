#include "core/strbuf.h"
#include "core/json_text.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sbuf_init(StrBuf *b) {
    if (!b) return;
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    b->failed = 0;
}

void sbuf_free(StrBuf *b) {
    if (!b) return;
    free(b->data);
    sbuf_init(b);
}

static int sb_reserve(StrBuf *b, size_t extra) {
    if (b->failed) return -1;
    size_t need = b->len + extra + 1;
    if (need <= b->cap) return 0;
    size_t n = b->cap ? b->cap : 256;
    while (n < need) n *= 2;
    char *tmp = realloc(b->data, n);
    if (!tmp) { b->failed = 1; return -1; }
    b->data = tmp;
    b->cap = n;
    return 0;
}

int sbuf_append(StrBuf *b, const char *s) {
    if (!b || !s) return -1;
    return sbuf_append_len(b, s, strlen(s));
}

int sbuf_append_len(StrBuf *b, const char *s, size_t len) {
    if (!b || !s) return -1;
    if (sb_reserve(b, len) != 0) return -1;
    memcpy(b->data + b->len, s, len);
    b->len += len;
    b->data[b->len] = '\0';
    return 0;
}

int sbuf_appendf(StrBuf *b, const char *fmt, ...) {
    if (!b || !fmt) return -1;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { b->failed = 1; return -1; }

    if (sb_reserve(b, (size_t)n) != 0) return -1;
    va_start(ap, fmt);
    int written = vsnprintf(b->data + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    if (written < 0 || (size_t)written >= b->cap - b->len) {
        b->failed = 1;
        return -1;
    }
    b->len += (size_t)written;
    return 0;
}

int sbuf_append_json_string(StrBuf *b, const char *s) {
    if (!b) return -1;
    if (sb_reserve(b, 2) != 0) return -1;
    b->data[b->len++] = '"';
    b->data[b->len] = '\0';

    size_t need = strlen(s ? s : "") * 6 + 2;
    if (sb_reserve(b, need) != 0) return -1;
    if (json_escape_into(b->data + b->len, b->cap - b->len, s) != 0) {
        b->failed = 1;
        return -1;
    }
    b->len += strlen(b->data + b->len);
    if (sb_reserve(b, 1) != 0) return -1;
    b->data[b->len++] = '"';
    b->data[b->len] = '\0';
    return 0;
}

char *sbuf_take(StrBuf *b) {
    if (!b) return NULL;
    if (b->failed) {
        sbuf_free(b);
        return NULL;
    }
    if (!b->data) {
        b->data = calloc(1, 1);
        if (!b->data) return NULL;
    }
    char *out = b->data;
    sbuf_init(b);
    return out;
}

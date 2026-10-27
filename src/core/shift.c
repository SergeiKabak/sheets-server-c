#include "core/shift.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* Приводит разделитель к '-': тире в UTF-8 (U+2013, U+2014) -> один '-'. */
static void normalize_dashes(const char *s, char *out, size_t cap) {
    size_t j = 0;
    if (!s) { if (cap) out[0] = '\0'; return; }
    for (size_t i = 0; s[i] && j + 1 < cap; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch == ' ') continue;
        if (ch >= 0xC0) {
            /* начало UTF-8 последовательности (тире) -> один '-' */
            out[j++] = '-';
            while (s[i + 1] && ((unsigned char)s[i + 1] & 0xC0) == 0x80) i++;
            continue;
        }
        if (ch >= 0x80) continue;
        out[j++] = (char)ch;
    }
    out[j] = '\0';
}

int shift_parse(const char *s, int *start, int *end) {
    if (!s || !*s) return -1;
    char tmp[64];
    normalize_dashes(s, tmp, sizeof(tmp));

    int h1 = 0, m1 = 0, h2 = 0, m2 = 0;
    if (sscanf(tmp, "%d:%d-%d:%d", &h1, &m1, &h2, &m2) != 4) return -1;
    if (h1 < 0 || h1 > 47 || m1 < 0 || m1 > 59) return -1;
    if (h2 < 0 || h2 > 47 || m2 < 0 || m2 > 59) return -1;

    int a = h1 * 60 + m1;
    int b = h2 * 60 + m2;
    if (b < a) { int t = a; a = b; b = t; }
    if (start) *start = a;
    if (end) *end = b;
    return 0;
}

int shift_overlap(const char *a, const char *b) {
    int s1, e1, s2, e2;
    if (shift_parse(a, &s1, &e1) != 0 || shift_parse(b, &s2, &e2) != 0) {
        if (!a || !b) return 0;
        return strcmp(a, b) == 0;
    }
    return s1 < e2 && s2 < e1;
}

int shift_start_minutes(const char *s) {
    if (!s) return SHIFT_START_UNKNOWN;
    while (*s == ' ') s++;
    int h = 0, m = 0;
    if (sscanf(s, "%d:%d", &h, &m) == 2 && h >= 0 && h < 48 && m >= 0 && m < 60)
        return h * 60 + m;
    return SHIFT_START_UNKNOWN;
}

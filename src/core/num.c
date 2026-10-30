#include "core/num.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

double num_parse(const char *s) {
    if (!s) return 0.0;
    char tmp[64];
    size_t n = strlen(s);
    if (n >= sizeof(tmp)) n = sizeof(tmp) - 1;
    memcpy(tmp, s, n);
    tmp[n] = '\0';
    for (size_t i = 0; i < n; i++)
        if (tmp[i] == ',') tmp[i] = '.';
    return atof(tmp);
}

int num_is_valid(const char *s) {
    if (!s || !*s) return 0;
    for (const char *p = s; *p; p++)
        if (!isdigit((unsigned char)*p) && *p != '.' && *p != '-') return 0;
    return 1;
}

void num_format2(double value, char *out, size_t cap) {
    if (!out || cap == 0) return;
    snprintf(out, cap, "%.2f", value);
}

int num_round2_text(const char *s, char *out, size_t cap) {
    if (!s || !*s || !out || cap < 5) return -1;

    /* Знак */
    int neg = 0;
    if (*s == '-' || *s == '+') { neg = (*s == '-'); s++; }
    if (!*s) return -1;

    /* Целая и дробная части; запятая = точка */
    char ipart[32] = "";
    char fpart[16] = "";
    size_t il = 0, fl = 0;
    int dot = 0;
    for (const char *p = s; *p; p++) {
        char c = *p;
        if (c == ',' || c == '.') {
            if (dot++) return -1;
        } else if (c >= '0' && c <= '9') {
            if (!dot) {
                if (il + 1 >= sizeof(ipart)) return -1;
                ipart[il++] = c;
            } else {
                if (fl + 1 >= sizeof(fpart)) return -1;
                fpart[fl++] = c;
            }
        } else {
            return -1;
        }
    }
    if (il == 0 && fl == 0) return -1;
    if (il > 12) return -1;              /* защита от переполнения atol */
    ipart[il] = '\0';
    fpart[fl] = '\0';

    /* Первые два знака дроби + третий как признак округления вверх */
    char f2[3] = { '0', '0', '\0' };
    if (fl >= 1) f2[0] = fpart[0];
    if (fl >= 2) f2[1] = fpart[1];
    int up = (fl >= 3 && fpart[2] >= '5');

    long ip = il ? atol(ipart) : 0;
    long fp = atol(f2);
    if (up && ++fp >= 100) { fp -= 100; ip++; }

    if (neg && (ip || fp)) {
        if ((size_t)snprintf(out, cap, "-%ld.%02ld", ip, fp) >= cap) return -1;
    } else {
        if ((size_t)snprintf(out, cap, "%ld.%02ld", ip, fp) >= cap) return -1;
    }
    return 0;
}

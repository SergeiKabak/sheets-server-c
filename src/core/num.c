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

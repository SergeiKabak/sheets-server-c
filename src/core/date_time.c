#include "core/date_time.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *const MONTH_NAMES[12] = {
    "Январь", "Февраль", "Март", "Апрель", "Май", "Июнь",
    "Июль", "Август", "Сентябрь", "Октябрь", "Ноябрь", "Декабрь"
};

const char *month_name(int mon) {
    if (mon < 1 || mon > 12) return NULL;
    return MONTH_NAMES[mon - 1];
}

void month_label(const char *numeric, char *out, size_t cap) {
    int mon = 0, year = 0;
    if (numeric && sscanf(numeric, "%d.%d", &mon, &year) == 2 &&
        mon >= 1 && mon <= 12) {
        snprintf(out, cap, "%s %d", MONTH_NAMES[mon - 1], year);
    } else {
        snprintf(out, cap, "%s", numeric ? numeric : "");
    }
}

int month_numeric_from_label(const char *month, int default_year, char *out, size_t cap) {
    if (!out || cap == 0) return -1;
    out[0] = '\0';
    if (!month) return -1;

    for (int i = 0; i < 12; i++) {
        const char *name = MONTH_NAMES[i];
        size_t len = strlen(name);
        if (strncmp(month, name, len) != 0) continue;

        const char *rest = month + len;
        while (*rest == ' ') rest++;
        int year = 0;
        if (sscanf(rest, "%d", &year) != 1) year = default_year;
        snprintf(out, cap, "%02d.%04d", i + 1, year);
        return 0;
    }
    /* уже в формате цифр ("10.2026") */
    snprintf(out, cap, "%s", month);
    return 0;
}

int month_parse(const char *numeric, int *mon, int *year) {
    int m = 0, y = 0;
    if (!numeric || sscanf(numeric, "%d.%d", &m, &y) != 2) return -1;
    if (m < 1 || m > 12 || y < 2000 || y > 2100) return -1;
    if (mon) *mon = m;
    if (year) *year = y;
    return 0;
}

int days_in_month(int mon, int year) {
    static const int dm[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (mon < 1 || mon > 12) return 0;
    if (mon == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0))
        return 29;
    return dm[mon - 1];
}

void date_normalize(const char *in, char *out, size_t cap) {
    int d = 0, m = 0, y = 0;
    if (sscanf(in ? in : "", "%d.%d.%d", &d, &m, &y) == 3) {
        snprintf(out, cap, "%02d.%02d.%04d", d, m, y);
    } else if (sscanf(in ? in : "", "%d/%d/%d", &d, &m, &y) == 3) {
        snprintf(out, cap, "%02d.%02d.%04d", d, m, y);
    } else if (sscanf(in ? in : "", "%d-%d-%d", &y, &m, &d) == 3) {
        snprintf(out, cap, "%02d.%02d.%04d", d, m, y);
    } else {
        snprintf(out, cap, "%s", in ? in : "");
    }
}

int date_parse(const char *s, int *day, int *month, int *year) {
    int d = 0, m = 0, y = 0;
    if (!s || sscanf(s, "%d.%d.%d", &d, &m, &y) != 3) return -1;
    if (day) *day = d;
    if (month) *month = m;
    if (year) *year = y;
    return 0;
}

int date_compare(const char *a, const char *b) {
    int d1 = 0, m1 = 0, y1 = 0, d2 = 0, m2 = 0, y2 = 0;
    sscanf(a ? a : "", "%d.%d.%d", &d1, &m1, &y1);
    sscanf(b ? b : "", "%d.%d.%d", &d2, &m2, &y2);
    if (y1 != y2) return y1 - y2;
    if (m1 != m2) return m1 - m2;
    return d1 - d2;
}

void time_normalize(const char *in, char *out, size_t cap) {
    int h = 0, m = 0;
    if (sscanf(in ? in : "", "%d:%d", &h, &m) == 2) {
        snprintf(out, cap, "%02d:%02d", h, m);
    } else {
        snprintf(out, cap, "%s", in ? in : "");
    }
}

int time_parse(const char *s) {
    if (!s) return -1;
    int h = 0, m = 0;
    if (sscanf(s, "%d:%d", &h, &m) != 2) return -1;
    if (h < 0 || h > 23 || m < 0 || m > 59) return -1;
    return h * 60 + m;
}

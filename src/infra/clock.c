#include "infra/clock.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

void clock_datetime(int tz_offset, char *date, size_t dcap,
                    char *time_s, size_t tcap) {
    time_t now = time(NULL) + tz_offset;
    struct tm tm;
    gmtime_r(&now, &tm);
    if (date && dcap)
        snprintf(date, dcap, "%02d.%02d.%04d", tm.tm_mday, tm.tm_mon + 1,
                 tm.tm_year + 1900);
    if (time_s && tcap)
        snprintf(time_s, tcap, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

void clock_month_tab(int tz_offset, char *out, size_t cap) {
    time_t now = time(NULL) + tz_offset;
    struct tm tm;
    gmtime_r(&now, &tm);
    snprintf(out, cap, "%02d.%04d", tm.tm_mon + 1, tm.tm_year + 1900);
}

int clock_current_year(int tz_offset) {
    time_t now = time(NULL) + tz_offset;
    struct tm tm;
    gmtime_r(&now, &tm);
    return tm.tm_year + 1900;
}

long clock_monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)(ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
}

#include "app/tracking_cache.h"

#include "infra/file_store.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* Имя месяца вида "10.2026" — только цифры и точки (иначе — путь в /tmp). */
static int tab_name_valid(const char *tab) {
    if (!tab || !*tab || strlen(tab) > 16) return 0;
    for (const unsigned char *p = (const unsigned char *)tab; *p; p++)
        if (!isdigit(*p) && *p != '.') return 0;
    return 1;
}

static int trk_cache_path(const char *tab, char *out, size_t cap) {
    if (!tab_name_valid(tab)) return -1;
    snprintf(out, cap, "/tmp/trk_days_%s.json", tab);
    return 0;
}

int trk_cache_read(const char *tab, char *out, size_t cap, int max_age) {
    char path[256];
    if (trk_cache_path(tab, path, sizeof(path)) != 0) return -1;
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    if (max_age >= 0 && time(NULL) - st.st_mtime > max_age) return -1;
    if (st.st_size <= 0 || (size_t)st.st_size >= cap) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(out, 1, (size_t)st.st_size, f);
    fclose(f);
    if (n != (size_t)st.st_size) return -1;
    out[n] = '\0';
    return 0;
}

int trk_cache_write(const char *tab, const char *json) {
    char path[256];
    if (trk_cache_path(tab, path, sizeof(path)) != 0) return -1;
    if (!json) return -1;
    return file_write_atomic(path, json, strlen(json));
}

void trk_cache_invalidate(const char *tab) {
    char path[256];
    if (trk_cache_path(tab, path, sizeof(path)) == 0) file_remove(path);
}

void trk_dirty_put(const char *tab) {
    if (!tab_name_valid(tab)) return;
    char path[64];
    snprintf(path, sizeof(path), "/tmp/gdirty_%s", tab);
    FILE *f = fopen(path, "w");
    if (f) { fputs("1", f); fclose(f); }
}

void trk_dirty_clear(const char *tab) {
    if (!tab_name_valid(tab)) return;
    char path[64];
    snprintf(path, sizeof(path), "/tmp/gdirty_%s", tab);
    file_remove(path);
}

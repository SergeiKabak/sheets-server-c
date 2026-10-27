#include "app/month_cache.h"

#include "vendor/cJSON.h"
#include "core/json_text.h"
#include "infra/file_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* ---------- месяцы-листы (/tmp/months.dat) ---------- */

/* Формат: [int count] затем для каждого месяца
 *          [string title] [row1: int len + строки] [row2: int len + строки] */
static void write_string(FILE *fp, const char *s) {
    int len = s ? (int)strlen(s) : 0;
    fwrite(&len, sizeof(int), 1, fp);
    if (len) fwrite(s, 1, (size_t)len, fp);
}

static void write_row(FILE *fp, char **row, int len) {
    fwrite(&len, sizeof(int), 1, fp);
    for (int i = 0; i < len; i++) write_string(fp, row ? row[i] : NULL);
}

static int read_string(FILE *fp, char **out) {
    int len;
    if (fread(&len, sizeof(int), 1, fp) != 1) return -1;
    if (len < 0) return -1;
    char *s = malloc((size_t)len + 1);
    if (!s) return -1;
    if (len && fread(s, 1, (size_t)len, fp) != (size_t)len) { free(s); return -1; }
    s[len] = '\0';
    *out = s;
    return 0;
}

static int read_row(FILE *fp, char ***out) {
    int len;
    if (fread(&len, sizeof(int), 1, fp) != 1) return -1;
    if (len < 0) return -1;
    char **row = malloc(sizeof(char *) * (len > 0 ? (size_t)len : 1));
    if (!row) return -1;
    for (int i = 0; i < len; i++) {
        char *s = NULL;
        if (read_string(fp, &s) < 0) {
            for (int k = 0; k < i; k++) free(row[k]);
            free(row);
            return -1;
        }
        row[i] = s;
    }
    *out = row;
    return len;
}

int month_cache_save(const MonthSchedule *months, int count) {
    FILE *fp = fopen(MONTHS_FILE, "wb");
    if (!fp) return -1;
    fwrite(&count, sizeof(int), 1, fp);
    for (int i = 0; i < count; i++) {
        write_string(fp, months[i].title);
        write_row(fp, months[i].shift1, months[i].shift1_len);
        write_row(fp, months[i].shift2, months[i].shift2_len);
    }
    fclose(fp);
    return 0;
}

int month_cache_load(MonthSchedule *months, int max) {
    FILE *fp = fopen(MONTHS_FILE, "rb");
    if (!fp) return 0;
    int count;
    if (fread(&count, sizeof(int), 1, fp) != 1) { fclose(fp); return 0; }
    if (count > max) count = max;
    int got = 0;
    for (int i = 0; i < count; i++) {
        MonthSchedule *m = &months[i];
        memset(m, 0, sizeof(*m));
        char *title = NULL;
        if (read_string(fp, &title) < 0) break;
        snprintf(m->title, sizeof(m->title), "%.*s", SCHEDULE_MAX_NAME - 1,
                 title ? title : "");
        free(title);
        int l1 = read_row(fp, &m->shift1);
        if (l1 < 0) break;
        m->shift1_len = l1;
        int l2 = read_row(fp, &m->shift2);
        if (l2 < 0) break;
        m->shift2_len = l2;
        got++;
    }
    fclose(fp);
    return got;
}

const char *cabinet_index_path(void) {
    return CABINET_INDEX_FILE;
}

/* ---------- кэш готового ответа месяца ---------- */

/* Hex первых 48 байт заголовка листа — безопасное имя файла без коллизий */
static int month_hex(const char *title, char *out, size_t cap) {
    if (!title || !*title || cap < 97) return -1;
    static const char *H = "0123456789abcdef";
    int o = 0;
    for (const unsigned char *p = (const unsigned char *)title; *p && o < 96; p++) {
        out[o++] = H[*p >> 4];
        out[o++] = H[*p & 0x0F];
    }
    out[o] = '\0';
    return 0;
}

static int month_resp_path(const char *title, char *out, size_t cap) {
    char hex[112];
    if (month_hex(title, hex, sizeof(hex)) != 0) return -1;
    int n = snprintf(out, cap, "/tmp/mresp_%s.json", hex);
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

int month_response_read(const char *title, char *out, size_t cap) {
    char path[256];
    if (month_resp_path(title, path, sizeof(path)) != 0) return -1;
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    if (time(NULL) - st.st_mtime > MONTH_RESP_TTL) return -1;
    if (st.st_size < 16 || (size_t)st.st_size >= cap) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t got = fread(out, 1, (size_t)st.st_size, f);
    fclose(f);
    out[got] = '\0';
    if (got < 16 || out[got - 1] != '}') return -1;
    return 0;
}

int month_response_write(const char *title, const char *s1, const char *s2) {
    char path[256];
    if (month_resp_path(title, path, sizeof(path)) != 0) return -1;
    char *et = json_escape(title);
    size_t cap = strlen(s1 ? s1 : "") + strlen(s2 ? s2 : "") + strlen(title) + 32;
    char *body = malloc(cap);
    if (!body) { free(et); return -1; }
    snprintf(body, cap, "{\"t\":\"%s\",\"s1\":%s,\"s2\":%s}",
             et ? et : "", s1 ? s1 : "[]", s2 ? s2 : "[]");
    free(et);
    int rc = file_write_atomic(path, body, strlen(body));
    free(body);
    return rc;
}

/* ---------- кэш строк месяца (для коллег) ---------- */

static int month_col_path(const char *title, char *out, size_t cap) {
    char hex[112];
    if (month_hex(title, hex, sizeof(hex)) != 0) return -1;
    int n = snprintf(out, cap, "/tmp/mcol_%s.json", hex);
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

int month_colleagues_cache_read(const char *title, char *out, size_t cap) {
    char path[192];
    if (month_col_path(title, path, sizeof(path)) != 0) return -1;
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    if (time(NULL) - st.st_mtime > MONTH_RESP_TTL) return -1;
    if (st.st_size < 2 || (size_t)st.st_size >= cap) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t got = fread(out, 1, (size_t)st.st_size, f);
    fclose(f);
    out[got] = '\0';
    if (got < 2 || out[0] != '[' || out[got - 1] != ']') return -1;
    return 0;
}

int month_colleagues_cache_write(const char *title, const char *json) {
    char path[192];
    if (month_col_path(title, path, sizeof(path)) != 0) return -1;
    return file_write_atomic(path, json ? json : "[]", strlen(json ? json : "[]"));
}

int month_colleagues_cache_filter(const char *all, int day, const char *cabinet,
                                  char *out, size_t outcap) {
    if (!all || !cabinet || !out || outcap < 4) return -1;
    out[0] = '\0';
    cJSON *arr = cJSON_Parse(all);
    if (!arr || !cJSON_IsArray(arr)) { cJSON_Delete(arr); return -1; }
    int off = snprintf(out, outcap, "[");
    int n = 0;
    cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr) {
        cJSON *jd = cJSON_GetObjectItemCaseSensitive(it, "day");
        cJSON *jc = cJSON_GetObjectItemCaseSensitive(it, "cabinet");
        if (!cJSON_IsNumber(jd) || !cJSON_IsString(jc) || !jc->valuestring) continue;
        if ((int)jd->valuedouble != day || strcmp(jc->valuestring, cabinet) != 0) continue;
        char *printed = cJSON_PrintUnformatted(it);
        if (!printed) continue;
        int w = snprintf(out + off, outcap - (size_t)off, "%s%s", n ? "," : "", printed);
        free(printed);
        if (w < 0 || (size_t)w >= outcap - (size_t)off) {
            cJSON_Delete(arr);
            out[0] = '\0';
            return -1;
        }
        off += w;
        n++;
    }
    cJSON_Delete(arr);
    if ((size_t)off + 2 >= outcap) { out[0] = '\0'; return -1; }
    snprintf(out + off, outcap - off, "]");
    return 0;
}

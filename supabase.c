/* supabase.c — буфер графика в Supabase через PostgREST (этап 1 плана).
 *
 * Цикл (раз в 60 с, из background_updater):
 *   1. sb_begin_cycle()           — фиксируем updated_at цикла
 *   2. sb_row(...) во время обхода листов Google (см. build_months)
 *   3. sb_flush(months, n, &err):
 *        POST   /rest/v1/schedule              — upsert (merge-duplicates)
 *        DELETE /rest/v1/schedule?month=eq.X&updated_at=lt.<cutoff>
 *               — уборка строк, которых больше нет в Google
 *        DELETE /rest/v1/schedule?month=eq.X   — месяцы, пропавшие из Google
 *   4. sb_log(...)                — журнал sync_log (пульс каждые 10 циклов + ошибки)
 *
 * Дубликаты PK внутри одного батча отсекаются: PostgREST отдаёт ошибку
 * «ON CONFLICT DO UPDATE cannot affect row a second time».
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "http.h"
#include "supabase.h"

#define SB_MAX_BUCKETS 64        /* месяцев (листов) максимум */
#define SB_KEY         192       /* ключ строки: day|employee|shift|cabinet */
#define SB_ROW         4096     /* одна JSON-строка (запас под экранирование) */

typedef struct {
    char   month[64];
    char  *rows;                 /* содержимое JSON-массива: {...},{...} */
    size_t len, cap;
    int    count;
    char (*keys)[SB_KEY];        /* дедупликация PK в батче */
    int    nkeys, kcap;
} SBucket;

static char    sb_base[320];     /* https://<ref>.supabase.co/rest/v1 */
static char    sb_secret[512];
static int     sb_on = 0;
static char    sb_cutoff[40] = "";
static SBucket sb_buckets[SB_MAX_BUCKETS];
static int     sb_nb = 0;

/* ---------------- env / состояние ---------------- */

void sb_init(void) {
    const char *url = getenv("SUPABASE_URL");
    const char *key = getenv("SUPABASE_SECRET_KEY");
    if (!key || !*key) key = getenv("SUPABASE_SERVICE_ROLE_KEY");
    const char *off = getenv("SUPABASE_SYNC");

    if (!url || !*url || !key || !*key || (off && off[0] == '0')) {
        sb_on = 0;
        fprintf(stderr, "[supabase] buffer off (%s)\n",
                (!url || !*url)   ? "нет SUPABASE_URL"
                : (!key || !*key) ? "нет SUPABASE_SECRET_KEY"
                                  : "SUPABASE_SYNC=0");
        return;
    }

    size_t n = strlen(url);
    while (n > 0 && url[n - 1] == '/') n--;
    snprintf(sb_base, sizeof(sb_base), "%.*s/rest/v1", (int)n, url);
    snprintf(sb_secret, sizeof(sb_secret), "%s", key);
    sb_on = 1;
    fprintf(stderr, "[supabase] schedule buffer on: %.90s\n", sb_base);
}

int sb_enabled(void) { return sb_on; }

void sb_now_iso(char *out, size_t cap) {
    time_t t = time(NULL);
    struct tm tmv;
    gmtime_r(&t, &tmv);
    strftime(out, cap, "%Y-%m-%dT%H:%M:%SZ", &tmv);
}

void sb_begin_cycle(void) {
    if (sb_on) sb_now_iso(sb_cutoff, sizeof(sb_cutoff));
}

/* ---------------- JSON ---------------- */

/* Экранирование строки для вставки в JSON (UTF-8 — как есть). */
static void jesc(char *out, size_t cap, const char *s) {
    size_t o = 0;
    if (!s) s = "";
    if (cap == 0) return;
    for (; *s && o + 8 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        else if (c == '\r') { out[o++] = '\\'; out[o++] = 'r'; }
        else if (c == '\t') { out[o++] = '\\'; out[o++] = 't'; }
        else if (c < 0x20) {
            int r = snprintf(out + o, cap - o, "\\u%04x", c);
            if (r > 0) o += (size_t)r;
        }
        else out[o++] = (char)c;
    }
    out[o] = '\0';
}

/* ---------------- буфер строк по месяцам ---------------- */

static int grow(char **p, size_t *cap, size_t need) {
    if (need <= *cap) return 1;
    size_t n = *cap ? *cap : 8192;
    while (n < need) n *= 2;
    char *tmp = realloc(*p, n);
    if (!tmp) return 0;
    *p = tmp;
    *cap = n;
    return 1;
}

static SBucket *bucket(const char *month) {
    for (int i = 0; i < sb_nb; i++)
        if (!strcmp(sb_buckets[i].month, month)) return &sb_buckets[i];
    if (sb_nb >= SB_MAX_BUCKETS) return NULL;
    SBucket *b = &sb_buckets[sb_nb++];
    memset(b, 0, sizeof(*b));
    snprintf(b->month, sizeof(b->month), "%s", month);
    return b;
}

void sb_row(const char *month, int day, const char *employee,
            const char *shift, const char *cabinet) {
    if (!sb_on || !month || !*month || day < 1 || day > 31) return;
    if (!employee || !*employee || !cabinet || !*cabinet) return;

    SBucket *b = bucket(month);
    if (!b) return;

    /* дедуп PK «день|сотрудник|смена|кабинет» */
    char pk[SB_KEY];
    snprintf(pk, sizeof(pk), "%d|%s|%s|%s", day, employee, shift ? shift : "", cabinet);
    for (int i = 0; i < b->nkeys; i++)
        if (!strcmp(b->keys[i], pk)) return;
    if (b->nkeys >= b->kcap) {
        int nk = b->kcap ? b->kcap * 2 : 64;
        char (*tmp)[SB_KEY] = realloc(b->keys, (size_t)nk * SB_KEY);
        if (!tmp) return;
        b->keys = tmp;
        b->kcap = nk;
    }
    memcpy(b->keys[b->nkeys], pk, SB_KEY);
    b->nkeys++;

    char mm[192], me[768], ms[768], mc[768], cut[64] = "";
    jesc(mm, sizeof(mm), month);
    jesc(me, sizeof(me), employee);
    jesc(ms, sizeof(ms), shift);
    jesc(mc, sizeof(mc), cabinet);
    if (sb_cutoff[0]) snprintf(cut, sizeof(cut), ",\"updated_at\":\"%s\"", sb_cutoff);

    char row[SB_ROW];
    snprintf(row, sizeof(row),
             "%s{\"month\":\"%s\",\"day\":%d,\"employee\":\"%s\","
             "\"shift_time\":\"%s\",\"cabinet\":\"%s\"%s}",
             b->count ? "," : "", mm, day, me, ms, mc, cut);

    if (!grow(&b->rows, &b->cap, b->len + strlen(row) + 1)) { b->nkeys--; return; }
    memcpy(b->rows + b->len, row, strlen(row));
    b->len += strlen(row);
    b->rows[b->len] = '\0';
    b->count++;
}

const char *sb_pending(const char *month, int *count) {
    if (count) *count = 0;
    for (int i = 0; i < sb_nb; i++) {
        if (!strcmp(sb_buckets[i].month, month)) {
            if (count) *count = sb_buckets[i].count;
            return sb_buckets[i].rows ? sb_buckets[i].rows : "";
        }
    }
    return NULL;
}

/* ---------------- HTTP к Supabase ---------------- */

/* method: "POST" | "DELETE" | "GET"; path — "/schedule?..." (без базы). */
static int sb_call(const char *method, const char *path, const char *body) {
    char url[1024];
    snprintf(url, sizeof(url), "%s%s", sb_base, path);

    char apikey[600], auth[600];
    snprintf(apikey, sizeof(apikey), "apikey: %s", sb_secret);
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", sb_secret);

    const char *extra[5];
    int e = 0;
    extra[e++] = apikey;
    extra[e++] = auth;
    if (!strcmp(method, "POST"))
        extra[e++] = "Prefer: resolution=merge-duplicates,return=minimal";
    else
        extra[e++] = "Prefer: return=minimal";
    extra[e] = NULL;

    int status = 0;
    char *out = http_request_h(method, url, "application/json", body, extra, &status);
    free(out);
    return status;
}

/* ---------------- flush ---------------- */

int sb_flush(const char *const *months, int nmonths, int *errors) {
    if (errors) *errors = 0;
    if (!sb_on) return 0;
    int total = 0, errs = 0;

    /* 1) upsert накопленных строк */
    for (int i = 0; i < sb_nb; i++) {
        SBucket *b = &sb_buckets[i];
        if (b->count <= 0) continue;

        char *body = malloc(b->len + 3);
        if (!body) { errs++; continue; }
        body[0] = '[';
        memcpy(body + 1, b->rows, b->len);
        body[b->len + 1] = ']';
        body[b->len + 2] = '\0';

        int st = sb_call("POST", "/schedule", body);
        free(body);
        if (st < 200 || st > 299) {
            errs++;
            fprintf(stderr, "[supabase] upsert %s rows=%d -> HTTP %d\n",
                    b->month, b->count, st);
        } else {
            total += b->count;
        }
        b->len = 0;
        b->count = 0;
        b->nkeys = 0;
    }

    /* 2) уборка устаревших строк (строки с прошлых циклов) и
          полная очистка месяцев, которых больше нет в Google */
    if (sb_cutoff[0]) {
        for (int i = 0; i < nmonths; i++) {
            if (!months || !months[i] || !months[i][0]) continue;
            char *em = url_encode(months[i]);
            char *ec = url_encode(sb_cutoff);
            if (em && ec) {
                char path[768];
                snprintf(path, sizeof(path),
                         "/schedule?month=eq.%s&updated_at=lt.%s", em, ec);
                int st = sb_call("DELETE", path, NULL);
                if (st < 200 || st > 299) {
                    errs++;
                    fprintf(stderr, "[supabase] delete stale %s -> HTTP %d\n",
                            months[i], st);
                }
            }
            free(em);
            free(ec);
        }
    }
    for (int i = 0; i < sb_nb; i++) {
        int known = 0;
        for (int j = 0; months && j < nmonths; j++)
            if (months[j] && !strcmp(months[j], sb_buckets[i].month)) { known = 1; break; }
        if (known) continue;
        char *em = url_encode(sb_buckets[i].month);
        if (em) {
            char path[320];
            snprintf(path, sizeof(path), "/schedule?month=eq.%s", em);
            int st = sb_call("DELETE", path, NULL);
            if (st < 200 || st > 299) {
                errs++;
                fprintf(stderr, "[supabase] delete removed month %s -> HTTP %d\n",
                        sb_buckets[i].month, st);
            }
        }
        free(em);
        free(sb_buckets[i].rows);
        free(sb_buckets[i].keys);
        memset(&sb_buckets[i], 0, sizeof(sb_buckets[i]));
        /* месяц исчез — сдвигаем массив bucket'ов */
        for (int j = i; j + 1 < sb_nb; j++) sb_buckets[j] = sb_buckets[j + 1];
        memset(&sb_buckets[sb_nb - 1], 0, sizeof(sb_buckets[0]));
        sb_nb--;
        i--;
    }

    if (errors) *errors = errs;
    return total;
}

void sb_log(const char *kind, const char *month, int rows, int ok, const char *error) {
    if (!sb_on) return;
    char kb[160], mb[160], eb[640];
    jesc(kb, sizeof(kb), kind ? kind : "");
    jesc(mb, sizeof(mb), month ? month : "");
    jesc(eb, sizeof(eb), error ? error : "");
    char body[1600];
    snprintf(body, sizeof(body),
             "{\"kind\":\"%s\",\"month\":\"%s\",\"row_count\":%d,"
             "\"ok\":%s,\"error\":\"%s\"}",
             kb, mb, rows, ok ? "true" : "false", eb);
    int st = sb_call("POST", "/sync_log", body);
    if (st < 200 || st > 299)
        fprintf(stderr, "[supabase] sync_log -> HTTP %d (таблица создана?)\n", st);
}

/* infra/supabase.c — транспорт и буфер строк для Supabase (PostgREST).
 *
 * Цикл (раз в 60 с, из app/background):
 *   1. sb_begin_cycle()           — фиксируем updated_at цикла
 *   2. sb_row(...) во время обхода листов Google
 *   3. sb_flush(months, n, &err):
 *        POST   /rest/v1/schedule              — upsert (merge-duplicates)
 *        DELETE /rest/v1/schedule?month=eq.X&updated_at=lt.<cutoff>
 *               — уборка строк, которых больше нет в Google
 *        DELETE /rest/v1/schedule?month=eq.X   — месяцы, пропавшие из Google
 *   4. sb_log(...)                — журнал sync_log (пульс каждые 10 циклов)
 *
 * Дубликаты PK внутри одного батча отсекаются: PostgREST отдаёт ошибку
 * «ON CONFLICT DO UPDATE cannot affect row a second time».
 */
#include "infra/supabase.h"

#include "vendor/cJSON.h"
#include "core/json_text.h"
#include "core/num.h"
#include "core/strbuf.h"
#include "core/url_codec.h"
#include "infra/http_client.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

static void sb_now_iso(char *out, size_t cap) {
    time_t t = time(NULL);
    struct tm tmv;
    gmtime_r(&t, &tmv);
    strftime(out, cap, "%Y-%m-%dT%H:%M:%SZ", &tmv);
}

void sb_begin_cycle(void) {
    if (sb_on) sb_now_iso(sb_cutoff, sizeof(sb_cutoff));
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
    /* Фильтр валидности (согласован): строка в базу попадает только если
       employee И shift_time непустые. Служебные строки листа («Всего дней
       в месяце», «Кабинет 1.1 (1-я смена)», «Смен в К1», итоги/проценты)
       имеют пустую смену и отсекаются. Пустое имя уже отсекалось. */
    if (!employee || !*employee || !shift || !*shift) return;
    if (!cabinet || !*cabinet) return;

    SBucket *b = bucket(month);
    if (!b) return;

    /* дедуп PK «день|сотрудник|смена|кабинет» */
    char pk[SB_KEY];
    snprintf(pk, sizeof(pk), "%d|%s|%s|%s", day, employee, shift, cabinet);
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

    StrBuf sb;
    sbuf_init(&sb);
    sbuf_appendf(&sb, "%s{\"month\":", b->count ? "," : "");
    sbuf_append_json_string(&sb, month);
    sbuf_appendf(&sb, ",\"day\":%d,\"employee\":", day);
    sbuf_append_json_string(&sb, employee);
    sbuf_append(&sb, ",\"shift_time\":");
    sbuf_append_json_string(&sb, shift);
    sbuf_append(&sb, ",\"cabinet\":");
    sbuf_append_json_string(&sb, cabinet);
    if (sb_cutoff[0]) {
        sbuf_append(&sb, ",\"updated_at\":");
        sbuf_append_json_string(&sb, sb_cutoff);
    }
    sbuf_append(&sb, "}");

    char *row = sbuf_take(&sb);
    if (!row) return;

    if (!grow(&b->rows, &b->cap, b->len + strlen(row) + 1)) {
        free(row);
        b->nkeys--;
        return;
    }
    memcpy(b->rows + b->len, row, strlen(row));
    b->len += strlen(row);
    b->rows[b->len] = '\0';
    b->count++;
    free(row);
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
    char *out = http_request_h(method, url, "application/json", body, extra,
                               &status, NULL, 0);
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
    StrBuf sb;
    sbuf_init(&sb);
    sbuf_append(&sb, "{\"kind\":");
    sbuf_append_json_string(&sb, kind ? kind : "");
    sbuf_append(&sb, ",\"month\":");
    sbuf_append_json_string(&sb, month ? month : "");
    sbuf_appendf(&sb, ",\"row_count\":%d,\"ok\":%s,\"error\":", rows, ok ? "true" : "false");
    sbuf_append_json_string(&sb, error ? error : "");
    sbuf_append(&sb, "}");
    char *body = sbuf_take(&sb);
    if (!body) return;
    int st = sb_call("POST", "/sync_log", body);
    free(body);
    if (st < 200 || st > 299)
        fprintf(stderr, "[supabase] sync_log -> HTTP %d (таблица создана?)\n", st);
}

/* ================= Чтение из SQL ================= */

char *sb_get(const char *path, int *status) {
    if (status) *status = 0;
    if (!sb_on || !path) return NULL;
    char url[2048];
    snprintf(url, sizeof(url), "%s%s", sb_base, path);
    char apikey[600], auth[600];
    snprintf(apikey, sizeof(apikey), "apikey: %s", sb_secret);
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", sb_secret);
    const char *extra[3] = { apikey, auth, NULL };
    return http_request_h("GET", url, NULL, NULL, extra, status, NULL, 0);
}

/* ================= Учёт времени (time_entries) ================= */

int sb_time_upsert(const char *month_tab, int day,
                   const char *in_t, const char *out_t,
                   const char *hours, const char *sum) {
    if (!sb_on || !month_tab || !*month_tab || day < 1 || day > 31) return 0;

    StrBuf sb;
    sbuf_init(&sb);
    sbuf_append(&sb, "{\"month\":");
    sbuf_append_json_string(&sb, month_tab);
    sbuf_appendf(&sb, ",\"day\":%d,\"t_in\":", day);
    sbuf_append_json_string(&sb, in_t ? in_t : "");
    sbuf_append(&sb, ",\"t_out\":");
    sbuf_append_json_string(&sb, out_t ? out_t : "");
    sbuf_append(&sb, ",\"hours\":");
    /* колонки numeric: не-число -> null (не уронит каст на PostgREST) */
    sbuf_append(&sb, num_is_valid(hours) ? "" : "null");
    if (num_is_valid(hours)) sbuf_append(&sb, hours);
    sbuf_append(&sb, ",\"amount\":");
    sbuf_append(&sb, num_is_valid(sum) ? "" : "null");
    if (num_is_valid(sum)) sbuf_append(&sb, sum);
    sbuf_append(&sb, "}");
    char *body = sbuf_take(&sb);
    if (!body) return 0;

    int st = sb_call("POST", "/time_entries", body);
    free(body);
    if (st < 200 || st > 299)
        fprintf(stderr, "[supabase] time upsert %s day=%d -> HTTP %d\n",
                month_tab, day, st);
    return st;
}

int sb_time_day_delete(const char *month_tab, int day) {
    if (!sb_on || !month_tab || !*month_tab || day < 1 || day > 31) return 0;
    char *em = url_encode(month_tab);
    if (!em) return 0;
    char path[512];
    snprintf(path, sizeof(path), "/time_entries?month=eq.%s&day=eq.%d", em, day);
    free(em);
    return sb_call("DELETE", path, NULL);
}

int sb_time_day_read(const char *month_tab, int day,
                     char *in, size_t incap,
                     char *out, size_t outcap,
                     char *hours, size_t hcap,
                     char *sum, size_t scap) {
    if (!sb_on || !month_tab || !*month_tab || day < 1 || day > 31) return -1;
    if (in && incap) in[0] = '\0';
    if (out && outcap) out[0] = '\0';
    if (hours && hcap) hours[0] = '\0';
    if (sum && scap) sum[0] = '\0';

    char *em = url_encode(month_tab);
    if (!em) return -1;
    char path[512];
    snprintf(path, sizeof(path),
             "/time_entries?month=eq.%s&day=eq.%d"
             "&select=t_in,t_out,hours,amount&limit=1", em, day);
    free(em);

    int st = 0;
    char *body = sb_get(path, &st);
    if (!body || st < 200 || st > 300) { free(body); return -1; }
    cJSON *arr = cJSON_Parse(body);
    free(body);
    if (!arr || !cJSON_IsArray(arr)) { cJSON_Delete(arr); return -1; }

    int rc = 1;   /* дня нет */
    if (cJSON_GetArraySize(arr) > 0) {
        cJSON *it = cJSON_GetArrayItem(arr, 0);
        cJSON *ji = cJSON_GetObjectItemCaseSensitive(it, "t_in");
        cJSON *jo = cJSON_GetObjectItemCaseSensitive(it, "t_out");
        cJSON *jh = cJSON_GetObjectItemCaseSensitive(it, "hours");
        cJSON *ja = cJSON_GetObjectItemCaseSensitive(it, "amount");
        if (in && incap && cJSON_IsString(ji) && ji->valuestring)
            snprintf(in, incap, "%s", ji->valuestring);
        if (out && outcap && cJSON_IsString(jo) && jo->valuestring)
            snprintf(out, outcap, "%s", jo->valuestring);
        if (hours && hcap && cJSON_IsNumber(jh))
            snprintf(hours, hcap, "%.2f", jh->valuedouble);
        if (sum && scap && cJSON_IsNumber(ja))
            snprintf(sum, scap, "%.2f", ja->valuedouble);
        rc = 0;
    }
    cJSON_Delete(arr);
    return rc;
}

/* ================= Диагностика: /diag/supabase ================= */

/* Точный счётчик строк PostgREST: GET ...&limit=0 с Prefer: count=exact —
   тело пустое, число в заголовке Content-Range "a-b/total".
   Возвращает HTTP-код; *total = -1, если заголовка нет. */
static int sb_count(const char *path, long *total) {
    if (total) *total = -1;
    char url[2048];
    snprintf(url, sizeof(url), "%s%s", sb_base, path);
    char apikey[600], auth[600];
    snprintf(apikey, sizeof(apikey), "apikey: %s", sb_secret);
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", sb_secret);
    const char *extra[4] = { apikey, auth, "Prefer: count=exact", NULL };
    char hdr[1024];
    int status = 0;
    char *body = http_request_h("GET", url, NULL, NULL, extra, &status, hdr, sizeof(hdr));
    free(body);
    /* HTTP/2 отдаёт имена заголовков в нижнем регистре, HTTP/1.1 — обычные.
       Ищем '/' ТОЛЬКО внутри строки заголовка: в буфере есть и другие
       заголовки с '/' (set-cookie: Path=/, alt-svc), которые дали бы 0. */
    const char *cr = strstr(hdr, "Content-Range:");
    if (!cr) cr = strstr(hdr, "content-range:");
    if (cr) {
        const char *eol = strpbrk(cr, "\r\n");
        size_t len = eol ? (size_t)(eol - cr) : strlen(cr);
        char line[256];
        if (len > sizeof(line) - 1) len = sizeof(line) - 1;
        memcpy(line, cr, len);
        line[len] = '\0';
        const char *slash = strrchr(line, '/');
        if (slash) *total = strtol(slash + 1, NULL, 10);
    }
    return status;
}

/* ISO "2026-10-27T11:48:30.12+00:00" -> unix-время (0 при ошибке) */
static time_t sb_parse_iso(const char *s) {
    int Y = 0, M = 0, D = 0, h = 0, mi = 0, sec = 0;
    if (!s || sscanf(s, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &mi, &sec) != 6)
        return 0;
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = Y - 1900;
    tmv.tm_mon = M - 1;
    tmv.tm_mday = D;
    tmv.tm_hour = h;
    tmv.tm_min = mi;
    tmv.tm_sec = sec;
    return timegm(&tmv);
}

int sb_diag(const char *const *months, int nmonths, char *out, size_t cap) {
    if (!out || cap < 64) return -1;
    out[0] = '\0';
    if (!sb_on) {
        snprintf(out, cap,
                 "{\"ok\":false,\"error\":\"синхронизация выключена "
                 "(нет SUPABASE_URL/SUPABASE_SECRET_KEY или SUPABASE_SYNC=0)\"}");
        return -1;
    }

    int errs = 0;
    long total = -1, empty_sh = -1, empty_em = -1, valid = -1;
    if (sb_count("/schedule?select=day&limit=0", &total) < 200) errs++;
    if (sb_count("/schedule?select=day&limit=0&shift_time=eq.", &empty_sh) < 200) errs++;
    if (sb_count("/schedule?select=day&limit=0&employee=eq.", &empty_em) < 200) errs++;
    if (sb_count("/schedule?select=day&limit=0&shift_time=not.eq.&employee=not.eq.", &valid) < 200)
        errs++;

    /* построчные счётчики по каждому месяцу */
    StrBuf months_js;
    sbuf_init(&months_js);
    sbuf_append(&months_js, "[");
    int mn = 0;
    for (int i = 0; months && i < nmonths; i++) {
        if (!months[i] || !*months[i]) continue;
        char *em = url_encode(months[i]);
        if (!em) continue;
        char path[640];
        long t2 = -1, es = -1, va = -1;
        int ok1, ok2, ok3;
        snprintf(path, sizeof(path), "/schedule?select=day&limit=0&month=eq.%s", em);
        ok1 = sb_count(path, &t2) >= 200;
        snprintf(path, sizeof(path), "/schedule?select=day&limit=0&month=eq.%s&shift_time=eq.", em);
        ok2 = sb_count(path, &es) >= 200;
        snprintf(path, sizeof(path),
                 "/schedule?select=day&limit=0&month=eq.%s&shift_time=not.eq.&employee=not.eq.", em);
        ok3 = sb_count(path, &va) >= 200;
        free(em);
        if (!ok1 || !ok2 || !ok3) { errs++; continue; }
        sbuf_appendf(&months_js, "%s{\"month\":", mn ? "," : "");
        sbuf_append_json_string(&months_js, months[i]);
        sbuf_appendf(&months_js, ",\"total\":%ld,\"empty_shift\":%ld,\"valid\":%ld}",
                   t2, es, va);
        mn++;
    }
    sbuf_append(&months_js, "]");
    char *months_buf = sbuf_take(&months_js);

    /* time_entries: точный счёт + уникальные месяцы (строк немного) */
    long te_total = -1;
    if (sb_count("/time_entries?select=day&limit=0", &te_total) < 200) errs++;
    StrBuf te_months;
    sbuf_init(&te_months);
    sbuf_append(&te_months, "[");
    {
        int st = 0;
        char *body = sb_get("/time_entries?select=month&limit=1000", &st);
        if (body && st >= 200 && st < 300) {
            cJSON *arr = cJSON_Parse(body);
            if (arr && cJSON_IsArray(arr)) {
                char uniq[64][64];
                int nun = 0;
                cJSON *it = NULL;
                cJSON_ArrayForEach(it, arr) {
                    cJSON *m = cJSON_GetObjectItemCaseSensitive(it, "month");
                    if (!cJSON_IsString(m) || !m->valuestring || !m->valuestring[0]) continue;
                    int dup = 0;
                    for (int u = 0; u < nun; u++)
                        if (!strcmp(uniq[u], m->valuestring)) { dup = 1; break; }
                    if (dup || nun >= 64) continue;
                    snprintf(uniq[nun], sizeof(uniq[0]), "%s", m->valuestring);
                    nun++;
                }
                for (int u = 0; u < nun; u++) {
                    sbuf_append(&te_months, u ? "," : "");
                    sbuf_append_json_string(&te_months, uniq[u]);
                }
            }
            cJSON_Delete(arr);
        } else {
            errs++;
        }
        free(body);
    }
    sbuf_append(&te_months, "]");
    char *te_months_buf = sbuf_take(&te_months);

    /* журнал: последняя запись + последний restore + возраст пульса */
    StrBuf log_js;
    sbuf_init(&log_js);
    int log_ok = 0;
    {
        int st = 0;
        char *body = sb_get("/sync_log?select=kind,row_count,ok,error,at"
                            "&order=at.desc&limit=10", &st);
        if (body && st >= 200 && st < 300) {
            cJSON *arr = cJSON_Parse(body);
            if (arr && cJSON_IsArray(arr)) {
                time_t now = time(NULL);
                cJSON *first = cJSON_GetArrayItem(arr, 0);
                if (first) {
                    cJSON *kind = cJSON_GetObjectItemCaseSensitive(first, "kind");
                    cJSON *rc   = cJSON_GetObjectItemCaseSensitive(first, "row_count");
                    cJSON *ok   = cJSON_GetObjectItemCaseSensitive(first, "ok");
                    cJSON *at   = cJSON_GetObjectItemCaseSensitive(first, "at");
                    cJSON *errf = cJSON_GetObjectItemCaseSensitive(first, "error");
                    long age = 0;
                    if (cJSON_IsString(at) && at->valuestring) {
                        time_t t = sb_parse_iso(at->valuestring);
                        if (t > 0) age = (long)(now - t);
                    }
                    sbuf_append(&log_js, "\"last\":{\"kind\":");
                    sbuf_append_json_string(&log_js,
                        (cJSON_IsString(kind) && kind->valuestring) ? kind->valuestring : "");
                    sbuf_appendf(&log_js, ",\"rows\":%d,\"ok\":%s,\"error\":",
                               cJSON_IsNumber(rc) ? rc->valueint : 0,
                               cJSON_IsTrue(ok) ? "true" : "false");
                    sbuf_append_json_string(&log_js,
                        (cJSON_IsString(errf) && errf->valuestring) ? errf->valuestring : "");
                    sbuf_append(&log_js, ",\"at\":");
                    sbuf_append_json_string(&log_js,
                        (cJSON_IsString(at) && at->valuestring) ? at->valuestring : "");
                    sbuf_appendf(&log_js, ",\"age_sec\":%ld}", age);
                }
                sbuf_append(&log_js, ",\"last_restore\":");
                int found_restore = 0;
                for (int i = 0; i < cJSON_GetArraySize(arr); i++) {
                    cJSON *it = cJSON_GetArrayItem(arr, i);
                    cJSON *kind = cJSON_GetObjectItemCaseSensitive(it, "kind");
                    if (!cJSON_IsString(kind) || !kind->valuestring ||
                        strcmp(kind->valuestring, "time_restore") != 0) continue;
                    cJSON *rc   = cJSON_GetObjectItemCaseSensitive(it, "row_count");
                    cJSON *ok   = cJSON_GetObjectItemCaseSensitive(it, "ok");
                    cJSON *at   = cJSON_GetObjectItemCaseSensitive(it, "at");
                    cJSON *errf = cJSON_GetObjectItemCaseSensitive(it, "error");
                    long age = 0;
                    if (cJSON_IsString(at) && at->valuestring) {
                        time_t t = sb_parse_iso(at->valuestring);
                        if (t > 0) age = (long)(now - t);
                    }
                    sbuf_appendf(&log_js, "{\"rows\":%d,\"ok\":%s,\"error\":",
                              cJSON_IsNumber(rc) ? rc->valueint : 0,
                              cJSON_IsTrue(ok) ? "true" : "false");
                    sbuf_append_json_string(&log_js,
                        (cJSON_IsString(errf) && errf->valuestring) ? errf->valuestring : "");
                    sbuf_append(&log_js, ",\"at\":");
                    sbuf_append_json_string(&log_js,
                        (cJSON_IsString(at) && at->valuestring) ? at->valuestring : "");
                    sbuf_appendf(&log_js, ",\"age_sec\":%ld}", age);
                    found_restore = 1;
                    break;
                }
                if (!found_restore) sbuf_append(&log_js, "null");
                log_ok = 1;
            }
            cJSON_Delete(arr);
        } else {
            errs++;
        }
        free(body);
    }
    if (!log_ok) {
        sbuf_free(&log_js);
        sbuf_init(&log_js);
        sbuf_append(&log_js, "\"unavailable\":true");
    }
    char *log_buf = sbuf_take(&log_js);

    char now[40];
    sb_now_iso(now, sizeof(now));
    int n = snprintf(out, cap,
        "{\"ok\":%s,\"at\":\"%s\",\"errors\":%d,"
        "\"schedule\":{\"total\":%ld,\"valid\":%ld,"
        "\"empty_shift\":%ld,\"empty_employee\":%ld},"
        "\"months\":[%s],"
        "\"time_entries\":{\"total\":%ld,\"months\":[%s]},"
        "\"sync_log\":{%s}}",
        (total >= 0) ? "true" : "false", now, errs,
        total, valid, empty_sh, empty_em,
        months_buf ? months_buf : "",
        te_total, te_months_buf ? te_months_buf : "",
        log_buf ? log_buf : "");
    free(months_buf);
    free(te_months_buf);
    free(log_buf);
    if (n < 0 || (size_t)n >= cap) return -1;
    return (total >= 0) ? 0 : -1;
}

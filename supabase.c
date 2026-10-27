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
#include <stdarg.h>
#include <ctype.h>
#include <time.h>

#include "cJSON.h"
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

/* ================= Этап 2: чтение из SQL ================= */

/* snprintf-аппенд с контролем переполнения: новый offset или (size_t)-1 */
static size_t addf(char *out, size_t cap, size_t off, const char *fmt, ...) {
    if (out == NULL || off >= cap) return (size_t)-1;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out + off, cap - off, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - off) return (size_t)-1;
    return off + (size_t)n;
}

/* «7:30-14:40» -> минуты начала смены; не разобрано -> 999999 (в конец) */
static int shift_start_min(const char *s) {
    if (!s) return 999999;
    while (*s == ' ') s++;
    int h = 0, m = 0;
    if (sscanf(s, "%d:%d", &h, &m) == 2 && h >= 0 && h < 48 && m >= 0 && m < 60)
        return h * 60 + m;
    return 999999;
}

char *sb_get(const char *path, int *status) {
    if (status) *status = 0;
    if (!sb_on || !path) return NULL;
    char url[2048];
    snprintf(url, sizeof(url), "%s%s", sb_base, path);
    char apikey[600], auth[600];
    snprintf(apikey, sizeof(apikey), "apikey: %s", sb_secret);
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", sb_secret);
    const char *extra[3] = { apikey, auth, NULL };
    return http_request_h("GET", url, NULL, NULL, extra, status);
}

#define SB_AGG_MAX 8   /* смен у сотрудника в месяце (Mini App показывает 2) */

int sb_month_arrays(const char *rows_json, const char *employee,
                    int sheet_days, const char *total,
                    char *s1, size_t s1cap, char *s2, size_t s2cap) {
    if (!s1 || !s2 || s1cap < 16 || s2cap < 16) return -1;
    s1[0] = '\0';
    s2[0] = '\0';

    cJSON *arr = rows_json ? cJSON_Parse(rows_json) : NULL;
    if (!arr || !cJSON_IsArray(arr)) { cJSON_Delete(arr); return -1; }

    struct Agg {
        char  shift[128];
        int   start;                       /* минуты начала — порядок как в листе */
        int   maxday;
        int   count;                       /* «Всего смен» */
        char *cells[32];                   /* cells[день] = кабинет */
    } aggs[SB_AGG_MAX];
    int na = 0, maxday = 0;

    int size = cJSON_GetArraySize(arr);
    for (int i = 0; i < size; i++) {
        cJSON *it  = cJSON_GetArrayItem(arr, i);
        cJSON *jd  = it ? cJSON_GetObjectItemCaseSensitive(it, "day") : NULL;
        cJSON *js  = it ? cJSON_GetObjectItemCaseSensitive(it, "shift_time") : NULL;
        cJSON *jc  = it ? cJSON_GetObjectItemCaseSensitive(it, "cabinet") : NULL;
        if (!cJSON_IsNumber(jd) || !cJSON_IsString(jc) || !jc->valuestring) continue;
        int day = (int)jd->valuedouble;
        if (day < 1 || day > 31) continue;
        const char *sh = (cJSON_IsString(js) && js->valuestring) ? js->valuestring : "";

        int k;
        for (k = 0; k < na; k++)
            if (!strcmp(aggs[k].shift, sh)) break;
        if (k == na) {
            if (na >= SB_AGG_MAX) continue;
            memset(&aggs[na], 0, sizeof(aggs[na]));
            snprintf(aggs[na].shift, sizeof(aggs[na].shift), "%s", sh);
            aggs[na].start = shift_start_min(sh);
            k = na++;
        }
        if (day > aggs[k].maxday) aggs[k].maxday = day;
        if (day > maxday) maxday = day;
        aggs[k].count++;
        free(aggs[k].cells[day]);
        aggs[k].cells[day] = strdup(jc->valuestring);
    }
    cJSON_Delete(arr);

    if (na == 0 || maxday < 1) goto cleanup;   /* данных нет — вызывающий уйдёт в фоллбэк */

    /* смены по времени начала: утренняя = s1, вечерняя = s2 (как в листе) */
    for (int i = 1; i < na; i++) {
        struct Agg tmp = aggs[i];
        int j = i - 1;
        for (; j >= 0 && aggs[j].start > tmp.start; j--) aggs[j + 1] = aggs[j];
        aggs[j + 1] = tmp;
    }

    /* число дней = число столбцов листа (sheet_days), но не меньше данных:
       счётчик должен стоять после последнего дня, как в листе */
    int dim = sheet_days > maxday ? sheet_days : maxday;
    if (dim < 1 || dim > 31) { goto cleanup; }

    char em[768], sh[256], cell[768];
    jesc(em, sizeof(em), employee ? employee : "");
    jesc(sh, sizeof(sh), aggs[0].shift);

    /* s1: [имя, смена, day1..dayD, всего смен] — формат строки листа */
    size_t o = addf(s1, s1cap, 0, "[\"%s\",\"%s\"", em, sh);
    for (int d = 1; d <= dim && o != (size_t)-1; d++) {
        const char *v = (d <= aggs[0].maxday && aggs[0].cells[d]) ? aggs[0].cells[d] : "";
        jesc(cell, sizeof(cell), v);
        o = addf(s1, s1cap, o, ",\"%s\"", cell);
    }
    if (total) {
        char t[64];
        jesc(t, sizeof(t), total);
        o = addf(s1, s1cap, o, ",\"%s\"]", t);      /* значение из листа */
    } else {
        o = addf(s1, s1cap, o, ",\"%d\"]", aggs[0].count);   /* запасной подсчёт */
    }

    /* s2: вторая смена либо пустой массив той же длины (как без второй строки) */
    size_t o2 = 0;
    if (na >= 2) {
        jesc(sh, sizeof(sh), aggs[1].shift);
        o2 = addf(s2, s2cap, 0, "[\"\",\"%s\"", sh);
        for (int d = 1; d <= dim && o2 != (size_t)-1; d++) {
            const char *v = (d <= aggs[1].maxday && aggs[1].cells[d]) ? aggs[1].cells[d] : "";
            jesc(cell, sizeof(cell), v);
            o2 = addf(s2, s2cap, o2, ",\"%s\"", cell);
        }
        if (total) {
            o2 = addf(s2, s2cap, o2, ",\"\"]");    /* в листе счётчик только в 1-й строке */
        } else {
            o2 = addf(s2, s2cap, o2, ",\"%d\"]", aggs[1].count);
        }
    } else {
        o2 = addf(s2, s2cap, 0, "[");
        for (int d = 0; d < dim + 3 && o2 != (size_t)-1; d++)
            o2 = addf(s2, s2cap, o2, "%s\"\"", d ? "," : "");
        o2 = addf(s2, s2cap, o2, "]");
    }

    if (o == (size_t)-1 || o2 == (size_t)-1) {
        s1[0] = '\0';
        s2[0] = '\0';
    }

cleanup:
    for (int k = 0; k < na; k++)
        for (int d = 0; d < 32; d++) free(aggs[k].cells[d]);
    if (!s1[0]) return -1;   /* ничего не построено (нет данных либо переполнение) */
    return 0;
}

int sb_colleagues_json(const char *rows_json, const char *self,
                       const char *shift, SBShiftFilter filter,
                       char *out, size_t outcap) {
    if (!out || outcap < 4) return -1;
    out[0] = '\0';

    cJSON *arr = rows_json ? cJSON_Parse(rows_json) : NULL;
    if (!arr || !cJSON_IsArray(arr)) { cJSON_Delete(arr); return -1; }

    char enames[64][256];
    char eshifts[64][128];
    int emitted = 0, nout = 0;
    size_t o = addf(out, outcap, 0, "[");

    int size = cJSON_GetArraySize(arr);
    for (int i = 0; i < size && emitted < 64; i++) {
        cJSON *it = cJSON_GetArrayItem(arr, i);
        cJSON *je = it ? cJSON_GetObjectItemCaseSensitive(it, "employee") : NULL;
        cJSON *js = it ? cJSON_GetObjectItemCaseSensitive(it, "shift_time") : NULL;
        if (!cJSON_IsString(je) || !je->valuestring || !je->valuestring[0]) continue;
        const char *name = je->valuestring;
        const char *s = (cJSON_IsString(js) && js->valuestring) ? js->valuestring : "";

        if (self && *self && !strcmp(name, self)) continue;          /* сам сотрудник */
        if (shift && *shift && filter && !filter(s, shift)) continue; /* пересечение смен */

        int dup = 0;
        for (int q = 0; q < emitted; q++)
            if (!strcmp(enames[q], name) && !strcmp(eshifts[q], s)) { dup = 1; break; }
        if (dup) continue;
        snprintf(enames[emitted], sizeof(enames[0]), "%s", name);
        snprintf(eshifts[emitted], sizeof(eshifts[0]), "%s", s);
        emitted++;

        char en[768], es[512];
        jesc(en, sizeof(en), name);
        jesc(es, sizeof(es), s);
        o = addf(out, outcap, o, "%s{\"name\":\"%s\",\"shift\":\"%s\"}",
                 nout ? "," : "", en, es);
        if (o == (size_t)-1) break;
        nout++;
    }
    cJSON_Delete(arr);

    if (o == (size_t)-1) { out[0] = '\0'; return -1; }
    o = addf(out, outcap, o, "]");
    if (o == (size_t)-1) { out[0] = '\0'; return -1; }
    return (int)o;
}

/* ================= Этап 3: учёт времени (time_entries) ================= */

/* «7.17» — допустимо как число; пусто/с запятой/мусор — нет */
static int is_num_str(const char *s) {
    if (!s || !*s) return 0;
    for (const char *p = s; *p; p++)
        if (!isdigit((unsigned char)*p) && *p != '.' && *p != '-') return 0;
    return 1;
}

int sb_time_upsert(const char *month_tab, int day,
                   const char *in_t, const char *out_t,
                   const char *hours, const char *sum) {
    if (!sb_on || !month_tab || !*month_tab || day < 1 || day > 31) return 0;
    char mt[64], mi[32], mo[32];
    jesc(mt, sizeof(mt), month_tab);
    jesc(mi, sizeof(mi), in_t ? in_t : "");
    jesc(mo, sizeof(mo), out_t ? out_t : "");
    /* колонки numeric: не-число -> null (не уронит каст на PostgREST) */
    char hbuf[64] = "null", sbuf[64] = "null";
    if (is_num_str(hours)) snprintf(hbuf, sizeof(hbuf), "%s", hours);
    if (is_num_str(sum))   snprintf(sbuf, sizeof(sbuf), "%s", sum);
    char body[1024];
    snprintf(body, sizeof(body),
             "{\"month\":\"%s\",\"day\":%d,\"t_in\":\"%s\",\"t_out\":\"%s\","
             "\"hours\":%s,\"amount\":%s}",
             mt, day, mi, mo, hbuf, sbuf);
    int st = sb_call("POST", "/time_entries", body);
    if (st < 200 || st > 299)
        fprintf(stderr, "[supabase] time upsert %s day=%d -> HTTP %d\n",
                month_tab, day, st);
    return st;
}

int sb_time_days_json(const char *rows_json, char *out, size_t outcap) {
    if (!out || outcap < 8) return -1;
    out[0] = '\0';
    cJSON *arr = rows_json ? cJSON_Parse(rows_json) : NULL;
    if (!arr || !cJSON_IsArray(arr)) { cJSON_Delete(arr); return -1; }

    size_t o = addf(out, outcap, 0, "{");
    int size = cJSON_GetArraySize(arr), n = 0;
    for (int i = 0; i < size; i++) {
        cJSON *it = cJSON_GetArrayItem(arr, i);
        cJSON *jd = it ? cJSON_GetObjectItemCaseSensitive(it, "day") : NULL;
        cJSON *ji = it ? cJSON_GetObjectItemCaseSensitive(it, "t_in") : NULL;
        cJSON *jo = it ? cJSON_GetObjectItemCaseSensitive(it, "t_out") : NULL;
        cJSON *jh = it ? cJSON_GetObjectItemCaseSensitive(it, "hours") : NULL;
        cJSON *ja = it ? cJSON_GetObjectItemCaseSensitive(it, "amount") : NULL;
        if (!cJSON_IsNumber(jd)) continue;
        int day = (int)jd->valuedouble;
        if (day < 1 || day > 31) continue;

        char in_j[64], out_j[64], h_j[64], s_j[64];
        jesc(in_j, sizeof(in_j), cJSON_IsString(ji) && ji->valuestring ? ji->valuestring : "");
        jesc(out_j, sizeof(out_j), cJSON_IsString(jo) && jo->valuestring ? jo->valuestring : "");
        /* формат tracking_days_build: часы/сумма — строки, у пустых — "" */
        char hbuf[32] = "", sbuf[32] = "";
        if (cJSON_IsNumber(jh)) snprintf(hbuf, sizeof(hbuf), "%.2f", jh->valuedouble);
        if (cJSON_IsNumber(ja)) snprintf(sbuf, sizeof(sbuf), "%.2f", ja->valuedouble);
        jesc(h_j, sizeof(h_j), hbuf);
        jesc(s_j, sizeof(s_j), sbuf);
        o = addf(out, outcap, o,
                 "%s\"%d\":{\"in\":\"%s\",\"out\":\"%s\",\"hours\":\"%s\",\"sum\":\"%s\"}",
                 n ? "," : "", day, in_j, out_j, h_j, s_j);
        if (o == (size_t)-1) break;
        n++;
    }
    cJSON_Delete(arr);

    if (o == (size_t)-1) { out[0] = '\0'; return -1; }
    o = addf(out, outcap, o, "}");
    if (o == (size_t)-1) { out[0] = '\0'; return -1; }
    return 0;
}

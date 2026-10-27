#include "app/month_api.h"

#include "app/month_cache.h"
#include "app/schedule_service.h"
#include "core/json_text.h"
#include "core/shift.h"
#include "core/strbuf.h"
#include "core/url_codec.h"
#include "domain/schedule.h"
#include "infra/supabase.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Массивы s1/s2 месяца из файлового кэша (фоллбэк, когда БД недоступна). */
static int build_month_arrays_json(const MonthSchedule *m,
                                   char *s1, size_t s1cap, char *s2, size_t s2cap) {
    int c1 = m->shift1_len, c2 = m->shift2_len;
    int n = c1 > c2 ? c1 : c2;
    if (n < 1) n = 1;
    size_t o1 = 0, o2 = 0;
    for (int i = 0; i < n; i++) {
        const char *v = month_schedule_cell(m, 0, i);
        char *ev = json_escape(v);
        int w = snprintf(s1 + o1, s1cap - o1, "%s\"%s\"", i ? "," : "[", ev ? ev : v);
        free(ev);
        if (w < 0 || (size_t)w >= s1cap - o1) return -1;
        o1 += (size_t)w;
    }
    for (int i = 0; i < n; i++) {
        const char *v = month_schedule_cell(m, 1, i);
        char *ev = json_escape(v);
        int w = snprintf(s2 + o2, s2cap - o2, "%s\"%s\"", i ? "," : "[", ev ? ev : v);
        free(ev);
        if (w < 0 || (size_t)w >= s2cap - o2) return -1;
        o2 += (size_t)w;
    }
    if (o1 >= s1cap - 2 || o2 >= s2cap - 2) return -1;
    snprintf(s1 + o1, s1cap - o1, "]");
    snprintf(s2 + o2, s2cap - o2, "]");
    return 0;
}

/* Массивы s1/s2 месяца: БД (этап 2), при недоступности — файловый кэш.
 * 0 — успех (структура листа: n = max(длин строк), дней = n-3,
 * счётчик «Всего смен» — из листа). */
static int month_s1s2(const AppConfig *cfg, const MonthSchedule *m,
                      char *s1, size_t s1cap, char *s2, size_t s2cap) {
    int c1 = m->shift1_len, c2 = m->shift2_len;
    int n = c1 > c2 ? c1 : c2;
    if (n < 1) n = 1;
    char total1[64] = "";
    if (m->shift1 && c1 > 0 && m->shift1[c1 - 1])
        snprintf(total1, sizeof(total1), "%s", m->shift1[c1 - 1]);

    if (sb_enabled() && cfg->search_name && *cfg->search_name &&
        schedule_read_month_from_db(m->title, cfg->search_name, n - 3, total1,
                                    s1, s1cap, s2, s2cap) == 0)
        return 0;
    return build_month_arrays_json(m, s1, s1cap, s2, s2cap);
}

/* ---------- /api/months ---------- */

int month_api_months_json(const AppConfig *cfg, char *out, size_t cap) {
    (void)cfg;
    MonthSchedule months[SCHEDULE_MAX_MONTHS];
    memset(months, 0, sizeof(months));
    int count = month_cache_load(months, SCHEDULE_MAX_MONTHS);

    StrBuf sb;
    sbuf_init(&sb);
    sbuf_append(&sb, "{\"months\":[");
    for (int i = 0; i < count; i++) {
        if (i) sbuf_append(&sb, ",");
        sbuf_append_json_string(&sb, months[i].title);
    }
    sbuf_append(&sb, "]}");

    char *json = sbuf_take(&sb);
    month_schedules_free(months, count);
    if (!json) { snprintf(out, cap, "{}"); return -1; }
    if (strlen(json) >= cap) { free(json); snprintf(out, cap, "{}"); return -1; }
    memcpy(out, json, strlen(json) + 1);
    free(json);
    return 0;
}

/* ---------- /api/month ---------- */

int month_api_month_json(TrackingService *svc, const char *query,
                          char *out, size_t cap) {
    char *name = url_query_param(query, "name");
    if (!name || !*name) {
        free(name);
        snprintf(out, cap, "{\"error\":\"не указано имя\"}");
        return 400;
    }

    MonthSchedule months[SCHEDULE_MAX_MONTHS];
    memset(months, 0, sizeof(months));
    int count = month_cache_load(months, SCHEDULE_MAX_MONTHS);
    int idx = -1;
    for (int i = 0; i < count; i++) {
        if (strcmp(months[i].title, name) == 0) { idx = i; break; }
    }
    free(name);
    if (idx < 0) {
        month_schedules_free(months, count);
        snprintf(out, cap, "{\"error\":\"не найдено\"}");
        return 404;
    }

    MonthSchedule *m = &months[idx];

    /* Отметки Time tracking — из своего кэша (обновляется фоновым циклом,
       инвалидируется при записи — поэтому их НЕ кэшируем в mresp) */
    char trk[16384];
    int have_trk = (tracking_days_cached_json(svc, m->title, trk, sizeof(trk)) == 0);

    /* Ответ графика: кэш готового ответа -> мгновенно, без HTTP к Supabase;
       при промахе строим (Supabase -> файловый кэш) и кладём в кэш */
    char cached[16384];
    char s1txt[16384] = "";
    char s2txt[16384] = "";
    int use_cache = (month_response_read(m->title, cached, sizeof(cached)) == 0);
    if (!use_cache) {
        if (month_s1s2(svc->cfg, m, s1txt, sizeof(s1txt), s2txt, sizeof(s2txt)) != 0) {
            month_schedules_free(months, count);
            snprintf(out, cap, "{\"error\":\"нет данных месяца\"}");
            return 500;
        }
        month_response_write(m->title, s1txt, s2txt);
        fprintf(stderr, "[cache] month '%s' -> mresp (supabase/file)\n", m->title);
    }

    StrBuf sb;
    sbuf_init(&sb);
    if (use_cache) {
        /* cached = {"t":..,"s1":[..],"s2":[..]} — срезаем последнюю скобку
           и вставляем trk из своего кэша */
        size_t len = strlen(cached);
        if (len > 0) sbuf_append_len(&sb, cached, len - 1);
        sbuf_append(&sb, ",\"trk\":");
        sbuf_append(&sb, have_trk ? trk : "null");
        sbuf_append(&sb, "}");
    } else {
        sbuf_append(&sb, "{\"t\":");
        sbuf_append_json_string(&sb, m->title);
        sbuf_append(&sb, ",\"s1\":");
        sbuf_append(&sb, s1txt);
        sbuf_append(&sb, ",\"s2\":");
        sbuf_append(&sb, s2txt);
        sbuf_append(&sb, ",\"trk\":");
        sbuf_append(&sb, have_trk ? trk : "null");
        sbuf_append(&sb, "}");
    }

    char *json = sbuf_take(&sb);
    month_schedules_free(months, count);
    if (!json) { snprintf(out, cap, "{}"); return 500; }
    if (strlen(json) >= cap) { free(json); snprintf(out, cap, "{}"); return 500; }
    memcpy(out, json, strlen(json) + 1);
    free(json);
    return 200;
}

/* ---------- /api/cabinet ---------- */

int month_api_cabinet_json(TrackingService *svc, const char *query,
                            char *out, size_t cap) {
    char *month = url_query_param(query, "month");
    char *cabinet = url_query_param(query, "cabinet");
    char *day_s = url_query_param(query, "day");
    char *shift = url_query_param(query, "shift");

    if (!month || !cabinet || !day_s) {
        free(month); free(cabinet); free(day_s); free(shift);
        snprintf(out, cap, "{\"error\":\"не хватает параметров\"}");
        return 400;
    }
    int day = atoi(day_s);

    fprintf(stderr, "[cabinet] month='%s' cabinet='%s' day=%d shift='%s'\n",
            month, cabinet, day, shift ? shift : "");

    StrBuf sb;
    sbuf_init(&sb);
    sbuf_append(&sb, "{\"cabinet\":");
    sbuf_append_json_string(&sb, cabinet);
    sbuf_append(&sb, ",\"month\":");
    sbuf_append_json_string(&sb, month);
    sbuf_appendf(&sb, ",\"day\":%d,\"shift\":", day);
    sbuf_append_json_string(&sb, shift ? shift : "");
    sbuf_append(&sb, ",\"colleagues\":");

    const char *self = (svc->cfg && svc->cfg->search_name) ? svc->cfg->search_name : NULL;
    const char *sh = (shift && *shift) ? shift : NULL;

    /* Ускорение тапа по кабинету: коллеги из кэша месяца (mcol) — без HTTP;
       при промахе/устаревании — Supabase, затем индексный файл (фоллбэки
       сохранены полностью) */
    int have_colleagues = 0;
    int cache_hit = 0;
    char colleagues[65536];
    {
        char *all = malloc(512 * 1024);
        if (all) {
            if (month_colleagues_cache_read(month, all, 512 * 1024) == 0) {
                char sub[65536];
                if (month_colleagues_cache_filter(all, day, cabinet, sub, sizeof(sub)) == 0) {
                    ColleagueList list;
                    if (colleagues_from_rows_json(sub, self, sh, shift_overlap, &list) == 0 &&
                        colleague_list_render_json(&list, colleagues, sizeof(colleagues)) > 0) {
                        have_colleagues = 1;
                        cache_hit = 1;
                    }
                }
            }
            free(all);
        }
    }
    if (!have_colleagues && sb_enabled()) {
        if (schedule_read_colleagues_from_db(month, day, cabinet, sh, self,
                                             colleagues, sizeof(colleagues)) == 0) {
            have_colleagues = 1;
        }
    }
    if (!have_colleagues) {
        int n = schedule_colleagues_from_index(month, day, cabinet, sh,
                                               colleagues, sizeof(colleagues));
        if (n >= 0) have_colleagues = 1;
    }
    if (have_colleagues) sbuf_append(&sb, colleagues);
    else sbuf_append(&sb, "[]");
    sbuf_append(&sb, "}");

    char *json = sbuf_take(&sb);
    if (!json) {
        free(month); free(cabinet); free(day_s); free(shift);
        snprintf(out, cap, "{}");
        return 500;
    }
    if (strlen(json) >= cap) {
        free(json);
        free(month); free(cabinet); free(day_s); free(shift);
        snprintf(out, cap, "{}");
        return 500;
    }
    memcpy(out, json, strlen(json) + 1);
    free(json);

    /* Ответ уже отправлен — клиент не ждёт. Без кэша коллег (первый тап
       или устарел >TTL) освежаем полный массив строк месяца под следующие
       тапы: трафик пропорционален реальной активности. */
    if (cache_hit == 0 && have_colleagues)
        schedule_refresh_colleagues_cache(month);

    free(month); free(cabinet); free(day_s); free(shift);
    return 200;
}

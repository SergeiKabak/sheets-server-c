#include "domain/schedule.h"

#include "vendor/cJSON.h"
#include "core/json_text.h"
#include "core/shift.h"
#include "core/strbuf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- месяц-лист ---------- */

void month_schedule_free(MonthSchedule *m) {
    if (!m) return;
    for (int j = 0; j < m->shift1_len; j++) free(m->shift1[j]);
    for (int j = 0; j < m->shift2_len; j++) free(m->shift2[j]);
    free(m->shift1);
    free(m->shift2);
    m->shift1 = NULL;
    m->shift2 = NULL;
    m->shift1_len = 0;
    m->shift2_len = 0;
}

void month_schedules_free(MonthSchedule *months, int count) {
    if (!months) return;
    for (int i = 0; i < count; i++) month_schedule_free(&months[i]);
}

char **month_schedule_copy_row(char **src, int len) {
    char **out = malloc(sizeof(char *) * (len > 0 ? (size_t)len : 1));
    if (!out) return NULL;
    for (int i = 0; i < len; i++)
        out[i] = src[i] ? strdup(src[i]) : NULL;
    return out;
}

int month_schedule_width(const MonthSchedule *m) {
    if (!m) return 0;
    int n = m->shift1_len > m->shift2_len ? m->shift1_len : m->shift2_len;
    return n < 1 ? 1 : n;
}

const char *month_schedule_cell(const MonthSchedule *m, int row, int col) {
    if (!m) return "";
    char **cells = (row == 0) ? m->shift1 : m->shift2;
    int len = (row == 0) ? m->shift1_len : m->shift2_len;
    if (!cells || col < 0 || col >= len || !cells[col]) return "";
    return cells[col];
}

/* ---------- строки месяца из БД ---------- */

#define SCHEDULE_MAX_SHIFTS 8   /* смен у сотрудника в месяце (Mini App показывает 2) */

/* Печатает фрагмент в буфер с контролем переполнения.
 * Возвращает новый offset либо (size_t)-1. */
static size_t appendf(char *out, size_t cap, size_t off, const char *fmt, ...) {
    if (!out || off >= cap) return (size_t)-1;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out + off, cap - off, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - off) return (size_t)-1;
    return off + (size_t)n;
}

int schedule_month_arrays(const char *rows_json, const char *employee,
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
    } aggs[SCHEDULE_MAX_SHIFTS];
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
            if (na >= SCHEDULE_MAX_SHIFTS) continue;
            memset(&aggs[na], 0, sizeof(aggs[na]));
            snprintf(aggs[na].shift, sizeof(aggs[na].shift), "%s", sh);
            aggs[na].start = shift_start_minutes(sh);
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
    if (dim < 1 || dim > 31) goto cleanup;

    char em[768], sh[256], cell[768];
    char *e = json_escape(employee ? employee : "");
    snprintf(em, sizeof(em), "%s", e ? e : "");
    free(e);
    e = json_escape(aggs[0].shift);
    snprintf(sh, sizeof(sh), "%s", e ? e : "");
    free(e);

    /* s1: [имя, смена, day1..dayD, всего смен] — формат строки листа */
    size_t o = appendf(s1, s1cap, 0, "[\"%s\",\"%s\"", em, sh);
    for (int d = 1; d <= dim && o != (size_t)-1; d++) {
        const char *v = (d <= aggs[0].maxday && aggs[0].cells[d]) ? aggs[0].cells[d] : "";
        e = json_escape(v);
        snprintf(cell, sizeof(cell), "%s", e ? e : "");
        free(e);
        o = appendf(s1, s1cap, o, ",\"%s\"", cell);
    }
    if (total) {
        e = json_escape(total);
        snprintf(cell, sizeof(cell), "%s", e ? e : "");
        free(e);
        o = appendf(s1, s1cap, o, ",\"%s\"]", cell);      /* значение из листа */
    } else {
        o = appendf(s1, s1cap, o, ",\"%d\"]", aggs[0].count);   /* запасной подсчёт */
    }

    /* s2: вторая смена либо пустой массив той же длины (как без второй строки) */
    size_t o2 = 0;
    if (na >= 2) {
        e = json_escape(aggs[1].shift);
        snprintf(sh, sizeof(sh), "%s", e ? e : "");
        free(e);
        o2 = appendf(s2, s2cap, 0, "[\"\",\"%s\"", sh);
        for (int d = 1; d <= dim && o2 != (size_t)-1; d++) {
            const char *v = (d <= aggs[1].maxday && aggs[1].cells[d]) ? aggs[1].cells[d] : "";
            e = json_escape(v);
            snprintf(cell, sizeof(cell), "%s", e ? e : "");
            free(e);
            o2 = appendf(s2, s2cap, o2, ",\"%s\"", cell);
        }
        if (total) {
            o2 = appendf(s2, s2cap, o2, ",\"\"]");    /* в листе счётчик только в 1-й строке */
        } else {
            o2 = appendf(s2, s2cap, o2, ",\"%d\"]", aggs[1].count);
        }
    } else {
        o2 = appendf(s2, s2cap, 0, "[");
        for (int d = 0; d < dim + 3 && o2 != (size_t)-1; d++)
            o2 = appendf(s2, s2cap, o2, "%s\"\"", d ? "," : "");
        o2 = appendf(s2, s2cap, o2, "]");
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

/* ---------- коллеги ---------- */

void colleague_list_init(ColleagueList *list) {
    if (!list) return;
    list->count = 0;
}

int colleague_list_add(ColleagueList *list, const char *name, const char *shift) {
    if (!list || !name || !*name) return -1;
    for (int i = 0; i < list->count; i++) {
        if (!strcmp(list->items[i].name, name) &&
            !strcmp(list->items[i].shift, shift ? shift : "")) {
            return 1;   /* дубль */
        }
    }
    if (list->count >= COLLEAGUE_MAX) return -1;
    snprintf(list->items[list->count].name, sizeof(list->items[0].name), "%s", name);
    snprintf(list->items[list->count].shift, sizeof(list->items[0].shift), "%s",
             shift ? shift : "");
    list->count++;
    return 0;
}

int colleague_list_render_json(const ColleagueList *list, char *out, size_t outcap) {
    if (!out || outcap < 4) return -1;
    out[0] = '\0';
    size_t o = appendf(out, outcap, 0, "[");
    for (int i = 0; list && i < list->count && o != (size_t)-1; i++) {
        char *en = json_escape(list->items[i].name);
        char *es = json_escape(list->items[i].shift);
        o = appendf(out, outcap, o, "%s{\"name\":\"%s\",\"shift\":\"%s\"}",
                    i ? "," : "", en ? en : "", es ? es : "");
        free(en);
        free(es);
    }
    if (o == (size_t)-1) { out[0] = '\0'; return -1; }
    o = appendf(out, outcap, o, "]");
    if (o == (size_t)-1) { out[0] = '\0'; return -1; }
    return (int)o;
}

int colleagues_from_rows_json(const char *rows_json, const char *self,
                              const char *shift, ShiftFilter filter,
                              ColleagueList *out) {
    if (!out) return -1;
    colleague_list_init(out);

    cJSON *arr = rows_json ? cJSON_Parse(rows_json) : NULL;
    if (!arr || !cJSON_IsArray(arr)) { cJSON_Delete(arr); return -1; }

    int size = cJSON_GetArraySize(arr);
    for (int i = 0; i < size; i++) {
        cJSON *it = cJSON_GetArrayItem(arr, i);
        cJSON *je = it ? cJSON_GetObjectItemCaseSensitive(it, "employee") : NULL;
        cJSON *js = it ? cJSON_GetObjectItemCaseSensitive(it, "shift_time") : NULL;
        if (!cJSON_IsString(je) || !je->valuestring || !je->valuestring[0]) continue;
        const char *name = je->valuestring;
        const char *s = (cJSON_IsString(js) && js->valuestring) ? js->valuestring : "";

        if (self && *self && !strcmp(name, self)) continue;          /* сам сотрудник */
        if (shift && *shift && filter && !filter(s, shift)) continue; /* пересечение смен */

        if (colleague_list_add(out, name, s) < 0) break;
    }
    cJSON_Delete(arr);
    return 0;
}

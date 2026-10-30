/* app/tracking_service.c — сценарии учёта рабочего времени.
 *
 * Структура листа (вкладка = текущий месяц и год, например "Октябрь 2026"):
 *   A: Дата | B: Время прихода | C: Время ухода | D: Время в часах
 *   E: Ставка в час | F: Сумма за день
 * Блоки разных сотрудников разделены пустой строкой.
 *
 * Запись выполняется через OAuth2 (Service Account), т.к. Google Sheets API
 * запрещает изменение данных по API key.
 */
#include "app/tracking_service.h"

#include "app/tracking_cache.h"
#include "vendor/cJSON.h"
#include "core/date_time.h"
#include "core/json_text.h"
#include "core/num.h"
#include "core/strbuf.h"
#include "core/url_codec.h"
#include "domain/time_entry.h"
#include "domain/timesheet.h"
#include "infra/clock.h"
#include "infra/http_client.h"
#include "infra/supabase.h"

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ================================================================
 *  Служебные функции
 * ================================================================ */

static void trk_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[track] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

/* Округление до двух знаков — так же, как печатает num_format2:
   в расчёте участвует ровно та ставка, что видна пользователю. */
static double round2(double v) {
    char b[32];
    num_format2(v, b, sizeof(b));
    return num_parse(b);
}

/* Ставка за час для месяца: явная из month_rates, иначе HOURLY_RATE.
 * explicit_out (может быть NULL) = 1, если для месяца ставка задана явно
 * (тогда её можно перезаписывать в колонку E листа). */
static double month_rate_ex(const TrackingService *svc, const char *tab,
                            int *explicit_out) {
    double r = 0;
    int ex = 0;
    if (svc && tab && *tab && sb_enabled() &&
        sb_month_rate_get(tab, &r) == 0 && r > 0)
        ex = 1;
    if (explicit_out) *explicit_out = ex;
    if (ex) return round2(r);
    return svc ? round2(svc->rate) : 0.0;
}

/* Имя вкладки по названию месяца ("Октябрь 2026" -> "10.2026").
 * Возвращает 0 при успехе. */
static int month_tab_and_parts(TrackingService *svc, const char *month,
                                char *tab, size_t tabcap, int *pmon, int *pyear) {
    if (!month || !*month) return -1;
    char numeric[64];
    if (month_numeric_from_label(month, clock_current_year(svc->tz_offset),
                                 numeric, sizeof(numeric)) != 0) return -1;
    int mon = 0, year = 0;
    if (month_parse(numeric, &mon, &year) != 0) return -1;
    snprintf(tab, tabcap, "%02d.%04d", mon, year);
    if (pmon) *pmon = mon;
    if (pyear) *pyear = year;
    return 0;
}

/* ================================================================
 *  Работа с листом учёта времени
 * ================================================================ */

/* Читает лист месяца; если листа нет — создаёт (с заголовками). */
static int load_or_create_month_sheet(TrackingService *svc, const char *tab,
                                       SheetRows *rows) {
    char *resp = google_sheets_get_values(&svc->sheets, tab);
    if (resp) {
        sheet_rows_parse_values_json(rows, resp);
        free(resp);
    } else {
        /* листа нет — создаём и перечитываем (пустой лист) */
        if (google_sheets_create_sheet(&svc->sheets, tab) != 0) return -1;
        char *r2 = google_sheets_get_values(&svc->sheets, tab);
        if (r2) { sheet_rows_parse_values_json(rows, r2); free(r2); }
    }

    /* Если лист пустой (или состоит только из заголовка) — добавляем/проверяем
       строку заголовков колонок. */
    int need_header = 1;
    if (rows->count > 0) {
        const char *a = sheet_row_get(&rows->row[0], 0);
        if (a && strcmp(a, "Дата") == 0) need_header = 0;
    }
    if (need_header) {
        sheet_rows_insert(rows, rows->count);
        SheetRow *h = &rows->row[rows->count - 1];
        for (int c = 0; c < SHEET_COLS; c++)
            sheet_row_set(h, c, SHEET_HEADERS[c]);
    }
    return 0;
}

/* Полностью перезаписывает лист указанными строками (A1:F{n}). */
static int write_rows(TrackingService *svc, const char *tab, SheetRows *rows) {
    if (rows->count == 0) return 0;

    /* Сортируем даты внутри блоков сотрудников, чтобы в таблице
       отметки шли по порядку (01.09, 17.09, 30.09). */
    sheet_rows_sort_dates(rows);

    char *body = sheet_rows_to_body_json(rows);
    if (!body) return -1;
    char suffix[64];
    snprintf(suffix, sizeof(suffix), "!A1:F%d", rows->count);
    int rc = google_sheets_update_values(&svc->sheets, tab, suffix, body);
    free(body);
    return rc;
}

/* ================================================================
 *  Отметки времени: БД -> кэш -> Google
 * ================================================================ */

/* Ответ дней из time_entries (БД — первоисточник).
 * 0 — успех, -1 — нет данных/БД недоступна (уходим в фоллбэк Google). */
static int tracking_days_from_db(TrackingService *svc, const char *month,
                                 char *out, size_t cap) {
    if (!sb_enabled() || !out || cap < 128) return -1;
    char tab[64];
    int pmon = 0, pyear = 0;
    if (month_tab_and_parts(svc, month, tab, sizeof(tab), &pmon, &pyear) != 0)
        return -1;

    char *et = url_encode(tab);
    if (!et) return -1;
    char path[512];
    snprintf(path, sizeof(path),
             "/time_entries?month=eq.%s&select=day,t_in,t_out,hours,amount&order=day",
             et);
    free(et);

    int st = 0;
    char *body = sb_get(path, &st);
    if (!body || st < 200 || st > 300) { free(body); return -1; }
    char days[16384];
    int ok = (time_entry_days_json(body, days, sizeof(days)) == 0);
    free(body);
    if (!ok) return -1;

    char today[32], label[128];
    clock_datetime(svc->tz_offset, today, sizeof(today), NULL, 0);
    tracking_month_label(tab, label, sizeof(label));
    char rate_buf[32];
    num_format2(month_rate_ex(svc, tab, NULL), rate_buf, sizeof(rate_buf));
    char *em = json_escape(tab);
    char *el = json_escape(label);
    int n = snprintf(out, cap,
                     "{\"ok\":true,\"month\":\"%s\",\"label\":\"%s\","
                     "\"today\":\"%s\",\"rate\":\"%s\",\"days\":%s}",
                     em ? em : "", el ? el : "", today, rate_buf, days);
    free(em);
    free(el);
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

/* Строит ответ дней напрямую из Google Sheets (без кэша и БД). */
static int tracking_days_build(TrackingService *svc, const char *month,
                               char *out, size_t cap) {
    if (!out || cap < 128) return 500;
    out[0] = '\0';

    char tab[64];
    int pmon = 0, pyear = 0;
    if (month_tab_and_parts(svc, month, tab, sizeof(tab), &pmon, &pyear) != 0) {
        snprintf(out, cap, "{\"ok\":false,\"error\":\"неверный месяц\"}");
        return 400;
    }

    SheetRows rows;
    sheet_rows_init(&rows);
    char *resp = google_sheets_get_values(&svc->sheets, tab);
    if (resp) {
        sheet_rows_parse_values_json(&rows, resp);
        free(resp);
    }

    char today[32];
    clock_datetime(svc->tz_offset, today, sizeof(today), NULL, 0);

    const char *name = (svc->cfg && svc->cfg->search_name) ? svc->cfg->search_name : NULL;
    int block = name ? sheet_find_employee(&rows, name) : -1;

    char label[128];
    tracking_month_label(tab, label, sizeof(label));
    char rate_buf[32];
    num_format2(month_rate_ex(svc, tab, NULL), rate_buf, sizeof(rate_buf));

    StrBuf sb;
    sbuf_init(&sb);
    sbuf_append(&sb, "{\"ok\":true,\"month\":");
    sbuf_append_json_string(&sb, tab);
    sbuf_append(&sb, ",\"label\":");
    sbuf_append_json_string(&sb, label);
    sbuf_append(&sb, ",\"today\":");
    sbuf_append_json_string(&sb, today);
    sbuf_append(&sb, ",\"rate\":");
    sbuf_append_json_string(&sb, rate_buf);
    sbuf_append(&sb, ",\"days\":{");

    int first = 1;
    if (block >= 0) {
        for (int i = block + 1; i < rows.count; i++) {
            if (sheet_row_is_empty(&rows.row[i])) break;
            const char *a = sheet_row_get(&rows.row[i], 0);
            if (!*a) continue;

            char nd[64];
            date_normalize(a, nd, sizeof(nd));
            int dd = 0, mm = 0, yy = 0;
            if (date_parse(nd, &dd, &mm, &yy) != 0) continue;
            if (mm != pmon || yy != pyear) continue;
            if (dd < 1 || dd > days_in_month(pmon, pyear)) continue;

            const char *tin   = sheet_row_get(&rows.row[i], 1);
            const char *tout  = sheet_row_get(&rows.row[i], 2);
            const char *hours = sheet_row_get(&rows.row[i], 3);
            const char *sum   = sheet_row_get(&rows.row[i], 5);
            if (!*tin && !*tout && !*hours && !*sum) continue;

            sbuf_appendf(&sb, "%s\"%d\":{", first ? "" : ",", dd);
            sbuf_append(&sb, "\"in\":");
            sbuf_append_json_string(&sb, tin);
            sbuf_append(&sb, ",\"out\":");
            sbuf_append_json_string(&sb, tout);
            sbuf_append(&sb, ",\"hours\":");
            sbuf_append_json_string(&sb, hours);
            sbuf_append(&sb, ",\"sum\":");
            sbuf_append_json_string(&sb, sum);
            sbuf_append(&sb, "}");
            first = 0;
        }
    }
    sbuf_append(&sb, "}}");

    char *json = sbuf_take(&sb);
    sheet_rows_free(&rows);
    if (!json) {
        snprintf(out, cap, "{\"ok\":false,\"error\":\"не удалось собрать ответ\"}");
        return 500;
    }
    if (strlen(json) >= cap) {
        free(json);
        snprintf(out, cap, "{\"ok\":false,\"error\":\"ответ не поместился\"}");
        return 500;
    }
    memcpy(out, json, strlen(json) + 1);
    free(json);
    return 200;
}

/* Ответ для месяца, вкладки которого ещё нет: все дни пустые. */
static int tracking_days_empty(TrackingService *svc, const char *month,
                               char *out, size_t cap) {
    char tab[64];
    int pmon = 0, pyear = 0;
    if (month_tab_and_parts(svc, month, tab, sizeof(tab), &pmon, &pyear) != 0) {
        snprintf(out, cap, "{\"ok\":false,\"error\":\"неверный месяц\"}");
        return 400;
    }
    char today[32], label[128];
    clock_datetime(svc->tz_offset, today, sizeof(today), NULL, 0);
    tracking_month_label(tab, label, sizeof(label));
    char rate_buf[32];
    num_format2(month_rate_ex(svc, tab, NULL), rate_buf, sizeof(rate_buf));
    char *em = json_escape(tab);
    char *el = json_escape(label);
    int n = snprintf(out, cap,
        "{\"ok\":true,\"month\":\"%s\",\"label\":\"%s\","
        "\"today\":\"%s\",\"rate\":\"%s\",\"days\":{}}",
        em ? em : "", el ? el : "", today, rate_buf);
    free(em); free(el);
    return (n > 0 && (size_t)n < cap) ? 200 : 500;
}

int tracking_days_json(TrackingService *svc, const char *month, char *out, size_t cap) {
    if (!out || cap < 128) return 500;
    out[0] = '\0';

    char tab[64];
    int pmon = 0, pyear = 0;
    if (month_tab_and_parts(svc, month, tab, sizeof(tab), &pmon, &pyear) != 0) {
        snprintf(out, cap, "{\"ok\":false,\"error\":\"неверный месяц\"}");
        return 400;
    }
    long t0 = clock_monotonic_ms();
    /* 1) свежий локальный кэш (обновляется из БД фоном и после записи) */
    if (trk_cache_read(tab, out, cap, TRK_CACHE_TTL) == 0) {
        fprintf(stderr, "[trk] days %s: cache %ldms\n", tab, clock_monotonic_ms() - t0);
        return 200;
    }
    /* 2) БД — первоисточник (правки дашборда видны сразу) */
    if (tracking_days_from_db(svc, month, out, cap) == 0) {
        trk_cache_write(tab, out);
        fprintf(stderr, "[trk] days %s: db %ldms\n", tab, clock_monotonic_ms() - t0);
        return 200;
    }
    /* 3) фоллбэк: Google Sheets */
    int code = tracking_days_build(svc, month, out, cap);
    if (code == 200) trk_cache_write(tab, out);
    fprintf(stderr, "[trk] days %s: google %ldms code=%d\n",
            tab, clock_monotonic_ms() - t0, code);
    return code;
}

int tracking_days_cached_json(TrackingService *svc, const char *month,
                               char *out, size_t cap) {
    char tab[64];
    int pmon = 0, pyear = 0;
    if (!out || cap < 128) return -1;
    if (month_tab_and_parts(svc, month, tab, sizeof(tab), &pmon, &pyear) != 0) return -1;
    return trk_cache_read(tab, out, cap, TRK_CACHE_TTL);
}

int tracking_cache_refresh(TrackingService *svc, const char *const *months, int count) {
    if (!months || count <= 0) return 0;
    const char *sa = getenv("GOOGLE_SERVICE_ACCOUNT_JSON");
    if (!sa || !*sa) return 0;

    if (!svc->sheets.sheet_id) {
        const char *id = getenv("TRACKING_SHEET_ID");
        int has_cached_id = (access("/tmp/tracking_sheet_id", R_OK) == 0);
        if ((!id || !*id) && !has_cached_id) return 0;
        if (google_sheets_ensure(&svc->sheets) != 0) return -1;
    }

    /* Один раз получаем список существующих вкладок: для месяцев без
       вкладки не делаем лишний запрос к Google (и не пишем 404 в лог). */
    char **tabs = NULL;
    int tabc = 0;
    int have_tabs = (tracking_list_months(svc, &tabs, &tabc, 64) == 0);

    char out[16384];
    int ok = 0;
    int lim = count < TRACKING_MAX_MONTHS ? count : TRACKING_MAX_MONTHS;
    for (int i = 0; i < lim; i++) {
        if (!months[i] || !*months[i]) continue;
        char tab[64];
        int pmon = 0, pyear = 0;
        if (month_tab_and_parts(svc, months[i], tab, sizeof(tab), &pmon, &pyear) != 0)
            continue;

        int exists = 0;
        if (have_tabs) {
            for (int j = 0; j < tabc; j++)
                if (tabs[j] && strcmp(tabs[j], tab) == 0) { exists = 1; break; }
        }
        /* Кэш дней строим из БД (первоисточник): правки в дашборде
           отражаются в Mini App в течение 60 с. Google — только фоллбэк. */
        int code = tracking_days_from_db(svc, months[i], out, sizeof(out)) == 0
                 ? 200
                 : ((have_tabs && !exists)
                        ? tracking_days_empty(svc, months[i], out, sizeof(out))
                        : tracking_days_build(svc, months[i], out, sizeof(out)));
        if (code != 200) continue;
        if (trk_cache_write(tab, out) == 0) ok++;
    }
    if (tabs) {
        for (int j = 0; j < tabc; j++) free(tabs[j]);
        free(tabs);
    }
    trk_log("cache refreshed: %d/%d months", ok, lim);
    return ok;
}

/* ================================================================
 *  Запись времени
 * ================================================================ */

/* Write-through: после УСПЕШНОЙ записи дублируем день в БД.
   Ошибка БД не меняет ответ клиенту — только лог. */
static void sb_write_time_day(const char *tab, int day,
                               const char *in_t, const char *out_t,
                               const char *hours, const char *sum) {
    if (!sb_enabled()) return;
    sb_time_upsert(tab, day, in_t, out_t, hours, sum);
}

int tracking_do_clock(TrackingService *svc, const char *action, char *out, size_t cap) {
    if (!action || !*action) {
        snprintf(out, cap, "{\"ok\":false,\"error\":\"не указано действие\"}");
        return 400;
    }
    int is_in = (strcmp(action, "in") == 0);
    int is_out = (strcmp(action, "out") == 0);
    if (!is_in && !is_out) {
        snprintf(out, cap, "{\"ok\":false,\"error\":\"неизвестное действие\"}");
        return 400;
    }

    const char *sn = svc->cfg ? svc->cfg->search_name : NULL;
    char *name = (sn && *sn) ? strdup(sn) : strdup("Сотрудник");
    if (!name) {
        snprintf(out, cap, "{\"ok\":false,\"error\":\"не указано имя сотрудника\"}");
        return 500;
    }

    /* Время с округлением в меньшую сторону до целых минут */
    char date[32], time_s[16];
    clock_datetime(svc->tz_offset, date, sizeof(date), time_s, sizeof(time_s));
    clock_month_tab(svc->tz_offset, svc->current_tab, sizeof(svc->current_tab));
    int day = atoi(date);   /* "05.10.2026" -> 5 */

    int rate_explicit = 0;
    double rate = month_rate_ex(svc, svc->current_tab, &rate_explicit);

    /* ==== БД — первоисточник: запись сразу в time_entries ==== */
    if (sb_enabled()) {
        char d_in[16] = "", d_out[16] = "";
        int rd = sb_time_day_read(svc->current_tab, day,
                                  d_in, sizeof(d_in), d_out, sizeof(d_out),
                                  NULL, 0, NULL, 0);
        if (is_in && rd == 0 && d_in[0]) {
            char *e = json_escape("Уже отмечен приход сегодня");
            snprintf(out, cap, "{\"ok\":false,\"error\":\"%s\",\"time\":\"%s\"}",
                     e ? e : "", time_s);
            free(e);
            free(name);
            return 409;
        }
        if (is_out && (rd != 0 || !d_in[0])) {
            snprintf(out, cap, "{\"ok\":false,\"error\":\"сегодня не отмечен приход\"}");
            free(name);
            return 409;
        }
        if (is_out && d_out[0]) {
            snprintf(out, cap, "{\"ok\":false,\"error\":\"уход уже отмечен сегодня\"}");
            free(name);
            return 409;
        }
        if (rd >= 0) {
            int ok = 0;
            char hours_buf[32] = "", sum_buf[32] = "";
            if (is_in) {
                int st = sb_time_upsert(svc->current_tab, day, time_s, "", "", "");
                ok = (st >= 200 && st < 300);
            } else {
                double hours = time_entry_hours(d_in, time_s);
                num_format2(hours, hours_buf, sizeof(hours_buf));
                num_format2(time_entry_sum(hours_buf, rate),
                            sum_buf, sizeof(sum_buf));
                int st = sb_time_upsert(svc->current_tab, day, d_in, time_s,
                                        hours_buf, sum_buf);
                ok = (st >= 200 && st < 300);
            }
            if (ok) {
                trk_dirty_put(svc->current_tab);
                trk_cache_invalidate(svc->current_tab);
                char *en = json_escape(name);
                if (is_in) {
                    snprintf(out, cap,
                             "{\"ok\":true,\"action\":\"in\",\"name\":\"%s\","
                             "\"date\":\"%s\",\"time\":\"%s\"}",
                             en ? en : "", date, time_s);
                    trk_log("CLOCK IN(db): %s date=%s time=%s", name, date, time_s);
                } else {
                    snprintf(out, cap,
                             "{\"ok\":true,\"action\":\"out\",\"name\":\"%s\","
                             "\"date\":\"%s\",\"time\":\"%s\","
                             "\"hours\":\"%s\",\"sum\":\"%s\"}",
                             en ? en : "", date, time_s, hours_buf, sum_buf);
                    trk_log("CLOCK OUT(db): %s date=%s time=%s hours=%s sum=%s",
                            name, date, time_s, hours_buf, sum_buf);
                }
                free(en);
                free(name);
                return 200;
            }
            trk_log("clock: БД недоступна, фоллбэк Google");
        } else {
            trk_log("clock: ошибка чтения БД, фоллбэк Google");
        }
        /* фоллбэк: недоступна БД — прежний путь через Google */
    }

    /* Google — фоллбэк: ключ и таблица (как раньше) */
    if (!svc->sheets.sheet_id) {
        if (google_sheets_ensure(&svc->sheets) != 0) {
            char *e = json_escape(svc->sheets.last_error[0] ? svc->sheets.last_error
                                                             : "нет доступа к таблице");
            snprintf(out, cap, "{\"ok\":false,\"error\":\"%s\"}",
                     e ? e : "нет доступа к таблице");
            free(e);
            free(name);
            return 500;
        }
    }

    SheetRows rows;
    sheet_rows_init(&rows);
    if (load_or_create_month_sheet(svc, svc->current_tab, &rows) != 0) {
        sheet_rows_free(&rows);
        free(name);
        char *e = json_escape(svc->sheets.last_error[0] ? svc->sheets.last_error
                                                         : "нет доступа к таблице");
        snprintf(out, cap, "{\"ok\":false,\"error\":\"%s\"}",
                 e ? e : "нет доступа к таблице");
        free(e);
        return 500;
    }

    int block = sheet_find_employee(&rows, name);
    int date_row = (block >= 0) ? sheet_find_date_row(&rows, block, date) : -1;
    int code = 200;

    if (is_in) {
        if (date_row >= 0 && *sheet_row_get(&rows.row[date_row], 1)) {
            char *e = json_escape("Уже отмечен приход сегодня");
            snprintf(out, cap, "{\"ok\":false,\"error\":\"%s\",\"time\":\"%s\"}",
                     e ? e : "", time_s);
            free(e);
            code = 409;
            goto finish;
        }
        char rate_buf[32];
        num_format2(rate, rate_buf, sizeof(rate_buf));

        if (date_row >= 0) {
            sheet_row_set(&rows.row[date_row], 1, time_s);
            if (rate_explicit || !*sheet_row_get(&rows.row[date_row], 4))
                sheet_row_set(&rows.row[date_row], 4, rate_buf);
        } else if (block >= 0) {
            int last = sheet_find_block_end(&rows, block);
            sheet_rows_insert(&rows, last + 1);
            int ni = last + 1;
            sheet_row_set(&rows.row[ni], 0, date);
            sheet_row_set(&rows.row[ni], 1, time_s);
            sheet_row_set(&rows.row[ni], 4, rate_buf);
        } else {
            sheet_rows_insert(&rows, rows.count);
            sheet_rows_insert(&rows, rows.count);
            int ni = rows.count - 1;
            sheet_row_set(&rows.row[ni], 0, name);
            sheet_rows_insert(&rows, rows.count);
            int di = rows.count - 1;
            sheet_row_set(&rows.row[di], 0, date);
            sheet_row_set(&rows.row[di], 1, time_s);
            sheet_row_set(&rows.row[di], 4, rate_buf);
        }

        if (write_rows(svc, svc->current_tab, &rows) != 0) {
            snprintf(out, cap, "{\"ok\":false,\"error\":\"не удалось записать в таблицу\"}");
            code = 500;
            goto finish;
        }
        char *en = json_escape(name);
        snprintf(out, cap,
            "{\"ok\":true,\"action\":\"in\",\"name\":\"%s\",\"date\":\"%s\",\"time\":\"%s\"}",
            en ? en : "", date, time_s);
        free(en);
        trk_log("CLOCK IN: %s date=%s time=%s", name, date, time_s);
        sb_write_time_day(svc->current_tab, atoi(date), time_s, "", "", "");

    } else {
        if (date_row < 0 || !*sheet_row_get(&rows.row[date_row], 1)) {
            snprintf(out, cap, "{\"ok\":false,\"error\":\"сегодня не отмечен приход\"}");
            code = 409;
            goto finish;
        }
        if (*sheet_row_get(&rows.row[date_row], 2)) {
            snprintf(out, cap, "{\"ok\":false,\"error\":\"уход уже отмечен сегодня\"}");
            code = 409;
            goto finish;
        }
        sheet_row_set(&rows.row[date_row], 2, time_s);

        double hours = time_entry_hours(sheet_row_get(&rows.row[date_row], 1), time_s);
        char hours_buf[32], sum_buf[32];
        num_format2(hours, hours_buf, sizeof(hours_buf));
        /* Явная ставка месяца важнее колонки E; иначе — прежнее поведение
           (ставка из строки листа, а если её нет — HOURLY_RATE). */
        double day_rate = rate;
        if (!rate_explicit) {
            double sheet_rate = num_parse(sheet_row_get(&rows.row[date_row], 4));
            if (sheet_rate > 0) day_rate = sheet_rate;
        }
        /* Считаем сумму из тех же округлённых часов, что показываем пользователю,
           чтобы не было расхождения (например, 5.02 × 14.2 = 71.28, а не 71.24). */
        num_format2(time_entry_sum(hours_buf, day_rate), sum_buf, sizeof(sum_buf));

        /* Статичные значения вместо формул: */
        sheet_row_set(&rows.row[date_row], 3, hours_buf);   /* D — часы */
        sheet_row_set(&rows.row[date_row], 5, sum_buf);     /* F — сумма */
        if (rate_explicit || !*sheet_row_get(&rows.row[date_row], 4)) {
            char rb[32];
            num_format2(day_rate, rb, sizeof(rb));
            sheet_row_set(&rows.row[date_row], 4, rb);
        }

        if (write_rows(svc, svc->current_tab, &rows) != 0) {
            snprintf(out, cap, "{\"ok\":false,\"error\":\"не удалось записать в таблицу\"}");
            code = 500;
            goto finish;
        }
        char *en = json_escape(name);
        snprintf(out, cap,
            "{\"ok\":true,\"action\":\"out\",\"name\":\"%s\",\"date\":\"%s\","
            "\"time\":\"%s\",\"hours\":\"%s\",\"sum\":\"%s\"}",
            en ? en : "", date, time_s, hours_buf, sum_buf);
        free(en);
        trk_log("CLOCK OUT: %s date=%s time=%s hours=%s sum=%s",
                name, date, time_s, hours_buf, sum_buf);
        sb_write_time_day(svc->current_tab, atoi(date),
                          sheet_row_get(&rows.row[date_row], 1),
                          time_s, hours_buf, sum_buf);
    }

finish:
    if (code == 200 && svc->current_tab[0]) trk_cache_invalidate(svc->current_tab);
    sheet_rows_free(&rows);
    free(name);
    return code;
}

int tracking_fill_day(TrackingService *svc, const char *month, int day,
                       const char *in_time, const char *out_time,
                       char *out, size_t cap) {
    if (!out || cap < 128) return 500;
    out[0] = '\0';

    if (!svc->sheets.sheet_id && google_sheets_ensure(&svc->sheets) != 0) {
        char *e = json_escape(svc->sheets.last_error[0] ? svc->sheets.last_error
                                                         : "нет доступа к таблице");
        snprintf(out, cap, "{\"ok\":false,\"error\":\"%s\"}", e ? e : "");
        free(e);
        return 500;
    }

    char tab[64];
    int pmon = 0, pyear = 0;
    if (month_tab_and_parts(svc, month, tab, sizeof(tab), &pmon, &pyear) != 0) {
        snprintf(out, cap, "{\"ok\":false,\"error\":\"неверный месяц\"}");
        return 400;
    }
    if (day < 1 || day > days_in_month(pmon, pyear)) {
        snprintf(out, cap, "{\"ok\":false,\"error\":\"неверный день месяца\"}");
        return 400;
    }
    if (!in_time || !*in_time || !out_time || !*out_time) {
        snprintf(out, cap,
                 "{\"ok\":false,\"error\":\"укажите время прихода и ухода\"}");
        return 400;
    }

    char norm_in[16], norm_out[16];
    time_normalize(in_time, norm_in, sizeof(norm_in));
    time_normalize(out_time, norm_out, sizeof(norm_out));
    int in_min  = time_parse(norm_in);
    int out_min = time_parse(norm_out);
    if (in_min < 0 || out_min < 0) {
        snprintf(out, cap,
                 "{\"ok\":false,\"error\":\"неверное время, нужно ЧЧ:ММ\"}");
        return 400;
    }

    char date[32];
    snprintf(date, sizeof(date), "%02d.%02d.%04d", day, pmon, pyear);

    /* Не даём отметить будущие дни: в таблице учитывается только
       уже отработанное время. */
    char today[32];
    clock_datetime(svc->tz_offset, today, sizeof(today), NULL, 0);
    int td = 0, tm = 0, ty = 0;
    if (date_parse(today, &td, &tm, &ty) == 0) {
        int future = (pyear > ty) ||
                     (pyear == ty && pmon > tm) ||
                     (pyear == ty && pmon == tm && day > td);
        if (future) {
            snprintf(out, cap,
                     "{\"ok\":false,\"error\":\"нельзя отметить будущую дату\"}");
            return 400;
        }
    }

    const char *sn = svc->cfg ? svc->cfg->search_name : NULL;
    char *name = (sn && *sn) ? strdup(sn) : strdup("Сотрудник");
    if (!name) {
        snprintf(out, cap, "{\"ok\":false,\"error\":\"не указано имя сотрудника\"}");
        return 500;
    }

    int rate_explicit = 0;
    double rate = month_rate_ex(svc, tab, &rate_explicit);

    SheetRows rows;
    sheet_rows_init(&rows);
    int code = 200;

    /* ==== БД — первоисточник: запись/ПРАВКА сразу в time_entries ==== */
    if (sb_enabled()) {
        /* ставка месяца — из month_rates, иначе HOURLY_RATE */
        double hours = (out_min - in_min) / 60.0;
        if (hours < 0) hours += 24.0;
        char hours_buf[32], sum_buf[32];
        num_format2(hours, hours_buf, sizeof(hours_buf));
        num_format2(time_entry_sum(hours_buf, rate),
                    sum_buf, sizeof(sum_buf));

        /* UPSERT перезаписывает существующий день — это и есть правка
           (409 «уже отмечено» убран: повторный submit — осознанное
           редактирование, а не ошибка) */
        int st = sb_time_upsert(tab, day, norm_in, norm_out, hours_buf, sum_buf);
        if (st >= 200 && st < 300) {
            trk_dirty_put(tab);                 /* выгрузка в Google — фоном */
            trk_cache_invalidate(tab);          /* следующий days уйдёт в БД */
            char *en = json_escape(name);
            snprintf(out, cap,
                     "{\"ok\":true,\"name\":\"%s\",\"month\":\"%s\",\"date\":\"%s\","
                     "\"day\":%d,\"in\":\"%s\",\"out\":\"%s\","
                     "\"hours\":\"%s\",\"sum\":\"%s\"}",
                     en ? en : "", tab, date, day, norm_in, norm_out,
                     hours_buf, sum_buf);
            free(en);
            trk_log("FILL(db): %s date=%s in=%s out=%s hours=%s sum=%s",
                    name, date, norm_in, norm_out, hours_buf, sum_buf);
            free(name);
            return 200;
        }
        trk_log("fill: БД недоступна, фоллбэк Google (tab=%s day=%d)", tab, day);
        /* фоллбэк: недоступна БД — прежний путь через Google */
    }

    if (!svc->sheets.sheet_id && google_sheets_ensure(&svc->sheets) != 0) {
        char *e = json_escape(svc->sheets.last_error[0] ? svc->sheets.last_error
                                                         : "нет доступа к таблице");
        snprintf(out, cap, "{\"ok\":false,\"error\":\"%s\"}", e ? e : "");
        free(e);
        free(name);
        return 500;
    }

    /* Если вкладки месяца ещё нет — load_or_create_month_sheet() создаст
       её с именем MM.YYYY (например, "10.2026" для октября 2026). */
    if (load_or_create_month_sheet(svc, tab, &rows) != 0) {
        char *e = json_escape(svc->sheets.last_error[0] ? svc->sheets.last_error
                                                         : "не удалось открыть вкладку месяца");
        snprintf(out, cap, "{\"ok\":false,\"error\":\"%s\"}", e ? e : "");
        free(e);
        code = 500;
        goto fill_finish;
    }

    int block = sheet_find_employee(&rows, name);
    int row = (block >= 0) ? sheet_find_date_row(&rows, block, date) : -1;

    if (row < 0) {
        if (block >= 0) {
            int last = sheet_find_block_end(&rows, block);
            sheet_rows_insert(&rows, last + 1);
            row = last + 1;
        } else {
            /* Пустая строка-разделитель, затем строка с ФИО, затем день. */
            sheet_rows_insert(&rows, rows.count);
            sheet_rows_insert(&rows, rows.count);
            int ni = rows.count - 1;
            sheet_row_set(&rows.row[ni], 0, name);
            sheet_rows_insert(&rows, rows.count);
            row = rows.count - 1;
        }
        sheet_row_set(&rows.row[row], 0, date);
    }

    sheet_row_set(&rows.row[row], 1, norm_in);
    sheet_row_set(&rows.row[row], 2, norm_out);

    char rate_buf[32];
    double day_rate = rate;
    if (!rate_explicit) {
        const char *sheet_rate = sheet_row_get(&rows.row[row], 4);
        if (sheet_rate && *sheet_rate) {
            double parsed = num_parse(sheet_rate);
            if (parsed > 0) day_rate = parsed;
        }
    }
    num_format2(day_rate, rate_buf, sizeof(rate_buf));
    if (rate_explicit || !*sheet_row_get(&rows.row[row], 4))
        sheet_row_set(&rows.row[row], 4, rate_buf);

    double hours = (out_min - in_min) / 60.0;
    if (hours < 0) hours += 24.0;
    char hours_buf[32], sum_buf[32];
    num_format2(hours, hours_buf, sizeof(hours_buf));
    /* Сумма из округлённых часов — как в «Пришла/Ушла», без расхождений. */
    num_format2(time_entry_sum(hours_buf, day_rate), sum_buf, sizeof(sum_buf));

    sheet_row_set(&rows.row[row], 3, hours_buf);
    sheet_row_set(&rows.row[row], 5, sum_buf);

    if (write_rows(svc, tab, &rows) != 0) {
        snprintf(out, cap,
                 "{\"ok\":false,\"error\":\"не удалось записать в таблицу\"}");
        code = 500;
        goto fill_finish;
    }

    char *en = json_escape(name);
    snprintf(out, cap,
             "{\"ok\":true,\"name\":\"%s\",\"month\":\"%s\",\"date\":\"%s\","
             "\"day\":%d,\"in\":\"%s\",\"out\":\"%s\","
             "\"hours\":\"%s\",\"sum\":\"%s\"}",
             en ? en : "", tab, date, day, norm_in, norm_out, hours_buf, sum_buf);
    free(en);
    trk_log("FILL: %s date=%s in=%s out=%s hours=%s sum=%s",
            name, date, norm_in, norm_out, hours_buf, sum_buf);
    sb_write_time_day(tab, day, norm_in, norm_out, hours_buf, sum_buf);
    code = 200;

fill_finish:
    if (code == 200) trk_cache_invalidate(tab);
    sheet_rows_free(&rows);
    free(name);
    return code;
}

int tracking_erase_day(TrackingService *svc, const char *month, int day,
                        char *out, size_t cap) {
    if (!out || cap < 128) return 500;
    out[0] = '\0';
    if (!month || !*month || day < 1) {
        snprintf(out, cap, "{\"ok\":false,\"error\":\"не указан месяц или день\"}");
        return 400;
    }

    char tab[64];
    int pm = 0, py = 0;
    if (month_tab_and_parts(svc, month, tab, sizeof(tab), &pm, &py) != 0) {
        snprintf(out, cap, "{\"ok\":false,\"error\":\"неверный месяц\"}");
        return 400;
    }
    if (day > days_in_month(pm, py)) {
        snprintf(out, cap, "{\"ok\":false,\"error\":\"неверный день месяца\"}");
        return 400;
    }

    const char *sn = svc->cfg ? svc->cfg->search_name : NULL;
    char *name = (sn && *sn) ? strdup(sn) : strdup("Сотрудник");
    if (!name) { snprintf(out, cap, "{\"ok\":false,\"error\":\"нет имени\"}"); return 500; }

    if (sb_enabled()) {
        int st = sb_time_day_delete(tab, day);
        if (st >= 200 && st < 300) {
            trk_dirty_put(tab);      /* фон вычистит строку листа этим же циклом */
            trk_cache_invalidate(tab);
            snprintf(out, cap, "{\"ok\":true,\"day\":%d,\"tab\":\"%s\"}", day, tab);
            trk_log("ERASE(db): %s day=%d", tab, day);
            free(name);
            return 200;
        }
        trk_log("erase: БД недоступна (HTTP %d), фоллбэк Google", st);
    }

    /* Фоллбэк: очищаем строку дня в листе напрямую */
    if (!svc->sheets.sheet_id && google_sheets_ensure(&svc->sheets) != 0) {
        snprintf(out, cap, "{\"ok\":false,\"error\":\"нет таблицы\"}");
        free(name);
        return 500;
    }
    SheetRows rows;
    sheet_rows_init(&rows);
    if (load_or_create_month_sheet(svc, tab, &rows) != 0) {
        snprintf(out, cap, "{\"ok\":false,\"error\":\"лист недоступен\"}");
        sheet_rows_free(&rows);
        free(name);
        return 500;
    }
    int block = sheet_find_employee(&rows, name);
    char date[32];
    snprintf(date, sizeof(date), "%02d.%02d.%04d", day, pm, py);
    int row = (block >= 0) ? sheet_find_date_row(&rows, block, date) : -1;
    int rc = 200;
    if (row >= 0) {
        for (int c = 1; c <= 5; c++) {
            if (c == 4) continue;        /* E — ставку храним */
            sheet_row_set(&rows.row[row], c, "");
        }
        if (write_rows(svc, tab, &rows) != 0) {
            snprintf(out, cap, "{\"ok\":false,\"error\":\"ошибка записи в Google\"}");
            rc = 500;
        } else {
            trk_cache_invalidate(tab);
            trk_log("ERASE(google): %s day=%d", tab, day);
        }
    } else {
        /* строки нет — удалять нечего */
        trk_cache_invalidate(tab);
    }
    if (rc == 200)
        snprintf(out, cap, "{\"ok\":true,\"day\":%d,\"tab\":\"%s\"}", day, tab);
    sheet_rows_free(&rows);
    free(name);
    return rc;
}

/* ================================================================
 *  Ставка за час на месяц
 * ================================================================ */

/* Вкладка месяца для API ставки: пустой month -> текущий месяц. */
static int rate_month_tab(TrackingService *svc, const char *month,
                          char *tab, size_t cap) {
    if (month && *month)
        return month_tab_and_parts(svc, month, tab, cap, NULL, NULL);
    clock_month_tab(svc->tz_offset, tab, cap);
    return (tab && tab[0]) ? 0 : -1;
}

/* Разбор ставки из строки: "600", "600.50", "600,50".
   Округляется до двух знаков (14.805 -> 14.81).
   0 — успех; -1 — не число или вне допустимого диапазона. */
static int rate_parse(const char *s, double *out) {
    if (!s || !*s || !out) return -1;
    size_t n = strlen(s);
    if (n >= 64) return -1;
    char norm[64];
    if (num_round2_text(s, norm, sizeof(norm)) != 0) return -1;
    double v = atof(norm);
    if (!(v > 0) || v > 1000000.0) return -1;
    *out = v;
    return 0;
}

/* Пересчитывает amount всех отмеченных дней месяца по новой ставке.
   0 — успех (updated — сколько строк обновлено), -1 — ошибка БД. */
static int recalc_month_amounts(TrackingService *svc, const char *tab,
                                double rate, int *updated) {
    (void)svc;
    if (updated) *updated = 0;
    if (!sb_enabled() || !tab || !*tab) return -1;

    char *et = url_encode(tab);
    if (!et) return -1;
    char path[512];
    snprintf(path, sizeof(path),
             "/time_entries?month=eq.%s&select=day,hours&order=day", et);
    free(et);

    int st = 0;
    char *body = sb_get(path, &st);
    if (!body || st < 200 || st > 299) { free(body); return -1; }
    cJSON *arr = cJSON_Parse(body);
    free(body);
    if (!cJSON_IsArray(arr)) { cJSON_Delete(arr); return -1; }

    int ok = 0, failed = 0;
    cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr) {
        cJSON *jd = cJSON_GetObjectItemCaseSensitive(it, "day");
        cJSON *jh = cJSON_GetObjectItemCaseSensitive(it, "hours");
        if (!cJSON_IsNumber(jd)) continue;
        int day = (int)jd->valuedouble;
        if (day < 1 || day > 31) continue;

        char hours_buf[32] = "";
        if (cJSON_IsNumber(jh))
            num_format2(jh->valuedouble, hours_buf, sizeof(hours_buf));
        else if (cJSON_IsString(jh) && jh->valuestring)
            snprintf(hours_buf, sizeof(hours_buf), "%s", jh->valuestring);
        if (!num_is_valid(hours_buf)) continue;

        char sum_buf[32];
        num_format2(time_entry_sum(hours_buf, rate), sum_buf, sizeof(sum_buf));
        int st2 = sb_time_amount_set(tab, day, sum_buf);
        if (st2 >= 200 && st2 < 300) ok++;
        else failed++;
    }
    cJSON_Delete(arr);
    if (updated) *updated = ok;
    return failed ? -1 : 0;
}

int tracking_month_rate_get(TrackingService *svc, const char *month,
                            char *out, size_t cap) {
    if (!out || cap < 128) return 500;
    out[0] = '\0';

    char tab[64];
    if (rate_month_tab(svc, month, tab, sizeof(tab)) != 0) {
        snprintf(out, cap, "{\"ok\":false,\"error\":\"неверный месяц\"}");
        return 400;
    }

    int explicit_rate = 0;
    double rate = month_rate_ex(svc, tab, &explicit_rate);
    char rate_buf[32];
    num_format2(rate, rate_buf, sizeof(rate_buf));
    snprintf(out, cap,
             "{\"ok\":true,\"month\":\"%s\",\"rate\":\"%s\",\"source\":\"%s\"}",
             tab, rate_buf, explicit_rate ? "month" : "default");
    return 200;
}

int tracking_month_rate_set(TrackingService *svc, const char *month,
                            const char *rate, char *out, size_t cap) {
    if (!out || cap < 128) return 500;
    out[0] = '\0';

    char tab[64];
    if (rate_month_tab(svc, month, tab, sizeof(tab)) != 0) {
        snprintf(out, cap, "{\"ok\":false,\"error\":\"неверный месяц\"}");
        return 400;
    }

    double value = 0;
    if (rate_parse(rate, &value) != 0) {
        snprintf(out, cap,
                 "{\"ok\":false,\"error\":\"ставка должна быть числом больше 0\"}");
        return 400;
    }
    char rate_buf[32];
    num_format2(value, rate_buf, sizeof(rate_buf));

    if (!sb_enabled()) {
        snprintf(out, cap,
                 "{\"ok\":false,\"error\":\"БД недоступна: ставка хранится в Supabase\"}");
        return 503;
    }
    int st = sb_month_rate_set(tab, rate_buf);
    if (st == 404) {
        snprintf(out, cap,
                 "{\"ok\":false,\"error\":\"таблица month_rates не создана: "
                 "выполните docs/sql/month_rates.sql в Supabase SQL Editor\"}");
        return 503;
    }
    if (st < 200 || st > 299) {
        snprintf(out, cap,
                 "{\"ok\":false,\"error\":\"не удалось сохранить ставку\"}");
        return 500;
    }

    int updated = 0;
    if (recalc_month_amounts(svc, tab, value, &updated) != 0) {
        snprintf(out, cap,
                 "{\"ok\":false,\"error\":\"ставка сохранена, но суммы "
                 "пересчитать не удалось — попробуйте ещё раз\"}");
        return 500;
    }

    trk_dirty_put(tab);          /* выгрузка в Google — фоном */
    trk_cache_invalidate(tab);   /* Mini App увидит новые суммы сразу */
    snprintf(out, cap,
             "{\"ok\":true,\"month\":\"%s\",\"rate\":\"%s\",\"days_updated\":%d}",
             tab, rate_buf, updated);
    trk_log("RATE: %s = %s (days=%d)", tab, rate_buf, updated);
    return 200;
}

/* ================================================================
 *  Выгрузка БД -> Google Sheets (БД — первоисточник, Google — архив)
 * ================================================================ */

/* Выгружает месяц: строки СВОЕГО блока берутся из time_entries —
   новые вставляются, пропавшие очищаются. Явная ставка месяца
   записывается в E всех строк-дней; если её нет — E не трогаем.
   0 — успех; dirty-флаг снимается только при успехе. */
static int tracking_push_month(TrackingService *svc, const char *tab,
                                char *err, size_t errcap) {
    if (!err || errcap == 0) return -1;
    err[0] = '\0';
    if (!tab || !*tab) { snprintf(err, errcap, "нет месяца"); return -1; }

    if (!svc->sheets.sheet_id && google_sheets_ensure(&svc->sheets) != 0) {
        snprintf(err, errcap, "%.*s", (int)(errcap - 1),
                 svc->sheets.last_error[0] ? svc->sheets.last_error : "нет таблицы");
        return -1;
    }

    const char *sn = svc->cfg ? svc->cfg->search_name : NULL;
    char *name = (sn && *sn) ? strdup(sn) : strdup("Сотрудник");
    if (!name) { snprintf(err, errcap, "нет имени сотрудника"); return -1; }

    int pm = 0, py = 0;
    char t2[64];
    if (month_tab_and_parts(svc, tab, t2, sizeof(t2), &pm, &py) != 0) {
        snprintf(err, errcap, "неверный месяц");
        free(name);
        return -1;
    }

    /* 1) дни месяца из БД */
    char *et = url_encode(tab);
    if (!et) { free(name); snprintf(err, errcap, "нет памяти"); return -1; }
    char path[512];
    snprintf(path, sizeof(path),
             "/time_entries?month=eq.%s&select=day,t_in,t_out,hours,amount&order=day",
             et);
    free(et);
    int st = 0;
    char *body = sb_get(path, &st);
    if (!body || st < 200 || st > 300) {
        free(body);
        free(name);
        snprintf(err, errcap, "БД недоступна (HTTP %d)", st);
        return -1;
    }
    cJSON *db = cJSON_Parse(body);
    free(body);
    if (!db || !cJSON_IsArray(db)) {
        cJSON_Delete(db);
        free(name);
        snprintf(err, errcap, "БД: битый ответ");
        return -1;
    }

    int rc = -1;
    SheetRows rows;
    sheet_rows_init(&rows);
    if (load_or_create_month_sheet(svc, tab, &rows) != 0) {
        snprintf(err, errcap, "лист недоступен: %.*s",
                 (int)(errcap > 32 ? errcap - 32 : 1),
                 svc->sheets.last_error[0] ? svc->sheets.last_error : "");
        goto finish;
    }

    int block = sheet_find_employee(&rows, name);
    if (block < 0) {
        /* блока ещё нет: разделитель + строка ФИО (как при первой записи) */
        sheet_rows_insert(&rows, rows.count);
        sheet_rows_insert(&rows, rows.count);
        int ni = rows.count - 1;
        sheet_row_set(&rows.row[ni], 0, name);
        block = ni;
    }

    int have_day[32] = { 0 };
    int rate_explicit = 0;
    double rate = month_rate_ex(svc, tab, &rate_explicit);
    char rate_buf[32];
    num_format2(rate, rate_buf, sizeof(rate_buf));

    /* 2) строки-дни из БД -> в лист */
    cJSON *it = NULL;
    cJSON_ArrayForEach(it, db) {
        cJSON *jd = cJSON_GetObjectItemCaseSensitive(it, "day");
        if (!cJSON_IsNumber(jd)) continue;
        int d = (int)jd->valuedouble;
        if (d < 1 || d > 31) continue;
        have_day[d] = 1;

        cJSON *ji = cJSON_GetObjectItemCaseSensitive(it, "t_in");
        cJSON *jo = cJSON_GetObjectItemCaseSensitive(it, "t_out");
        cJSON *jh = cJSON_GetObjectItemCaseSensitive(it, "hours");
        cJSON *ja = cJSON_GetObjectItemCaseSensitive(it, "amount");
        const char *vin  = (cJSON_IsString(ji) && ji->valuestring) ? ji->valuestring : "";
        const char *vout = (cJSON_IsString(jo) && jo->valuestring) ? jo->valuestring : "";
        char vh[32] = "", vs[32] = "";
        if (cJSON_IsNumber(jh)) num_format2(jh->valuedouble, vh, sizeof(vh));
        if (cJSON_IsNumber(ja)) num_format2(ja->valuedouble, vs, sizeof(vs));

        char date[32];
        snprintf(date, sizeof(date), "%02d.%02d.%04d", d, pm, py);
        int row = sheet_find_date_row(&rows, block, date);
        if (row < 0) {
            int last = sheet_find_block_end(&rows, block);
            sheet_rows_insert(&rows, last + 1);
            row = last + 1;
        }
        sheet_row_set(&rows.row[row], 0, date);
        sheet_row_set(&rows.row[row], 1, vin);
        sheet_row_set(&rows.row[row], 2, vout);
        sheet_row_set(&rows.row[row], 3, vh);
        if (rate_explicit || !*sheet_row_get(&rows.row[row], 4))
            sheet_row_set(&rows.row[row], 4, rate_buf);
        sheet_row_set(&rows.row[row], 5, vs);
    }

    /* 3) дни листа, которых больше нет в БД — снимаем отметки (ставку храним) */
    for (int i = block + 1; i < rows.count; i++) {
        if (sheet_row_is_empty(&rows.row[i])) break;
        const char *a = sheet_row_get(&rows.row[i], 0);
        if (!*a) continue;
        char nd[64];
        date_normalize(a, nd, sizeof(nd));
        int d = 0, m = 0, y = 0;
        if (date_parse(nd, &d, &m, &y) != 0) continue;
        if (m != pm || y != py) continue;
        if (d >= 1 && d <= 31 && have_day[d]) continue;
        for (int c = 1; c <= 5; c++) {
            if (c == 4) continue;              /* E — ставка, не трогаем */
            sheet_row_set(&rows.row[i], c, "");
        }
        if (rate_explicit)                     /* явная ставка месяца — во все дни */
            sheet_row_set(&rows.row[i], 4, rate_buf);
    }

    if (write_rows(svc, tab, &rows) != 0) {
        snprintf(err, errcap, "ошибка записи в Google");
        goto finish;                 /* dirty остаётся — повторим в цикле */
    }
    trk_dirty_clear(tab);
    trk_log("PUSH(db->google): %s ok", tab);
    rc = 0;

finish:
    cJSON_Delete(db);
    sheet_rows_free(&rows);
    free(name);
    return rc;
}

void tracking_push_pending(TrackingService *svc) {
    if (!sb_enabled()) return;
    DIR *d = opendir("/tmp");
    if (!d) return;
    char tabs[32][16];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < 32) {
        if (strncmp(e->d_name, "gdirty_", 7) != 0) continue;
        const char *tab = e->d_name + 7;
        size_t tl = strlen(tab);
        if (tl == 0 || tl > 15) continue;
        if (strspn(tab, "0123456789.") != tl) continue;
        snprintf(tabs[n], sizeof(tabs[0]), "%s", tab);
        n++;
    }
    closedir(d);
    for (int i = 0; i < n; i++) {
        char err[160] = "";
        if (tracking_push_month(svc, tabs[i], err, sizeof(err)) != 0)
            trk_log("PUSH fail: %s (%s)", tabs[i], err);
    }
}

int tracking_push_all(TrackingService *svc, const char *const *months, int nmonths) {
    if (!sb_enabled()) return 0;
    int ok = 0;
    for (int i = 0; months && i < nmonths; i++) {
        if (!months[i] || !*months[i]) continue;
        char tab[64];
        int pm = 0, py = 0;
        if (month_tab_and_parts(svc, months[i], tab, sizeof(tab), &pm, &py) != 0)
            continue;
        char err[160] = "";
        if (tracking_push_month(svc, tab, err, sizeof(err)) == 0) ok++;
        else trk_log("PUSH fail: %s (%s)", tab, err);
    }
    return ok;
}

/* ================================================================
 *  Месяцы, итоги, создание вкладки
 * ================================================================ */

void tracking_month_label(const char *numeric, char *out, size_t cap) {
    month_label(numeric, out, cap);
}

int tracking_list_months(TrackingService *svc, char ***out, int *count, int max) {
    if (!out || !count || max <= 0) return -1;
    *out = NULL;
    *count = 0;

    if (!svc->sheets.sheet_id && google_sheets_ensure(&svc->sheets) != 0) return -1;
    if (!svc->sheets.sheet_id) return -1;

    char **list = NULL;
    int n = 0;
    if (google_sheets_list_tabs(&svc->sheets, &list, &n, max) != 0) return -1;

    /* оставляем только вкладки-месяцы вида "10.2026" */
    int kept = 0;
    for (int i = 0; i < n; i++) {
        int mon = 0, year = 0;
        if (sscanf(list[i], "%d.%d", &mon, &year) == 2 && mon >= 1 && mon <= 12) {
            list[kept++] = list[i];
        } else {
            free(list[i]);
        }
    }
    *out = list;
    *count = kept;
    return 0;
}

int tracking_month_total(TrackingService *svc, const char *month,
                          double *hours, double *sum, int *days) {
    if (hours) *hours = 0;
    if (sum) *sum = 0;
    if (days) *days = 0;
    if (!month || !*month) return -1;

    if (!svc->sheets.sheet_id && google_sheets_ensure(&svc->sheets) != 0) return -1;
    if (!svc->sheets.sheet_id) return -1;

    SheetRows rows;
    sheet_rows_init(&rows);
    char *resp = google_sheets_get_values(&svc->sheets, month);
    if (!resp) return -1;
    sheet_rows_parse_values_json(&rows, resp);
    free(resp);

    const char *name = (svc->cfg && svc->cfg->search_name) ? svc->cfg->search_name : NULL;
    int block = name ? sheet_find_employee(&rows, name) : -1;
    if (block >= 0)
        sheet_block_totals(&rows, block, hours, sum, days);
    sheet_rows_free(&rows);
    return 0;
}

int tracking_ensure_month(TrackingService *svc, const char *month,
                           char *out, size_t cap) {
    if (!out || cap < 128) return 500;
    out[0] = '\0';
    svc->sheets.last_get_status = 0;
    svc->sheets.last_get_resp[0] = '\0';
    svc->sheets.last_create_status = 0;
    svc->sheets.last_create_resp[0] = '\0';

    if (!svc->sheets.sheet_id && google_sheets_ensure(&svc->sheets) != 0) {
        char *e = json_escape(svc->sheets.last_error[0] ? svc->sheets.last_error
                                                         : "нет доступа к таблице");
        snprintf(out, cap, "{\"ok\":false,\"error\":\"%s\"}", e ? e : "");
        free(e);
        return 500;
    }

    char tab[64];
    int pmon = 0, pyear = 0;
    if (month_tab_and_parts(svc, month, tab, sizeof(tab), &pmon, &pyear) != 0) {
        snprintf(out, cap, "{\"ok\":false,\"error\":\"неверный месяц\"}");
        return 400;
    }

    SheetRows rows;
    sheet_rows_init(&rows);
    int code = 200;
    if (load_or_create_month_sheet(svc, tab, &rows) != 0) {
        char *e = json_escape(svc->sheets.last_error[0] ? svc->sheets.last_error
                                                         : "не удалось создать вкладку месяца");
        snprintf(out, cap, "{\"ok\":false,\"error\":\"%s\"}", e ? e : "");
        free(e);
        code = 500;
        goto ensure_finish;
    }
    if (write_rows(svc, tab, &rows) != 0) {
        char *e = json_escape(svc->sheets.last_error[0] ? svc->sheets.last_error
                                                         : "не удалось записать заголовки вкладки");
        snprintf(out, cap, "{\"ok\":false,\"error\":\"%s\"}", e ? e : "");
        free(e);
        code = 500;
        goto ensure_finish;
    }

    char *eg = json_escape(svc->sheets.last_get_resp);
    char *ec = json_escape(svc->sheets.last_create_resp);
    snprintf(out, cap,
             "{\"ok\":true,\"month\":\"%s\",\"rows\":%d,\"sheet_id\":\"%s\","
             "\"get_status\":%d,\"get_resp\":\"%s\","
             "\"create_status\":%d,\"create_resp\":\"%s\"}",
             tab, rows.count, svc->sheets.sheet_id ? svc->sheets.sheet_id : "",
             svc->sheets.last_get_status, eg ? eg : "",
             svc->sheets.last_create_status, ec ? ec : "");
    free(eg); free(ec);
    trk_log("ENSURE MONTH: %s rows=%d", tab, rows.count);
    trk_cache_invalidate(tab);

ensure_finish:
    sheet_rows_free(&rows);
    return code;
}

/* ================================================================
 *  Инициализация
 * ================================================================ */

void tracking_service_init(TrackingService *svc, const AppConfig *cfg) {
    if (!svc) return;
    memset(svc, 0, sizeof(*svc));
    svc->cfg = cfg;
    svc->tz_offset = 3 * 3600;   /* локальное время = UTC + 3ч */

    google_sheets_init(&svc->sheets);

    const char *r = getenv("HOURLY_RATE");
    if (r && *r) svc->rate = num_parse(r);
    else { svc->rate = 500.0; trk_log("HOURLY_RATE not set, using default 500"); }

    const char *tz = getenv("TZ_OFFSET_SECONDS");
    if (tz) svc->tz_offset = atoi(tz);

    trk_log("init: sheet=%s sa=%s rate=%.0f tz=%d",
            svc->sheets.sheet_id ? svc->sheets.sheet_id : "(null)",
            (svc->sheets.auth.sa_json) ? "есть" : "нет",
            svc->rate, svc->tz_offset);
}

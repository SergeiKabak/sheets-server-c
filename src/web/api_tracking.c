#include "web/api_tracking.h"

#include "core/date_time.h"
#include "core/json_text.h"
#include "core/strbuf.h"
#include "core/url_codec.h"
#include "domain/timesheet.h"
#include "infra/clock.h"
#include "infra/supabase.h"
#include "web/http_reply.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Соответствие кода ответа — статусной строке. */
static const char *status_for(int code) {
    switch (code) {
        case 200: return "200 OK";
        case 400: return "400 Bad Request";
        case 409: return "409 Conflict";
        case 503: return "503 Service Unavailable";
        default:  return "500 Internal Server Error";
    }
}

/* POST /api/tracking/clock?action=in|out */
static void api_tracking_clock(AppContext *app, int fd, const char *query) {
    char *action = url_query_param(query, "action");
    char out[1024];
    int code = tracking_do_clock(&app->tracking, action, out, sizeof(out));
    http_reply_json(fd, status_for(code), out);
    free(action);
}

/* GET  /api/tracking/rate?month=...       — ставка месяца
   POST /api/tracking/rate?month=...&rate=600 — сохранить и пересчитать */
static void api_tracking_rate(AppContext *app, int fd, const char *query) {
    char *month = url_query_param(query, "month");
    char *rate = url_query_param(query, "rate");
    char out[1024];
    int code = (rate && *rate)
        ? tracking_month_rate_set(&app->tracking, month, rate, out, sizeof(out))
        : tracking_month_rate_get(&app->tracking, month, out, sizeof(out));
    http_reply_json(fd, status_for(code), out);
    free(month);
    free(rate);
}

/* GET /api/tracking/days?month=Октябрь 2026 — какие дни уже отмечены. */
static void api_tracking_days(AppContext *app, int fd, const char *query) {
    char *month = url_query_param(query, "month");
    char out[16384];
    int code = tracking_days_json(&app->tracking, month, out, sizeof(out));
    http_reply_json(fd, status_for(code), out);
    free(month);
}

/* POST /api/tracking/fill?month=...&day=...&in=...&out=... */
static void api_tracking_fill(AppContext *app, int fd, const char *query) {
    char *month = url_query_param(query, "month");
    char *day_s = url_query_param(query, "day");
    char *in_s  = url_query_param(query, "in");
    char *out_s = url_query_param(query, "out");
    int day = (day_s && *day_s) ? atoi(day_s) : 0;

    char out[2048];
    int code = tracking_fill_day(&app->tracking, month, day, in_s, out_s,
                                 out, sizeof(out));
    http_reply_json(fd, status_for(code), out);
    free(month); free(day_s); free(in_s); free(out_s);
}

/* POST /api/tracking/erase?month=...&day=N — удалить отметку дня. */
static void api_tracking_erase(AppContext *app, int fd, const char *query) {
    char *month = url_query_param(query, "month");
    char *day_s = url_query_param(query, "day");
    int day = (day_s && *day_s) ? atoi(day_s) : 0;
    char out[512];
    int code = tracking_erase_day(&app->tracking, month, day, out, sizeof(out));
    http_reply_json(fd, status_for(code), out);
    free(month);
    free(day_s);
}

/* GET /api/tracking/summary?month=...&name=... — сводка по сотруднику. */
static void api_tracking_summary(AppContext *app, int fd, const char *query) {
    char *name = url_query_param(query, "name");
    char *month = url_query_param(query, "month");
    /* Если имя не передано — берём SEARCH_NAME (отмечается только один сотрудник) */
    if (!name) {
        const char *sn = app->cfg.search_name;
        if (sn && *sn) name = strdup(sn);
    }

    char sheet[256];
    if (month && *month) {
        /* "Октябрь 2026" -> "10.2026" */
        month_numeric_from_label(month, clock_current_year(app->tracking.tz_offset),
                                 sheet, sizeof(sheet));
    } else {
        clock_month_tab(app->tracking.tz_offset, sheet, sizeof(sheet));
    }

    if (!app->tracking.sheets.sheet_id) {
        http_reply_json(fd, "503 Service Unavailable",
                        "{\"ok\":false,\"error\":\"таблица учёта времени не настроена\"}");
        free(name); free(month);
        return;
    }

    SheetRows rows;
    sheet_rows_init(&rows);
    char *resp = google_sheets_get_values(&app->tracking.sheets, sheet);
    if (!resp) {
        http_reply_json(fd, "503 Service Unavailable",
                        "{\"ok\":false,\"error\":\"таблица ещё не готова\"}");
        free(name); free(month);
        return;
    }
    sheet_rows_parse_values_json(&rows, resp);
    free(resp);

    double total_hours = 0, total_sum = 0;
    int days = 0;
    int block = name ? sheet_find_employee(&rows, name) : -1;
    if (block >= 0)
        sheet_block_totals(&rows, block, &total_hours, &total_sum, &days);

    StrBuf sb;
    sbuf_init(&sb);
    sbuf_append(&sb, "{\"ok\":true,\"name\":");
    sbuf_append_json_string(&sb, name ? name : "");
    sbuf_append(&sb, ",\"month\":");
    sbuf_append_json_string(&sb, sheet);
    sbuf_appendf(&sb, ",\"days\":%d,\"total_hours\":\"%.2f\",\"total_sum\":\"%.2f\"}",
               days, total_hours, total_sum);
    char *msg = sbuf_take(&sb);
    http_reply_json(fd, "200 OK", msg ? msg : "{}");
    free(msg);
    sheet_rows_free(&rows);
    free(name); free(month);
}

/* GET /api/tracking/months -> список месяцев с заработком. */
static void api_tracking_months(AppContext *app, int fd) {
    char **months = NULL;
    int count = 0;
    if (tracking_list_months(&app->tracking, &months, &count, 24) != 0 || count == 0) {
        if (months) {
            for (int i = 0; i < count; i++) free(months[i]);
            free(months);
        }
        http_reply_json(fd, "200 OK", "{\"ok\":true,\"months\":[]}");
        return;
    }

    StrBuf sb;
    sbuf_init(&sb);
    sbuf_append(&sb, "{\"ok\":true,\"months\":[");
    for (int i = 0; i < count; i++) {
        char label[64];
        tracking_month_label(months[i], label, sizeof(label));
        if (i) sbuf_append(&sb, ",");
        sbuf_append(&sb, "{\"tab\":");
        sbuf_append_json_string(&sb, months[i]);
        sbuf_append(&sb, ",\"label\":");
        sbuf_append_json_string(&sb, label);
        sbuf_append(&sb, "}");
    }
    sbuf_append(&sb, "]}");
    char *msg = sbuf_take(&sb);
    http_reply_json(fd, "200 OK", msg ? msg : "{\"ok\":true,\"months\":[]}");
    free(msg);

    for (int i = 0; i < count; i++) free(months[i]);
    free(months);
}

/* GET /diag/tracking/ensure?month=Сентябрь 2026 — создать вкладку месяца. */
static void api_tracking_ensure(AppContext *app, int fd, const char *query) {
    char *month = url_query_param(query, "month");
    char out[8192];
    int code = tracking_ensure_month(&app->tracking, month, out, sizeof(out));
    http_reply_json(fd, status_for(code), out);
    free(month);
}

/* ================================================================
 *  Диагностика трекинга: /diag/tracking[?write=1]
 * ================================================================ */
static void api_tracking_diag(AppContext *app, int fd, const char *query) {
    GoogleSheets *sheets = &app->tracking.sheets;

    StrBuf sb;
    sbuf_init(&sb);
    sbuf_append(&sb, "{\"ok\":true");

    /* 1) наличие Service Account */
    sbuf_append(&sb, ",\"sa_configured\":");
    sbuf_append(&sb, sheets->auth.sa_json ? "true" : "false");
    if (sheets->auth.sa_json) {
        char *em = json_field_string(sheets->auth.sa_json, "client_email");
        sbuf_append(&sb, ",\"sa_email\":");
        sbuf_append_json_string(&sb, em ? em : "");
        free(em);
    }

    /* 2) получение access token */
    const char *tok = google_auth_token(&sheets->auth);
    sbuf_append(&sb, ",\"token_ok\":");
    sbuf_append(&sb, tok ? "true" : "false");

    /* 3) таблица */
    sbuf_append(&sb, ",\"sheet_id\":");
    sbuf_append_json_string(&sb, sheets->sheet_id ? sheets->sheet_id : "");
    sbuf_append(&sb, ",\"env_sheet_id\":");
    sbuf_append_json_string(&sb, getenv("TRACKING_SHEET_ID") ? getenv("TRACKING_SHEET_ID") : "");
    sbuf_append(&sb, ",\"share_email\":");
    sbuf_append_json_string(&sb, getenv("TRACKING_SHARE_EMAIL") ? getenv("TRACKING_SHARE_EMAIL") : "");
    sbuf_append(&sb, ",\"emp_map\":\"не требуется\"");
    sbuf_appendf(&sb, ",\"rate\":\"%.0f\"", app->tracking.rate);
    sbuf_append(&sb, ",\"rates_map\":\"не требуется\"");
    sbuf_appendf(&sb, ",\"tz_offset\":\"%d\"", app->tracking.tz_offset);

    /* 4) список вкладок */
    if (sheets->sheet_id && tok) {
        char **tabs = NULL;
        int tabc = 0;
        sbuf_append(&sb, ",\"tabs\":[");
        if (google_sheets_list_tabs(sheets, &tabs, &tabc, 128) == 0) {
            for (int i = 0; i < tabc; i++) {
                if (i) sbuf_append(&sb, ",");
                sbuf_append_json_string(&sb, tabs[i]);
            }
            for (int i = 0; i < tabc; i++) free(tabs[i]);
            free(tabs);
        }
        sbuf_append(&sb, "]");
    }

    /* 5) проверка чтения вкладки месяца */
    char month[256];
    clock_month_tab(app->tracking.tz_offset, month, sizeof(month));
    sbuf_append(&sb, ",\"month_tab\":");
    sbuf_append_json_string(&sb, month);
    if (sheets->sheet_id && tok) {
        char *resp = google_sheets_get_values(sheets, month);
        sbuf_append(&sb, ",\"read_ok\":");
        sbuf_append(&sb, resp ? "true" : "false");
        if (resp) {
            SheetRows rows;
            sheet_rows_init(&rows);
            sheet_rows_parse_values_json(&rows, resp);
            sbuf_appendf(&sb, ",\"rows\":%d", rows.count);
            sheet_rows_free(&rows);
            free(resp);
        }
    }

    /* 6) пробная запись (только по явному запросу ?write=1) */
    char *w = url_query_param(query, "write");
    if (w && strcmp(w, "1") == 0 && sheets->sheet_id && tok) {
        char *resp = google_sheets_get_values(sheets, month);
        if (resp) {
            free(resp);
        } else if (google_sheets_create_sheet(sheets, month) == 0) {
            sbuf_append(&sb, ",\"tab_created\":true");
            char vals[6][64];
            for (int c = 0; c < SHEET_COLS; c++)
                snprintf(vals[c], 64, "%s", SHEET_HEADERS[c]);
            const char *ptr[6] = { vals[0], vals[1], vals[2], vals[3], vals[4], vals[5] };
            StrBuf row;
            sbuf_init(&row);
            sbuf_append(&row, "[");
            for (int c = 0; c < SHEET_COLS; c++) {
                if (c) sbuf_append(&row, ",");
                sbuf_append_json_string(&row, ptr[c]);
            }
            sbuf_append(&row, "]");
            char *hj = sbuf_take(&row);
            if (hj) {
                google_sheets_update_range(sheets, month, "!A1:F1", hj);
                free(hj);
            }
        }

        char date[32], ts[16];
        clock_datetime(app->tracking.tz_offset, date, sizeof(date), ts, sizeof(ts));
        const char *cells[6] = { date, ts, "", "", "500", "" };
        StrBuf row;
        sbuf_init(&row);
        sbuf_append(&row, "[");
        for (int c = 0; c < SHEET_COLS; c++) {
            if (c) sbuf_append(&row, ",");
            sbuf_append_json_string(&row, cells[c]);
        }
        sbuf_append(&row, "]");
        char *json = sbuf_take(&row);
        int rc = -1;
        if (json) {
            rc = google_sheets_append_row(sheets, month, json);
            free(json);
        }
        sbuf_append(&sb, ",\"write_ok\":");
        sbuf_append(&sb, rc == 0 ? "true" : "false");
        sbuf_append(&sb, ",\"write_test\":{\"date\":");
        sbuf_append_json_string(&sb, date);
        sbuf_append(&sb, ",\"time\":");
        sbuf_append_json_string(&sb, ts);
        sbuf_append(&sb, "}");
    }
    free(w);

    /* 7) Подсказка по действиям */
    if (strstr(sheets->last_error, "DISABLED")) {
        sbuf_append(&sb, ",\"hint\":\"Enable Google Sheets API in the project that owns "
                        "the Service Account (project id women-health-684ad). The API key "
                        "project already has it enabled, but the OAuth2 consumer project "
                        "does not.\"");
    } else if (strstr(sheets->last_error, "not accessible")) {
        sbuf_append(&sb, ",\"hint\":\"Share the Time tracking spreadsheet with the "
                        "Service Account email as Editor.\"");
    } else if (strstr(sheets->last_error, "does not have permission")) {
        sbuf_append(&sb, ",\"hint\":\"Service Account cannot create new spreadsheets. "
                        "Set TRACKING_SHEET_ID to the existing Time tracking spreadsheet id.\"");
    } else if (!app->tracking.rate) {
        sbuf_append(&sb, ",\"hint\":\"HOURLY_RATE is 0 - set it (e.g. 500) so the "
                        "payment column is calculated.\"");
    } else if (!sheets->sheet_id[0]) {
        sbuf_append(&sb, ",\"hint\":\"TRACKING_SHEET_ID is not set. Set it to the Time "
                        "tracking spreadsheet id so the server can read/write it.\"");
    }

    if (sheets->last_error[0]) {
        sbuf_append(&sb, ",\"last_error\":");
        sbuf_append_json_string(&sb, sheets->last_error);
    }
    sbuf_append(&sb, "}");

    char *out = sbuf_take(&sb);
    http_reply_json(fd, "200 OK", out ? out : "{}");
    free(out);
}

void api_tracking_handle(AppContext *app, int fd, const char *method,
                          const char *path, const char *body) {
    (void)method;   /* маршрутизация идёт по path, метод пока не различается */
    (void)body;     /* тело запроса не разбирается: данные идут в query */

    /* Первый вызов: находим существующую таблицу или создаём новую
       (владельцем будет Service Account). */
    if (!app->tracking.sheets.sheet_id) {
        int rc = google_sheets_ensure(&app->tracking.sheets);
        if (rc != 0 && strcmp(path, "/diag/tracking") != 0) {
            http_reply_json(fd, "503 Service Unavailable",
                            "{\"ok\":false,\"error\":\"таблица учёта времени не настроена\"}");
            return;
        }
    }

    if (strncmp(path, "/api/tracking/clock", 18) == 0) {
        api_tracking_clock(app, fd, path);
        return;
    }
    if (strncmp(path, "/api/tracking/rate", 18) == 0) {
        api_tracking_rate(app, fd, path);
        return;
    }
    if (strncmp(path, "/api/tracking/days", 18) == 0) {
        api_tracking_days(app, fd, path);
        return;
    }
    if (strncmp(path, "/api/tracking/fill", 18) == 0) {
        api_tracking_fill(app, fd, path);
        return;
    }
    if (strncmp(path, "/api/tracking/erase", 19) == 0) {
        api_tracking_erase(app, fd, path);
        return;
    }
    if (strncmp(path, "/api/tracking/summary", 20) == 0) {
        api_tracking_summary(app, fd, path);
        return;
    }
    if (strcmp(path, "/api/tracking/months") == 0) {
        api_tracking_months(app, fd);
        return;
    }
    if (strncmp(path, "/diag/tracking/ensure", 21) == 0) {
        api_tracking_ensure(app, fd, path);
        return;
    }
    if (strcmp(path, "/diag/tracking") == 0) {
        api_tracking_diag(app, fd, path);
        return;
    }
    http_reply_json(fd, "404 Not Found", "{\"ok\":false,\"error\":\"не найдено\"}");
}

#include "web/diag.h"

#include "app/month_cache.h"
#include "core/json_text.h"
#include "core/strbuf.h"
#include "domain/schedule.h"
#include "infra/http_client.h"
#include "infra/supabase.h"
#include "web/http_reply.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Диагностика индекса кабинетов: количество строк и пример. */
static void diag_cabinet(int fd) {
    FILE *fp = fopen(cabinet_index_path(), "r");
    if (!fp) {
        http_reply(fd, "503 Service Unavailable", "{\"error\":\"индекс ещё не готов\"}");
        return;
    }
    long lines = 0;
    char ln[1024];
    char sample[3072] = "";
    int si = 0;
    while (fgets(ln, sizeof(ln), fp)) {
        lines++;
        if (si < 6) {
            /* экранируем перевод строки и табуляцию для корректного JSON */
            for (const char *q = ln; *q && si + 8 < (int)sizeof(sample); q++) {
                if (*q == '\n' || *q == '\r') { sample[si++] = ' '; continue; }
                if (*q == '\t') { sample[si++] = ' '; continue; }
                if (*q == '"' || *q == '\\') { sample[si++] = '\\'; }
                sample[si++] = *q;
            }
            sample[si++] = ' ';
            sample[si] = '\0';
        }
    }
    fclose(fp);
    char body[4096];
    snprintf(body, sizeof(body),
             "{\"index_lines\":%ld,\"sample\":\"%s\"}", lines, sample);
    http_reply(fd, "200 OK", body);
}

/* Диагностика буфера Supabase: точные счётчики, пульс sync_log,
 * состояние time_entries. Только чтение. */
static void diag_supabase(AppContext *app, int fd) {
    (void)app;
    MonthSchedule months[SCHEDULE_MAX_MONTHS];
    memset(months, 0, sizeof(months));
    int c = month_cache_load(months, SCHEDULE_MAX_MONTHS);
    const char *titles[SCHEDULE_MAX_MONTHS];
    int nt = 0;
    for (int i = 0; i < c && nt < SCHEDULE_MAX_MONTHS; i++) titles[nt++] = months[i].title;

    char body[8192];
    if (sb_diag(titles, nt, body, sizeof(body)) != 0 && !strstr(body, "\"ok\"")) {
        snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"диагностика недоступна\"}");
    }
    month_schedules_free(months, c);
    http_reply(fd, "200 OK", body);
}

/* Диагностика: пробуем достучаться до Google Sheets и отдаём голову ответа. */
static void diag_google(AppContext *app, int fd) {
    int status = 0;
    char url[1024];
    snprintf(url, sizeof(url),
        "https://sheets.googleapis.com/v4/spreadsheets/%s"
        "?fields=sheets.properties(title,hidden)&key=%s",
        app->cfg.spreadsheet_id ? app->cfg.spreadsheet_id : "",
        app->cfg.google_api_key ? app->cfg.google_api_key : "");
    char *resp = http_get(url, &status);
    char body[1024];
    if (resp) {
        snprintf(body, sizeof(body), "google OK. head:\n%.700s", resp);
        free(resp);
    } else {
        snprintf(body, sizeof(body),
                 "google FAILED (curl error - см. лог: status и вывод curl)");
    }
    http_reply(fd, "200 OK", body);
}

void diag_handle(AppContext *app, int fd, const char *path) {
    if (strcmp(path, "/diag/cabinet") == 0) {
        diag_cabinet(fd);
        return;
    }
    if (strcmp(path, "/diag/supabase") == 0) {
        diag_supabase(app, fd);
        return;
    }
    if (strcmp(path, "/diag") == 0) {
        diag_google(app, fd);
        return;
    }
    http_reply(fd, "404 Not Found", "Not Found");
}

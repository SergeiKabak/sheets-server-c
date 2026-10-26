/* Локальный тест нового заполнения Time tracking.
   Собирается вместе с tracking.c и подменяет http_request_full,
   поэтому Google API не нужен. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tracking.c"

static int g_status;
static char *g_last_put = NULL;
static char *g_last_put_url = NULL;
static int g_put_count = 0;
static int g_created_sep = 0;

static const char *MONTH_JSON =
    "{\"range\":\"'10.2026'!A1:F8\",\"majorDimension\":\"ROWS\",\"values\":["
    "[\"Дата\",\"Время прихода\",\"Время ухода\",\"Время в часах\",\"Ставка в час\",\"Сумма за день\"],"
    "[\"Иванов Иван\",\"\",\"\",\"\",\"\",\"\"],"
    "[\"05.10.2026\",\"07:30\",\"14:40\",\"7.17\",\"500\",\"3585.00\"],"
    "[\"06.10.2026\",\"08:00\",\"\",\"\",\"500\",\"\"],"
    "[\"\",\"\",\"\",\"\",\"\",\"\"],"
    "[\"Петров Петр\",\"\",\"\",\"\",\"\",\"\"],"
    "[\"05.10.2026\",\"09:00\",\"18:00\",\"9.00\",\"400\",\"3600.00\"]"
    "]}";

char *http_request_full(const char *method, const char *url,
                        const char *content_type, const char *body,
                        const char *bearer, int *status) {
    (void)content_type;
    (void)bearer;
    if (status) *status = 200;
    g_status = 200;

    if (strstr(url, "/token")) {
        return strdup("{\"access_token\":\"test-token\",\"expires_in\":3600}");
    }

    if (strcmp(method, "GET") == 0) {
        if (strstr(url, "fields=spreadsheetId")) {
            return strdup("{\"spreadsheetId\":\"SHEET123\"}");
        }
        if (strstr(url, "/values/")) {
            if (strstr(url, "10.2026")) return strdup(MONTH_JSON);
            if (strstr(url, "09.2026") && g_created_sep)
                return strdup("{\"values\":[]}");
            if (status) *status = 404;
            g_status = 404;
            return strdup("{\"error\":{\"code\":404,\"message\":\"not found\"}}");
        }
        return strdup("{}");
    }

    if (strcmp(method, "PUT") == 0) {
        g_put_count++;
        free(g_last_put);
        free(g_last_put_url);
        g_last_put = strdup(body ? body : "");
        g_last_put_url = strdup(url ? url : "");
        return strdup("{\"updatedCells\":6}");
    }

    if (strcmp(method, "POST") == 0 && strstr(url, "batchUpdate")) {
        if (body && strstr(body, "09.2026")) g_created_sep = 1;
        return strdup("{\"replies\":[]}");
    }

    if (status) *status = 500;
    g_status = 500;
    return strdup("{\"error\":{\"message\":\"unexpected\"}}");
}

static int fails = 0;
static void check(int cond, const char *what) {
    if (cond) {
        printf("OK   %s\n", what);
    } else {
        printf("FAIL %s\n", what);
        fails++;
    }
}
int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s /path/to/rsa_key.pem\n", argv[0]);
        return 2;
    }

    FILE *kf = fopen(argv[1], "rb");
    if (!kf) { perror("key"); return 2; }
    static char pem[8192];
    size_t pn = fread(pem, 1, sizeof(pem) - 1, kf);
    fclose(kf);
    pem[pn] = '\0';

    static char esc[16384];
    size_t e = 0;
    for (size_t i = 0; i < pn && e + 3 < sizeof(esc); i++) {
        if (pem[i] == '\n') { esc[e++] = '\\'; esc[e++] = 'n'; }
        else if (pem[i] == '"') { esc[e++] = '\\'; esc[e++] = '"'; }
        else esc[e++] = pem[i];
    }
    esc[e] = '\0';

    static char sa[24576];
    snprintf(sa, sizeof(sa),
        "{\"type\":\"service_account\",\"client_email\":\"test@example.com\","
        "\"private_key\":\"%s\",\"token_uri\":\"http://fake/token\"}", esc);

    setenv("GOOGLE_SERVICE_ACCOUNT_JSON", sa, 1);
    setenv("TRACKING_SHEET_ID", "SHEET123", 1);
    setenv("SEARCH_NAME", "Иванов Иван", 1);
    setenv("HOURLY_RATE", "500", 1);
    setenv("TZ_OFFSET_SECONDS", "10800", 1);
    setenv("GOOGLE_API_BASE", "http://fake", 1);
    setenv("GOOGLE_DRIVE_BASE", "http://fake", 1);

    char out[16384];
    int code;

    code = tracking_days_json("Октябрь 2026", out, sizeof(out));
    printf("days: %d %s\n", code, out);
    check(code == 200, "GET days: код 200");
    check(strstr(out, "\"month\":\"10.2026\"") != NULL, "GET days: месяц 10.2026");
    check(strstr(out, "\"5\":{\"in\":\"07:30\",\"out\":\"14:40\"") != NULL, "GET days: день 5 отмечен");
    check(strstr(out, "\"6\":{\"in\":\"08:00\",\"out\":\"\"") != NULL, "GET days: день 6 без ухода");
    check(!strstr(out, "\"5\":{\"in\":\"09:00\""), "GET days: чужой сотрудник не попал");

    int before = g_put_count;
    code = tracking_fill_day("Октябрь 2026", 7, "7:30", "14:40", out, sizeof(out));
    printf("fill 7: %d %s\n", code, out);
    check(code == 200, "fill: новый день записан");
    check(g_put_count == before + 1, "fill: одна запись в таблицу");
    check(strstr(out, "\"hours\":\"7.17\"") && strstr(out, "\"sum\":\"3585.00\""),
          "fill: часы и сумма рассчитаны");
    check(g_last_put && strstr(g_last_put, "\"07.10.2026\",\"07:30\",\"14:40\",\"7.17\",\"500.00\",\"3585.00\""),
          "fill: строка 07.10 записана в нужные колонки");
    check(g_last_put && strstr(g_last_put, "\"05.10.2026\",\"07:30\",\"14:40\""),
          "fill: существующие строки сохранены");

    before = g_put_count;
    code = tracking_fill_day("10.2026", 5, "07:30", "14:40", out, sizeof(out));
    printf("fill 5 repeat: %d %s\n", code, out);
    check(code == 409, "fill: повторная отметка запрещена");
    check(g_put_count == before, "fill: при 409 запись не выполняется");

    code = tracking_fill_day("Октябрь 2026", 6, "08:00", "15:00", out, sizeof(out));
    printf("fill 6: %d %s\n", code, out);
    check(code == 200, "fill: незавершённый день дополнен");
    check(g_last_put && strstr(g_last_put, "\"06.10.2026\",\"08:00\",\"15:00\",\"7.00\""),
          "fill: уход и часы проставлены");

    code = tracking_fill_day("Январь 2030", 1, "07:30", "14:40", out, sizeof(out));
    printf("fill future: %d %s\n", code, out);
    check(code == 400 && strstr(out, "будущую"), "fill: будущая дата отклонена");

    code = tracking_fill_day("Октябрь 2026", 7, "25:00", "14:40", out, sizeof(out));
    printf("fill bad time: %d %s\n", code, out);
    check(code == 400 && strstr(out, "неверное время"), "fill: неверное время отклонено");

    code = tracking_fill_day("Ноябрь 2026", 31, "07:30", "14:40", out, sizeof(out));
    printf("fill bad day: %d %s\n", code, out);
    check(code == 400 && strstr(out, "неверный день"), "fill: несуществующий день отклонён");

    before = g_put_count;
    code = tracking_fill_day("Сентябрь 2026", 10, "07:30", "14:40", out, sizeof(out));
    printf("fill sep: %d %s\n", code, out);
    check(code == 200, "fill: прошлый месяц записан");
    check(g_created_sep, "fill: вкладка прошлого месяца создана");
    check(g_put_count == before + 1, "fill: запись в новую вкладку");
    check(g_last_put_url && strstr(g_last_put_url, "09.2026"), "fill: запись ушла в 09.2026");
    check(g_last_put && strstr(g_last_put, "\"Дата\",\"Время прихода\",\"Время ухода\""),
          "fill: заголовки новой вкладки записаны");
    check(g_last_put && strstr(g_last_put, "\"10.09.2026\",\"07:30\",\"14:40\""),
          "fill: строка 10.09 записана");

    char *p1 = query_param("/api/tracking/fill?month=10.2026&day=5&in=07:30&out=14:40", "in");
    char *p2 = query_param("/api/tracking/fill?month=10.2026&day=5&min=bad", "in");
    check(p1 && strcmp(p1, "07:30") == 0, "query: параметр in найден");
    check(!p2, "query: in не находится внутри min");
    free(p1); free(p2);

    printf(fails ? "\nFAILED: %d\n" : "\nALL TESTS PASSED\n", fails);
    return fails ? 1 : 0;
}


#include "infra/google_sheets.h"

#include "core/json_text.h"
#include "core/url_codec.h"
#include "infra/http_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void google_sheets_init(GoogleSheets *sheets) {
    if (!sheets) return;
    sheets->sheet_id = NULL;
    sheets->last_error[0] = '\0';
    sheets->last_get_status = 0;
    sheets->last_get_resp[0] = '\0';
    sheets->last_create_status = 0;
    sheets->last_create_resp[0] = '\0';

    google_auth_init(&sheets->auth, NULL);

    const char *base = getenv("GOOGLE_API_BASE");
    snprintf(sheets->api_base, sizeof(sheets->api_base), "%s",
             (base && *base) ? base : "https://sheets.googleapis.com");
    const char *dbase = getenv("GOOGLE_DRIVE_BASE");
    snprintf(sheets->drive_base, sizeof(sheets->drive_base), "%s",
             (dbase && *dbase) ? dbase : "https://www.googleapis.com");
}

/* Google иногда отдаёт тело ошибки с HTTP-кодом 200 (через прокси).
   Проверяем наличие поля "error" в JSON, иначе можно принять ошибку
   за успешный ответ и решить, что запись выполнена. */
static int resp_has_error(const char *resp) {
    if (!resp) return 0;
    const char *p = strstr(resp, "\"error\"");
    while (p) {
        const char *q = p + 7;
        while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
        if (*q == ':') return 1;
        p = strstr(p + 7, "\"error\"");
    }
    return 0;
}

/* Проверяет, содержит ли ответ ошибку "API не включён", и заполняет
   last_error понятным текстом. Возвращает 1, если это тот случай. */
static int detect_api_disabled(GoogleSheets *sheets, const char *resp) {
    if (!resp) return 0;
    if (strstr(resp, "SERVICE_DISABLED") ||
        (strstr(resp, "has not been used in project") && strstr(resp, "or it is disabled"))) {
        const char *p = strstr(resp, "consumer\": \"projects/");
        char proj[64] = "";
        if (p) {
            p += 21;
            size_t i = 0;
            while (*p && *p != '"' && i < sizeof(proj) - 1) proj[i++] = *p++;
            proj[i] = '\0';
        }
        snprintf(sheets->last_error, sizeof(sheets->last_error),
                 "Google Sheets API отключён в проекте %s. "
                 "Включите его: https://console.developers.google.com/apis/api/"
                 "sheets.googleapis.com/overview?project=%s",
                 proj[0] ? proj : "(unknown)", proj[0] ? proj : "YOUR_PROJECT");
        fprintf(stderr, "[track] %s\n", sheets->last_error);
        return 1;
    }
    return 0;
}

/* ---------- чтение вкладки ---------- */

char *google_sheets_get_values(GoogleSheets *sheets, const char *tab) {
    if (!sheets || !tab) return NULL;

    /* Чтение возможно и по публичному API key (если таблица доступна на
       чтение), запись — только через OAuth2 Service Account. Пробуем сначала OAuth2. */
    const char *token = google_auth_token(&sheets->auth);
    if (!token) {
        const char *apikey = getenv("GOOGLE_API_KEY");
        if (apikey && *apikey) {
            char url2[2048];
            char *enc2 = url_encode(tab);
            if (enc2) {
                snprintf(url2, sizeof(url2),
                    "%s/v4/spreadsheets/%s/values/%%27%s%%27!A:F?key=%s",
                    sheets->api_base,
                    getenv("TRACKING_SHEET_ID") ? getenv("TRACKING_SHEET_ID") : "",
                    enc2, apikey);
                free(enc2);
                int st2 = 0;
                char *r2 = http_request_full("GET", url2, NULL, NULL, NULL, &st2);
                sheets->last_get_status = st2;
                if (r2) snprintf(sheets->last_get_resp, sizeof(sheets->last_get_resp),
                                 "%.400s", r2);
                if (r2 && st2 == 200 && !resp_has_error(r2)) return r2;
                free(r2);
            }
        }
        return NULL;
    }

    char url[2048];
    /* Имя листа с пробелами оборачиваем в одинарные кавычки (A1-нотация),
       кодируем только его — суффикс !A:F оставляем как есть. */
    char *enc_sheet = url_encode(tab);
    if (!enc_sheet) return NULL;
    snprintf(url, sizeof(url),
        "%s/v4/spreadsheets/%s/values/%%27%s%%27!A:F",
        sheets->api_base, sheets->sheet_id ? sheets->sheet_id : "", enc_sheet);
    free(enc_sheet);

    int status = 0;
    char *resp = http_request_full("GET", url, NULL, NULL, token, &status);
    if (!resp) return NULL;
    sheets->last_get_status = status;
    snprintf(sheets->last_get_resp, sizeof(sheets->last_get_resp), "%.400s", resp);
    if (status != 200 || resp_has_error(resp)) {
        int parse_range = (strstr(resp, "Unable to parse range") != NULL);
        if (!parse_range) {
            if (!detect_api_disabled(sheets, resp))
                snprintf(sheets->last_error, sizeof(sheets->last_error),
                         "ошибка чтения таблицы: код %d, ответ %.160s", status, resp);
            fprintf(stderr, "[track] %s\n", sheets->last_error);
        } else {
            snprintf(sheets->last_error, sizeof(sheets->last_error),
                     "вкладки '%s' ещё нет", tab);
        }
        free(resp);
        return NULL;
    }
    return resp;
}

/* ---------- добавление строки ---------- */

int google_sheets_append_row(GoogleSheets *sheets, const char *tab,
                              const char *values_json) {
    const char *token = google_auth_token(&sheets->auth);
    if (!token) return -1;

    char url[2048];
    char *enc_sheet = url_encode(tab);
    if (!enc_sheet) return -1;
    snprintf(url, sizeof(url),
        "%s/v4/spreadsheets/%s/values/%%27%s%%27!A:F:append"
        "?valueInputOption=RAW&insertDataOption=INSERT_ROWS",
        sheets->api_base, sheets->sheet_id ? sheets->sheet_id : "", enc_sheet);
    free(enc_sheet);

    char body[4096];
    snprintf(body, sizeof(body), "{\"values\":[%s]}", values_json);

    int status = 0;
    char *resp = http_request_full("POST", url, "application/json", body, token, &status);
    if (!resp) return -1;
    if (status != 200 || resp_has_error(resp)) {
        if (!detect_api_disabled(sheets, resp))
            snprintf(sheets->last_error, sizeof(sheets->last_error),
                     "ошибка добавления строки: код %d, ответ %.160s", status, resp);
        fprintf(stderr, "[track] %s\n", sheets->last_error);
        free(resp);
        return -1;
    }
    fprintf(stderr, "[track] appended row to '%s'\n", tab);
    free(resp);
    return 0;
}

/* ---------- обновление диапазона ---------- */

int google_sheets_update_values(GoogleSheets *sheets, const char *tab,
                                 const char *a1_suffix, const char *values_object) {
    const char *token = google_auth_token(&sheets->auth);
    if (!token) return -1;

    char url[2048];
    char *enc = url_encode(tab);
    if (!enc) return -1;
    snprintf(url, sizeof(url),
        "%s/v4/spreadsheets/%s/values/%%27%s%%27%s"
        "?valueInputOption=RAW",
        sheets->api_base, sheets->sheet_id ? sheets->sheet_id : "", enc, a1_suffix);
    free(enc);

    int status = 0;
    char *resp = http_request_full("PUT", url, "application/json", values_object,
                                   token, &status);
    if (!resp) return -1;
    if (status != 200 || resp_has_error(resp)) {
        if (!detect_api_disabled(sheets, resp))
            snprintf(sheets->last_error, sizeof(sheets->last_error),
                     "ошибка записи значений: код %d, ответ %.160s", status, resp);
        fprintf(stderr, "[track] %s\n", sheets->last_error);
        free(resp);
        return -1;
    }
    free(resp);
    return 0;
}

int google_sheets_update_range(GoogleSheets *sheets, const char *tab,
                                const char *a1_suffix, const char *values_json) {
    char body[4096];
    snprintf(body, sizeof(body), "{\"values\":[%s]}", values_json);
    return google_sheets_update_values(sheets, tab, a1_suffix, body);
}

/* ---------- создание вкладки ---------- */

int google_sheets_create_sheet(GoogleSheets *sheets, const char *tab) {
    const char *token = google_auth_token(&sheets->auth);
    if (!token) return -1;

    char url[2048];
    snprintf(url, sizeof(url),
        "%s/v4/spreadsheets/%s:batchUpdate",
        sheets->api_base, sheets->sheet_id ? sheets->sheet_id : "");

    char body[2048];
    char *esc = json_escape(tab);
    snprintf(body, sizeof(body),
        "{\"requests\":[{\"addSheet\":{\"properties\":{\"title\":\"%s\"}}}]}",
        esc ? esc : tab);
    free(esc);

    int status = 0;
    char *resp = http_request_full("POST", url, "application/json", body, token, &status);
    if (!resp) return -1;
    sheets->last_create_status = status;
    snprintf(sheets->last_create_resp, sizeof(sheets->last_create_resp), "%.400s", resp);
    if (status != 200 || resp_has_error(resp)) {
        /* Вкладка уже существует — это не ошибка */
        if (strstr(resp, "already exists")) {
            fprintf(stderr, "[track] sheet '%s' already exists\n", tab);
            free(resp);
            return 0;
        }
        if (!detect_api_disabled(sheets, resp))
            snprintf(sheets->last_error, sizeof(sheets->last_error),
                     "ошибка создания листа: код %d, ответ %.160s", status, resp);
        fprintf(stderr, "[track] %s\n", sheets->last_error);
        free(resp);
        return -1;
    }
    fprintf(stderr, "[track] created sheet '%s'\n", tab);
    free(resp);
    return 0;
}

/* ---------- список вкладок ---------- */

int google_sheets_list_tabs(GoogleSheets *sheets, char ***out, int *count, int max) {
    if (!sheets || !out || !count || max <= 0) return -1;
    *out = NULL;
    *count = 0;

    const char *token = google_auth_token(&sheets->auth);
    if (!token) return -1;

    char url[2048];
    snprintf(url, sizeof(url),
             "%s/v4/spreadsheets/%s?fields=sheets.properties.title",
             sheets->api_base, sheets->sheet_id ? sheets->sheet_id : "");
    int status = 0;
    char *resp = http_request_full("GET", url, NULL, NULL, token, &status);
    if (!resp || status != 200 || resp_has_error(resp)) { free(resp); return -1; }

    char **list = calloc((size_t)max, sizeof(char *));
    if (!list) { free(resp); return -1; }
    int n = 0;
    const char *p = resp;
    while (n < max && (p = strstr(p, "\"title\"")) != NULL) {
        p = strchr(p, ':');
        if (!p) break;
        p++;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (*p != '"') break;
        p++;
        const char *st = p;
        while (*p && *p != '"') p++;
        size_t len = (size_t)(p - st);
        char *name = malloc(len + 1);
        if (!name) break;
        memcpy(name, st, len);
        name[len] = '\0';
        list[n++] = name;
        p++;
    }
    free(resp);
    *out = list;
    *count = n;
    return 0;
}

/* ---------- создание таблицы и выдача доступа ---------- */

static char *sheets_create_spreadsheet(GoogleSheets *sheets, const char *title) {
    const char *token = google_auth_token(&sheets->auth);
    if (!token) return NULL;

    char body[1024];
    char *esc = json_escape(title);
    snprintf(body, sizeof(body),
        "{\"properties\":{\"title\":\"%s\",\"locale\":\"ru_RU\","
        "\"autoRecalc\":\"ON_CHANGE\"}}",
        esc ? esc : title);
    free(esc);

    int status = 0;
    char url[512];
    snprintf(url, sizeof(url), "%s/v4/spreadsheets", sheets->api_base);
    char *resp = http_request_full("POST", url, "application/json", body, token, &status);
    if (!resp) return NULL;
    if (status != 200 || resp_has_error(resp)) {
        if (!detect_api_disabled(sheets, resp))
            snprintf(sheets->last_error, sizeof(sheets->last_error),
                     "ошибка создания таблицы: код %d, ответ %.160s", status, resp);
        fprintf(stderr, "[track] %s\n", sheets->last_error);
        free(resp);
        return NULL;
    }
    char *id = json_field_string(resp, "spreadsheetId");
    char *surl = json_field_string(resp, "spreadsheetUrl");
    if (id) {
        fprintf(stderr, "[track] created spreadsheet '%s' id=%s url=%s\n",
                title, id, surl ? surl : "?");
        /* сохраняем ID, чтобы пережить перезапуск */
        FILE *f = fopen("/tmp/tracking_sheet_id", "w");
        if (f) { fprintf(f, "%s\n", id); fclose(f); }
    }
    free(surl);
    free(resp);
    return id;
}

/* Выдаёт доступ на редактирование указанному email. */
static int drive_share(GoogleSheets *sheets, const char *file_id,
                       const char *email, const char *role) {
    const char *token = google_auth_token(&sheets->auth);
    if (!token || !email || !*email) return -1;

    char url[2048];
    snprintf(url, sizeof(url),
        "%s/drive/v3/files/%s/permissions"
        "?sendNotificationEmail=false", sheets->drive_base, file_id);

    char body[1024];
    char *ee = json_escape(email);
    snprintf(body, sizeof(body),
        "{\"role\":\"%s\",\"type\":\"user\",\"emailAddress\":\"%s\"}",
        role ? role : "writer", ee ? ee : email);
    free(ee);

    int status = 0;
    char *resp = http_request_full("POST", url, "application/json", body, token, &status);
    if (!resp) return -1;
    if ((status != 200 && status != 201) || resp_has_error(resp)) {
        fprintf(stderr, "[track] drive_share status=%d resp=%.300s\n", status, resp);
        free(resp);
        return -1;
    }
    fprintf(stderr, "[track] shared '%s' with %s as %s\n",
            file_id, email, role ? role : "writer");
    free(resp);
    return 0;
}

/* Проверяет, что таблица существует и доступна. */
static int sheets_sheet_exists(GoogleSheets *sheets, const char *file_id) {
    const char *token = google_auth_token(&sheets->auth);
    if (!token) return 0;
    char url[2048];
    snprintf(url, sizeof(url),
        "%s/v4/spreadsheets/%s?fields=spreadsheetId",
        sheets->api_base, file_id);
    int status = 0;
    char *resp = http_request_full("GET", url, NULL, NULL, token, &status);
    if (!resp) return 0;
    int ok = (status == 200 && !resp_has_error(resp));
    if (!ok) detect_api_disabled(sheets, resp);
    free(resp);
    return ok;
}

int google_sheets_ensure(GoogleSheets *sheets) {
    if (!sheets) return -1;
    const char *env_id = getenv("TRACKING_SHEET_ID");
    const char *share_email = getenv("TRACKING_SHARE_EMAIL");
    const char *title = getenv("TRACKING_SHEET_TITLE");
    if (!title || !*title) title = "Time tracking";

    /* 1) ID из окружения */
    if (env_id && *env_id) {
        if (sheets_sheet_exists(sheets, env_id)) {
            free(sheets->sheet_id);
            sheets->sheet_id = strdup(env_id);
            if (share_email && *share_email) drive_share(sheets, env_id, share_email, "writer");
            return 0;
        }
        snprintf(sheets->last_error, sizeof(sheets->last_error),
                 "TRACKING_SHEET_ID=%s недоступен с текущими учётными данными. "
                 "Проверьте доступ Service Account к таблице.", env_id);
        fprintf(stderr, "[track] %s\n", sheets->last_error);
        /* Если ID задан явно — не создаём другую таблицу: иначе записи
           уйдут в новый файл, а пользователь ищет их в своей таблице. */
        return -1;
    }

    /* 2) ID из локального кэша (создан ранее этим же сервис-аккаунтом) */
    FILE *cf = fopen("/tmp/tracking_sheet_id", "r");
    if (cf) {
        char cached[128] = {0};
        if (fgets(cached, sizeof(cached), cf)) {
            char *nl = strchr(cached, '\n');
            if (nl) *nl = '\0';
            if (*cached && sheets_sheet_exists(sheets, cached)) {
                fclose(cf);
                free(sheets->sheet_id);
                sheets->sheet_id = strdup(cached);
                fprintf(stderr, "[track] using cached spreadsheet id=%s\n", cached);
                if (share_email && *share_email) drive_share(sheets, cached, share_email, "writer");
                return 0;
            }
        }
        fclose(cf);
    }

    /* 3) создаём новую таблицу */
    char *new_id = sheets_create_spreadsheet(sheets, title);
    if (!new_id) {
        if (!sheets->last_error[0])
            snprintf(sheets->last_error, sizeof(sheets->last_error),
                     "не удалось создать таблицу (нужен Service Account с доступом "
                     "к spreadsheets)");
        if (strstr(sheets->last_error, "does not have permission"))
            snprintf(sheets->last_error, sizeof(sheets->last_error),
                     "Service Account не может создавать таблицы. Задайте "
                     "TRACKING_SHEET_ID — ID существующей таблицы Time tracking.");
        fprintf(stderr, "[track] %s\n", sheets->last_error);
        return -1;
    }
    free(sheets->sheet_id);
    sheets->sheet_id = new_id;

    /* 4) выдаём доступ владельцу */
    if (share_email && *share_email) {
        if (drive_share(sheets, sheets->sheet_id, share_email, "writer") != 0)
            fprintf(stderr, "[track] WARNING: could not share with %s — share manually\n",
                    share_email);
    }
    return 0;
}

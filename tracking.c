/*
 * tracking.c — учёт рабочего времени в Google Sheets ("Time tracking").
 *
 * Структура листа (вкладка = текущий месяц и год, например "Октябрь 2026"):
 *   A: Дата | B: Время прихода | C: Время ухода | D: Время в часах
 *   E: Ставка в час | F: Сумма за день
 * Блоки разных сотрудников разделены пустой строкой.
 *
 * Запись выполняется через OAuth2 (Service Account), т.к. Google Sheets API
 * запрещает изменение данных по API key.
 */

#include "tracking.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <stdarg.h>
#include <sys/stat.h>

#define TRK_BUF       (256 * 1024)
#define TRK_KEYFILE   "/tmp/trk_sa_key.pem"
#define TRK_INFILE    "/tmp/trk_jwt_in.txt"
#define TRK_SIGFILE   "/tmp/trk_jwt_sig.bin"
#define TRK_INITFILE  "/tmp/trk_initdata.txt"

static char *g_sa_json = NULL;
static char *g_sheet_id = NULL;
static char *g_emp_map = NULL;
static char *g_rates_map = NULL;
static double g_default_rate = 0.0;
static int g_tz_offset = 3 * 3600;   /* локальное время = UTC + 3ч */

static char *g_token = NULL;
static time_t g_token_exp = 0;
static int g_inited = 0;
static char g_current_sheet[256] = "";
/* Базовые URL Google API (переопределяются через окружение — используется в тестах) */
static char *g_api_base = NULL;
static char *g_drive_base = NULL;
/* Последняя ошибка (для диагностики) */
static char g_last_error[512] = "";

static void trk_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[track] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

/* ---------- base64url (без padding) ---------- */
static const char B64URL[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static char *b64url(const unsigned char *in, size_t len) {
    size_t olen = 4 * ((len + 2) / 3) + 1;
    char *out = malloc(olen);
    if (!out) return NULL;
    size_t i = 0, o = 0;
    while (i < len) {
        unsigned int v = (unsigned int)in[i] << 16;
        if (i + 1 < len) v |= (unsigned int)in[i + 1] << 8;
        if (i + 2 < len) v |= (unsigned int)in[i + 2];
        out[o++] = B64URL[(v >> 18) & 0x3F];
        out[o++] = B64URL[(v >> 12) & 0x3F];
        out[o++] = (i + 1 < len) ? B64URL[(v >> 6) & 0x3F] : '=';
        out[o++] = (i + 2 < len) ? B64URL[v & 0x3F] : '=';
        i += 3;
    }
    out[o] = '\0';
    while (o && out[o - 1] == '=') out[--o] = '\0';
    return out;
}

/* ---------- извлечение строкового поля из JSON с раскодировкой escape-последовательностей ---------- */
static char *json_get_string(const char *json, const char *key) {
    if (!json || !key) return NULL;
    char pat[192];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return NULL;
    p += strlen(pat);
    while (*p && *p != ':') p++;
    if (!*p) return NULL;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') return NULL;
    p++;
    char *out = malloc(strlen(p) + 1);
    if (!out) return NULL;
    size_t o = 0;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
                case 'n': out[o++] = '\n'; break;
                case 't': out[o++] = '\t'; break;
                case 'r': out[o++] = '\r'; break;
                case 'b': out[o++] = '\b'; break;
                case 'f': out[o++] = '\f'; break;
                case '"': out[o++] = '"';  break;
                case '\\':out[o++] = '\\'; break;
                case '/': out[o++] = '/';  break;
                case 'u': {
                    char hex[5] = {0};
                    for (int k = 0; k < 4 && p[1]; k++) hex[k] = *++p;
                    long cp = strtol(hex, NULL, 16);
                    if (cp < 0x80) {
                        out[o++] = (char)cp;
                    } else if (cp < 0x800) {
                        out[o++] = (char)(0xC0 | (cp >> 6));
                        out[o++] = (char)(0x80 | (cp & 0x3F));
                    } else {
                        out[o++] = (char)(0xE0 | (cp >> 12));
                        out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        out[o++] = (char)(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: out[o++] = *p;
            }
            p++;
        } else {
            out[o++] = *p++;
        }
    }
    out[o] = '\0';
    return out;
}

/* ---------- URL-кодирование ---------- */
static char *trk_url_encode(const char *src) {
    size_t len = strlen(src);
    char *dst = malloc(len * 3 + 1);
    if (!dst) return NULL;
    char *d = dst;
    for (const unsigned char *s = (const unsigned char *)src; *s; s++) {
        if ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') ||
            (*s >= '0' && *s <= '9') || *s == '-' || *s == '_' ||
            *s == '.' || *s == '~') {
            *d++ = (char)*s;
        } else {
            d += sprintf(d, "%%%02X", *s);
        }
    }
    *d = '\0';
    return dst;
}

/* ---------- URL-декодирование ---------- */
static char *trk_url_decode(const char *src) {
    size_t len = strlen(src);
    char *dst = malloc(len + 1);
    if (!dst) return NULL;
    char *d = dst;
    for (size_t i = 0; i < len; i++) {
        if (src[i] == '%' && i + 2 < len && isxdigit((unsigned char)src[i+1]) &&
            isxdigit((unsigned char)src[i+2])) {
            char hex[3] = {src[i+1], src[i+2], 0};
            *d++ = (char)strtol(hex, NULL, 16);
            i += 2;
        } else if (src[i] == '+') {
            *d++ = ' ';
        } else {
            *d++ = src[i];
        }
    }
    *d = '\0';
    return dst;
}

/* ---------- Экранирование строки для JSON ---------- */
static char *json_escape_local(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char *out = malloc(len * 2 + 1);
    if (!out) return NULL;
    char *d = out;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"':  *d++ = '\\'; *d++ = '"';  break;
            case '\\': *d++ = '\\'; *d++ = '\\'; break;
            case '\n': *d++ = '\\'; *d++ = 'n';  break;
            case '\r': *d++ = '\\'; *d++ = 'r';  break;
            case '\t': *d++ = '\\'; *d++ = 't';  break;
            default:
                if (c < 0x20) { d += sprintf(d, "\\u%04x", c); }
                else *d++ = (char)c;
        }
    }
    *d = '\0';
    return out;
}

/* ---------- HTTP-запрос через curl ---------- */
/* Возвращает тело ответа; *status — HTTP-код (0 при ошибке). */
static char *trk_curl(const char *method, const char *url,
                      const char *content_type, const char *body,
                      const char *bearer, int *status) {
    if (status) *status = 0;
    /* ВАЖНО: учитываем длину всех частей команды, иначе snprintf обрежет
       строку (например, длинный Bearer-токен) и shell получит битую команду. */
    size_t cmdlen = strlen(url) + (body ? strlen(body) : 0) +
                     (bearer ? strlen(bearer) : 0) +
                     (content_type ? strlen(content_type) : 0) + 2048;
    char *cmd = malloc(cmdlen);
    if (!cmd) return NULL;

    /* Тело и заголовки оборачиваем в одинарные кавычки, экранируя ' */
    char *qbody = NULL;
    if (body) {
        size_t bl = strlen(body);
        qbody = malloc(bl * 4 + 16);
        if (!qbody) { free(cmd); return NULL; }
        size_t o = 0;
        qbody[o++] = '\'';
        for (size_t i = 0; i < bl; i++) {
            if (body[i] == '\'') {
                qbody[o++] = '\''; qbody[o++] = '\\'; qbody[o++] = '\''; qbody[o++] = '\'';
            } else {
                qbody[o++] = body[i];
            }
        }
        qbody[o++] = '\'';
        qbody[o] = '\0';
    }

    size_t o = 0;
    /* APPEND безопасно: если места не хватит — команда обрежется, но не выйдет
       за пределы буфера и не станет «битой». */
    #define APPEND(...) do { \
        int _n = snprintf(cmd + o, cmdlen - o, __VA_ARGS__); \
        if (_n < 0) break; \
        size_t _need = (size_t)_n; \
        if (_need >= cmdlen - o) { o = cmdlen - 1; break; } \
        o += _need; \
    } while(0)

    APPEND("curl -sS --max-time 25 -X %s -w '\\n%%{http_code}' ", method);
    if (bearer && *bearer)
        APPEND("-H 'Authorization: Bearer %s' ", bearer);
    if (content_type)
        APPEND("-H 'Content-Type: %s' ", content_type);
    if (qbody)
        APPEND("--data-binary %s ", qbody);
    APPEND("'%s'", url);
    #undef APPEND

    FILE *fp = popen(cmd, "r");
    free(cmd);
    free(qbody);
    if (!fp) return NULL;

    char *buf = malloc(TRK_BUF);
    if (!buf) { pclose(fp); return NULL; }
    size_t total = 0, n;
    while ((n = fread(buf + total, 1, TRK_BUF - total - 1, fp)) > 0) {
        total += n;
        if (total >= TRK_BUF - 1) break;
    }
    buf[total] = '\0';
    pclose(fp);

    /* последняя строка — HTTP-код */
    char *last = strrchr(buf, '\n');
    if (last) {
        if (status) *status = atoi(last + 1);
        *last = '\0';
    }
    return buf;
}

/* ---------- base64 стандартный (нужен для сверки hash из Telegram initData) ---------- */
static const char B64STD[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *b64std(const unsigned char *in, size_t len) {
    size_t olen = 4 * ((len + 2) / 3) + 1;
    char *out = malloc(olen);
    if (!out) return NULL;
    size_t i = 0, o = 0;
    while (i < len) {
        unsigned int v = (unsigned int)in[i] << 16;
        if (i + 1 < len) v |= (unsigned int)in[i + 1] << 8;
        if (i + 2 < len) v |= (unsigned int)in[i + 2];
        out[o++] = B64STD[(v >> 18) & 0x3F];
        out[o++] = B64STD[(v >> 12) & 0x3F];
        out[o++] = (i + 1 < len) ? B64STD[(v >> 6) & 0x3F] : '=';
        out[o++] = (i + 2 < len) ? B64STD[v & 0x3F] : '=';
        i += 3;
    }
    out[o] = '\0';
    return out;
}

static void hexify(const unsigned char *in, size_t len, char *out) {
    static const char *H = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2]     = H[in[i] >> 4];
        out[i * 2 + 1] = H[in[i] & 0x0F];
    }
    out[len * 2] = '\0';
}

/* Выполняет openssl и возвращает бинарный вывод (hex-строка на входе как ключ) */
static unsigned char *run_openssl_bin(const char *args_template, const char *input_file,
                                      size_t *out_len) {
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "%s", args_template);
    (void)input_file;
    FILE *fp = popen(cmd, "r");
    if (!fp) return NULL;
    unsigned char *buf = malloc(64 * 1024);
    if (!buf) { pclose(fp); return NULL; }
    size_t total = 0, n;
    while ((n = fread(buf + total, 1, 64 * 1024 - total - 1, fp)) > 0) {
        total += n;
        if (total >= 64 * 1024 - 1) break;
    }
    pclose(fp);
    if (out_len) *out_len = total;
    return buf;
}

/* HMAC-SHA256: hexkey — hex-представление ключа, data_file — файл с данными.
   Возвращает 32 байта. */
static unsigned char *hmac_sha256(const char *hexkey, const char *data_file,
                                  size_t *out_len) {
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
        "openssl dgst -sha256 -mac HMAC -macopt hexkey:%s -binary '%s' 2>/dev/null",
        hexkey, data_file);
    return run_openssl_bin(cmd, data_file, out_len);
}

/* ==================================================================
 *  OAuth2: Service Account -> access token (RS256 JWT)
 * ================================================================== */

/* Кэширует access_token до истечения срока */
static const char *sa_access_token(void) {
    if (g_token && time(NULL) < g_token_exp - 60) return g_token;

    char *email = json_get_string(g_sa_json, "client_email");
    char *pkey  = json_get_string(g_sa_json, "private_key");
    char *turi  = json_get_string(g_sa_json, "token_uri");
    if (!email || !pkey) {
        snprintf(g_last_error, sizeof(g_last_error),
                 "service account JSON invalid: client_email=%s private_key=%s",
                 email ? "ok" : "MISSING", pkey ? "ok" : "MISSING");
        trk_log("%s", g_last_error);
        free(email); free(pkey); free(turi);
        return NULL;
    }
    if (!turi) turi = strdup("https://oauth2.googleapis.com/token");

    /* сохраняем приватный ключ в PEM-файл (в JSON переносы строк как \n) */
    FILE *kp = fopen(TRK_KEYFILE, "w");
    if (!kp) { free(email); free(pkey); free(turi); return NULL; }
    fputs(pkey, kp);
    fclose(kp);
    chmod(TRK_KEYFILE, 0600);

    /* JWT header + payload */
    char header[] = "{\"alg\":\"RS256\",\"typ\":\"JWT\"}";
    char payload[1024];
    time_t now = time(NULL);
    snprintf(payload, sizeof(payload),
        "{\"iss\":\"%s\",\"scope\":\"https://www.googleapis.com/auth/spreadsheets\","
        "\"aud\":\"https://oauth2.googleapis.com/token\",\"iat\":%ld,\"exp\":%ld}",
        email, (long)now, (long)(now + 3600));

    char *h = b64url((const unsigned char *)header, strlen(header));
    char *p = b64url((const unsigned char *)payload, strlen(payload));
    if (!h || !p) { free(email); free(pkey); free(turi); free(h); free(p); return NULL; }

    char signing[2048];
    snprintf(signing, sizeof(signing), "%s.%s", h, p);

    FILE *in = fopen(TRK_INFILE, "w");
    if (!in) { free(email); free(pkey); free(turi); free(h); free(p); return NULL; }
    fputs(signing, in);
    fclose(in);

    /* подпись RSA-SHA256 */
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
        "openssl dgst -sha256 -sign '%s' -out '%s' '%s' 2>&1", TRK_KEYFILE,
        TRK_SIGFILE, TRK_INFILE);
    if (system(cmd) != 0) {
        trk_log("openssl signing failed");
        free(email); free(pkey); free(turi); free(h); free(p);
        return NULL;
    }

    FILE *sf = fopen(TRK_SIGFILE, "rb");
    if (!sf) { free(email); free(pkey); free(turi); free(h); free(p); return NULL; }
    unsigned char sig[1024];
    size_t siglen = fread(sig, 1, sizeof(sig), sf);
    fclose(sf);

    char *sig_b64 = b64url(sig, siglen);
    if (!sig_b64) { free(email); free(pkey); free(turi); free(h); free(p); return NULL; }

    char jwt[3072];
    snprintf(jwt, sizeof(jwt), "%s.%s", signing, sig_b64);

    char body[4096];
    snprintf(body, sizeof(body),
        "grant_type=urn%%3Aietf%%3Aparams%%3Aoauth%%3Agrant-type%%3Ajwt-bearer"
        "&assertion=%s", jwt);

    int status = 0;
    char *resp = trk_curl("POST", turi, "application/x-www-form-urlencoded",
                          body, NULL, &status);
    free(email); free(pkey); free(turi); free(h); free(p); free(sig_b64);

    if (!resp || status != 200) {
        snprintf(g_last_error, sizeof(g_last_error),
                 "token request failed status=%d body=%.180s",
                 status, resp ? resp : "(no response)");
        trk_log("%s", g_last_error);
        free(resp);
        return NULL;
    }
    char *tok = json_get_string(resp, "access_token");
    /* срок жизни читаем ДО освобождения ответа (иначе use-after-free) */
    long exp = 3600;
    char *e = json_get_string(resp, "expires_in");
    if (e) { long v = atol(e); if (v > 60) exp = v; free(e); }
    free(resp);
    if (!tok) {
        snprintf(g_last_error, sizeof(g_last_error), "no access_token in response");
        trk_log("%s", g_last_error);
        return NULL;
    }

    free(g_token);
    g_token = tok;
    g_token_exp = now + exp - 120;
    trk_log("access token obtained");
    return g_token;
}

/* ==================================================================
 *  Работа с листом "Time tracking"
 * ================================================================== */

static const char *MONTH_NAMES[] = {
    "Январь", "Февраль", "Март", "Апрель", "Май", "Июнь",
    "Июль", "Август", "Сентябрь", "Октябрь", "Ноябрь", "Декабрь"
};

/* Текущий месяц и год по локальному времени -> "Октябрь 2026" */
static void current_month_name(char *out, size_t cap) {
    time_t now = time(NULL) + g_tz_offset;
    struct tm tm;
    gmtime_r(&now, &tm);
    snprintf(out, cap, "%s %d", MONTH_NAMES[tm.tm_mon], tm.tm_year + 1900);
}

/* Текущие дата и время по локальному времени -> "23.10.2026", "08:30" */
static void current_datetime(char *date, size_t dcap, char *time_s, size_t tcap) {
    time_t now = time(NULL) + g_tz_offset;
    struct tm tm;
    gmtime_r(&now, &tm);
    snprintf(date, dcap, "%02d.%02d.%d", tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900);
    snprintf(time_s, tcap, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

/* Значение поля из query-строки (после "name=") */
static char *query_param(const char *query, const char *name) {
    if (!query) return NULL;
    char pat[128];
    snprintf(pat, sizeof(pat), "%s=", name);
    const char *p = strstr(query, pat);
    if (!p) return NULL;
    p += strlen(pat);
    const char *end = strchr(p, '&');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    char *raw = malloc(len + 1);
    if (!raw) return NULL;
    memcpy(raw, p, len);
    raw[len] = '\0';
    char *dec = trk_url_decode(raw);
    free(raw);
    return dec;
}

/* Поиск сотрудника в EMPLOYEE_MAP: {"chat_id":"Имя Фамилия", ...} */
static char *employee_name_by_chat(const char *chat_id) {
    if (!g_emp_map || !chat_id) return NULL;
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\"", chat_id);
    const char *p = strstr(g_emp_map, pat);
    if (!p) return NULL;
    p += strlen(pat);
    while (*p && *p != ':') p++;
    if (!*p) return NULL;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') return NULL;
    p++;
    const char *e = p;
    while (*e && *e != '"') {
        if (*e == '\\' && e[1]) e++;
        e++;
    }
    size_t len = (size_t)(e - p);
    char *name = malloc(len + 1);
    if (!name) return NULL;
    memcpy(name, p, len);
    name[len] = '\0';
    return name;
}

/* Ставка для сотрудника: EMPLOYEE_MAP_RATES = {"Имя Фамилия":"500"} */
static double employee_rate_by_name(const char *name) {
    if (!g_rates_map || !name) return g_default_rate;
    char pat[320];
    snprintf(pat, sizeof(pat), "\"%s\"", name);
    const char *p = strstr(g_rates_map, pat);
    if (!p) return g_default_rate;
    p += strlen(pat);
    while (*p && *p != ':') p++;
    if (!*p) return g_default_rate;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') p++;
    double r = atof(p);
    return r > 0 ? r : g_default_rate;
}

/* Проверяет, содержит ли ответ Google ошибку "API не включён" и заполняет
   g_last_error понятным текстом. Возвращает 1, если это тот случай. */
static int detect_api_disabled(const char *resp) {
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
        snprintf(g_last_error, sizeof(g_last_error),
                 "Google Sheets API is DISABLED in project %s. "
                 "Enable it: https://console.developers.google.com/apis/api/sheets.googleapis.com/overview?project=%s",
                 proj[0] ? proj : "(unknown)", proj[0] ? proj : "YOUR_PROJECT");
        trk_log("%s", g_last_error);
        return 1;
    }
    return 0;
}

/* ---------- Google Sheets: чтение листа ---------- */
/* Возвращает JSON ответа values API или NULL. */
static char *sheets_get_values(const char *sheet_name) {
    /* Чтение возможно и по публичному API key (если таблица доступна на чтение),
       запись — только через OAuth2 Service Account. Пробуем сначала OAuth2. */
    const char *token = sa_access_token();
    if (!token) {
        const char *apikey = getenv("GOOGLE_API_KEY");
        if (apikey && *apikey) {
            char url2[2048];
            char *enc2 = trk_url_encode(sheet_name);
            if (enc2) {
                snprintf(url2, sizeof(url2),
                    "%s/v4/spreadsheets/%s/values/%%27%s%%27!A:F?key=%s",
                    g_api_base, getenv("TRACKING_SHEET_ID") ? getenv("TRACKING_SHEET_ID") : "",
                    enc2, apikey);
                free(enc2);
                int st2 = 0;
                char *r2 = trk_curl("GET", url2, NULL, NULL, NULL, &st2);
                if (r2 && st2 == 200) return r2;
                free(r2);
            }
        }
        return NULL;
    }

    char url[2048];
    /* Имя листа с пробелами оборачиваем в одинарные кавычки (A1-нотация),
       кодируем только его — суффикс !A:F оставляем как есть. */
    char *enc_sheet = trk_url_encode(sheet_name);
    if (!enc_sheet) return NULL;
    snprintf(url, sizeof(url),
        "%s/v4/spreadsheets/%s/values/%%27%s%%27!A:F",
        g_api_base, g_sheet_id, enc_sheet);
    free(enc_sheet);

    int status = 0;
    char *resp = trk_curl("GET", url, NULL, NULL, token, &status);
    if (!resp) return NULL;
    if (status != 200) {
        if (!detect_api_disabled(resp))
            snprintf(g_last_error, sizeof(g_last_error),
                     "sheets_get_values status=%d resp=%.160s", status, resp);
        trk_log("%s", g_last_error);
        free(resp);
        return NULL;
    }
    return resp;
}

/* ---------- Google Sheets: добавление строки (append) ---------- */
/* values: массив из 6 строк (A..F) */
static int sheets_append_row(const char *sheet_name, const char *values_json) {
    const char *token = sa_access_token();
    if (!token) return -1;

    char url[2048];
    char *enc_sheet = trk_url_encode(sheet_name);
    if (!enc_sheet) return -1;
    snprintf(url, sizeof(url),
        "%s/v4/spreadsheets/%s/values/%%27%s%%27!A:F:append"
        "?valueInputOption=USER_ENTERED&insertDataOption=INSERT_ROWS",
        g_api_base, g_sheet_id, enc_sheet);
    free(enc_sheet);

    char body[4096];
    snprintf(body, sizeof(body), "{\"values\":[%s]}", values_json);

    int status = 0;
    char *resp = trk_curl("POST", url, "application/json", body, token, &status);
    if (!resp) return -1;
    if (status != 200) {
        if (!detect_api_disabled(resp))
            snprintf(g_last_error, sizeof(g_last_error),
                     "sheets_append_row status=%d resp=%.160s", status, resp);
        trk_log("%s", g_last_error);
        free(resp);
        return -1;
    }
    trk_log("appended row to '%s'", sheet_name);
    free(resp);
    return 0;
}

/* ---------- Google Sheets: обновление диапазона ---------- */
/* A1-нотация, например "Лист1!A5:F5" */
/* Обновляет диапазон, принимая ГОТОВЫЙ JSON-объект {"values":[...]}
   (используется для записи нескольких строк сразу). */
static int sheets_update_values_raw(const char *sheet_name, const char *a1_suffix,
                                    const char *values_object) {
    const char *token = sa_access_token();
    if (!token) return -1;

    char url[2048];
    char *enc = trk_url_encode(sheet_name);
    if (!enc) return -1;
    snprintf(url, sizeof(url),
        "%s/v4/spreadsheets/%s/values/%%27%s%%27%s"
        "?valueInputOption=USER_ENTERED",
        g_api_base, g_sheet_id, enc, a1_suffix);
    free(enc);

    int status = 0;
    char *resp = trk_curl("PUT", url, "application/json", values_object, token, &status);
    if (!resp) return -1;
    if (status != 200) {
        if (!detect_api_disabled(resp))
            snprintf(g_last_error, sizeof(g_last_error),
                     "sheets_update_values_raw status=%d resp=%.160s", status, resp);
        trk_log("%s", g_last_error);
        free(resp);
        return -1;
    }
    free(resp);
    return 0;
}

static int sheets_update_range(const char *sheet_name, const char *a1_suffix,
                               const char *values_json) {
    const char *token = sa_access_token();
    if (!token) return -1;

    char url[2048];
    /* Кодируем только имя листа; суффикс диапазона (!A5:F5) оставляем как есть.
       Имя с пробелами оборачиваем в одинарные кавычки (требование A1-нотации). */
    char *enc = trk_url_encode(sheet_name);
    if (!enc) return -1;
    snprintf(url, sizeof(url),
        "%s/v4/spreadsheets/%s/values/%%27%s%%27%s"
        "?valueInputOption=USER_ENTERED",
        g_api_base, g_sheet_id, enc, a1_suffix);
    free(enc);

    char body[4096];
    snprintf(body, sizeof(body), "{\"values\":[%s]}", values_json);

    int status = 0;
    char *resp = trk_curl("PUT", url, "application/json", body, token, &status);
    if (!resp) return -1;
    if (status != 200) {
        if (!detect_api_disabled(resp))
            snprintf(g_last_error, sizeof(g_last_error),
                     "sheets_update_range status=%d resp=%.160s", status, resp);
        trk_log("%s", g_last_error);
        free(resp);
        return -1;
    }
    free(resp);
    return 0;
}

/* ---------- Google Sheets: создание вкладки ---------- */
/* Создаёт лист с указанным именем (если уже есть — ошибка API, это ОК). */
static int sheets_create_sheet(const char *sheet_name) {
    const char *token = sa_access_token();
    if (!token) return -1;

    char url[2048];
    snprintf(url, sizeof(url),
        "%s/v4/spreadsheets/%s:batchUpdate", g_api_base, g_sheet_id);

    char body[2048];
    char *esc = json_escape_local(sheet_name);
    snprintf(body, sizeof(body),
        "{\"requests\":[{\"addSheet\":{\"properties\":{\"title\":\"%s\"}}}]}",
        esc ? esc : sheet_name);
    free(esc);

    int status = 0;
    char *resp = trk_curl("POST", url, "application/json", body, token, &status);
    if (!resp) return -1;
    if (status != 200) {
        /* Вкладка уже существует — это не ошибка */
        if (strstr(resp, "already exists")) {
            trk_log("sheet '%s' already exists", sheet_name);
            free(resp);
            return 0;
        }
        if (!detect_api_disabled(resp))
            snprintf(g_last_error, sizeof(g_last_error),
                     "sheets_create_sheet status=%d resp=%.160s", status, resp);
        trk_log("%s", g_last_error);
        free(resp);
        return -1;
    }
    trk_log("created sheet '%s'", sheet_name);
    free(resp);
    return 0;
}

/* ==================================================================
 *  Telegram initData: проверка подписи и извлечение пользователя
 * ================================================================== */

typedef struct { char key[128]; char *val; } KvPair;

/* Разбирает query-строку initData в список пар */
static int parse_kv(const char *qs, KvPair *out, int max) {
    int n = 0;
    const char *p = qs;
    while (*p && n < max) {
        const char *amp = strchr(p, '&');
        size_t seglen = amp ? (size_t)(amp - p) : strlen(p);
        char *seg = malloc(seglen + 1);
        if (!seg) break;
        memcpy(seg, p, seglen);
        seg[seglen] = '\0';
        char *eq = strchr(seg, '=');
        if (eq) {
            *eq = '\0';
            char *k = trk_url_decode(seg);
            char *v = trk_url_decode(eq + 1);
            if (k && v) {
                snprintf(out[n].key, sizeof(out[n].key), "%s", k);
                out[n].val = v;
                n++;
                free(k);
            } else {
                free(k); free(v);
            }
        }
        free(seg);
        if (!amp) break;
        p = amp + 1;
    }
    return n;
}

/* Проверяет подпись initData и возвращает telegram user id (NULL при ошибке) */
static char *telegram_verify_initdata(const char *init_data, const char *bot_token) {
    if (!init_data || !*init_data || !bot_token || !*bot_token) return NULL;

    KvPair kv[64];
    int n = parse_kv(init_data, kv, 64);
    if (n <= 0) return NULL;

    /* secret_key = HMAC_SHA256(key="WebAppData", msg=bot_token) */
    const char *WEBAPP = "WebAppData";
    char webapp_hex[64];
    hexify((const unsigned char *)WEBAPP, strlen(WEBAPP), webapp_hex);

    FILE *bt = fopen("/tmp/trk_bot_token.txt", "w");
    if (!bt) return NULL;
    fputs(bot_token, bt);
    fclose(bt);

    size_t sklen = 0;
    unsigned char *sk = hmac_sha256(webapp_hex, "/tmp/trk_bot_token.txt", &sklen);
    if (!sk || sklen != 32) { free(sk); return NULL; }
    char sk_hex[128];
    hexify(sk, sklen, sk_hex);
    free(sk);

    /* data_check_string: пары без hash, отсортированные по ключу, через \n */
    char *provided_hash = NULL;
    for (int i = 0; i < n; i++) {
        if (strcmp(kv[i].key, "hash") == 0) { provided_hash = kv[i].val; kv[i].val = NULL; }
    }
    /* сортировка по ключу */
    for (int i = 0; i < n - 1; i++) {
        for (int j = i + 1; j < n; j++) {
            if (kv[i].key && kv[j].key && strcmp(kv[i].key, kv[j].key) > 0) {
                KvPair tmp = kv[i]; kv[i] = kv[j]; kv[j] = tmp;
            }
        }
    }
    FILE *dcs = fopen("/tmp/trk_dcs.txt", "w");
    if (!dcs) { free(provided_hash); return NULL; }
    int first = 1;
    for (int i = 0; i < n; i++) {
        if (!kv[i].val) continue;
        if (!first) fputc('\n', dcs);
        fprintf(dcs, "%s=%s", kv[i].key, kv[i].val);
        first = 0;
    }
    fclose(dcs);

    size_t hlen = 0;
    unsigned char *h = hmac_sha256(sk_hex, "/tmp/trk_dcs.txt", &hlen);
    if (!h || hlen != 32) { free(h); free(provided_hash); return NULL; }
    char *calc = b64url(h, hlen);
    free(h);
    if (!calc) { free(provided_hash); return NULL; }

    int ok = provided_hash && strcmp(calc, provided_hash) == 0;
    free(calc);
    if (!ok) {
        trk_log("initData signature mismatch");
        free(provided_hash);
        return NULL;
    }
    free(provided_hash);

    /* достаём user.id */
    char *user_json = NULL;
    for (int i = 0; i < n; i++) {
        if (kv[i].val && strcmp(kv[i].key, "user") == 0) { user_json = kv[i].val; kv[i].val = NULL; }
    }
    char *uid = user_json ? json_get_string(user_json, "id") : NULL;
    free(user_json);

    for (int i = 0; i < n; i++) free(kv[i].val);
    return uid;
}

/* Полное имя пользователя из initData (для fallback) */
static char *telegram_user_fullname(const char *init_data) {
    KvPair kv[64];
    int n = parse_kv(init_data, kv, 64);
    char *result = NULL;
    for (int i = 0; i < n; i++) {
        if (kv[i].val && strcmp(kv[i].key, "user") == 0) {
            char *first = json_get_string(kv[i].val, "first_name");
            char *last  = json_get_string(kv[i].val, "last_name");
            if (first && last) {
                size_t l = strlen(first) + strlen(last) + 2;
                result = malloc(l);
                if (result) snprintf(result, l, "%s %s", first, last);
            } else if (first) {
                result = strdup(first);
            }
            free(first); free(last);
            break;
        }
    }
    for (int i = 0; i < n; i++) free(kv[i].val);
    return result;
}

/* ==================================================================
 *  Разбор и сборка листа
 * ================================================================== */

/* Одна строка листа: 6 ячеек (A..F) */
typedef struct { char *cell[6]; } TRow;

typedef struct { TRow *row; int count; int cap; } TRows;

static void trows_free(TRows *t) {
    for (int i = 0; i < t->count; i++) {
        for (int c = 0; c < 6; c++) free(t->row[i].cell[c]);
    }
    free(t->row);
    t->row = NULL;
    t->count = t->cap = 0;
}

static TRow *trows_add(TRows *t) {
    if (t->count >= t->cap) {
        t->cap = t->cap ? t->cap * 2 : 32;
        t->row = realloc(t->row, sizeof(TRow) * t->cap);
    }
    TRow *r = &t->row[t->count++];
    for (int c = 0; c < 6; c++) r->cell[c] = strdup("");
    return r;
}

/* Разбирает JSON-массив значений Google Sheets в TRows */
static int parse_values_json(const char *json, TRows *out) {
    const char *v = strstr(json, "\"values\"");
    if (!v) return 0;
    const char *arr = strchr(v, '[');
    if (!arr) return 0;
    arr++;

    while (*arr && *arr != ']') {
        while (*arr == ' ' || *arr == '\t' || *arr == '\n' || *arr == '\r' || *arr == ',') arr++;
        if (*arr != '[') break;
        arr++;

        TRow *r = trows_add(out);
        int cell = 0;
        while (*arr && *arr != ']') {
            while (*arr == ' ' || *arr == '\t' || *arr == '\n' || *arr == '\r' || *arr == ',') arr++;
            if (*arr == ']') break;
            if (*arr == '"') {
                arr++;
                const char *start = arr;
                while (*arr && *arr != '"') {
                    if (*arr == '\\' && arr[1]) arr++;
                    arr++;
                }
                size_t len = (size_t)(arr - start);
                char *raw = malloc(len + 1);
                if (raw) {
                    memcpy(raw, start, len);
                    raw[len] = '\0';
                    char *dec = raw;
                    /* базовая распаковка escape-последовательностей */
                    char *outp = malloc(len + 1);
                    size_t o = 0;
                    for (size_t i = 0; i < len; i++) {
                        if (raw[i] == '\\' && i + 1 < len) {
                            i++;
                            switch (raw[i]) {
                                case 'n': outp[o++] = '\n'; break;
                                case 't': outp[o++] = '\t'; break;
                                case 'r': outp[o++] = '\r'; break;
                                default:  outp[o++] = raw[i];
                            }
                        } else outp[o++] = raw[i];
                    }
                    outp[o] = '\0';
                    free(raw);
                    dec = outp;
                    if (cell < 6) {
                        free(r->cell[cell]);
                        r->cell[cell] = dec;
                    } else {
                        free(dec);
                    }
                    cell++;
                }
                if (*arr == '"') arr++;
            } else {
                /* число/булево без кавычек */
                const char *start = arr;
                while (*arr && *arr != ',' && *arr != ']') arr++;
                size_t len = (size_t)(arr - start);
                while (len && (start[len-1] == ' ' || start[len-1] == '\n')) len--;
                char *num = malloc(len + 1);
                if (num) {
                    memcpy(num, start, len);
                    num[len] = '\0';
                    if (cell < 6) { free(r->cell[cell]); r->cell[cell] = num; }
                    else free(num);
                    cell++;
                }
            }
        }
        while (*arr && *arr != ']') arr++;
        if (*arr == ']') arr++;
    }
    return out->count;
}

/* Ячейка пустая? */
static int cell_empty(const char *s) {
    return !s || !*s;
}

/* Разбор "ЧЧ:ММ" в минуты от полуночи; -1 при ошибке */
static int parse_hhmm(const char *s) {
    if (!s) return -1;
    int h = 0, m = 0;
    if (sscanf(s, "%d:%d", &h, &m) != 2) return -1;
    if (h < 0 || h > 23 || m < 0 || m > 59) return -1;
    return h * 60 + m;
}

/* Строка пустая (все ячейки пустые)? */
static int row_is_empty(TRow *r) {
    for (int c = 0; c < 6; c++) {
        if (!cell_empty(r->cell[c])) return 0;
    }
    return 1;
}

/* Нормализует время к виду ЧЧ:ММ (Google может вернуть "8:00") */
static void normalize_time(const char *in, char *out, size_t cap) {
    int h = 0, m = 0;
    if (sscanf(in, "%d:%d", &h, &m) == 2) {
        snprintf(out, cap, "%02d:%02d", h, m);
    } else {
        snprintf(out, cap, "%s", in ? in : "");
    }
}

/* Нормализует дату к виду ДД.ММ.ГГГГ */
static void normalize_date(const char *in, char *out, size_t cap) {
    int d = 0, m = 0, y = 0;
    if (sscanf(in, "%d.%d.%d", &d, &m, &y) == 3) {
        snprintf(out, cap, "%02d.%02d.%04d", d, m, y);
    } else if (sscanf(in, "%d/%d/%d", &d, &m, &y) == 3) {
        snprintf(out, cap, "%02d.%02d.%04d", d, m, y);
    } else if (sscanf(in, "%d-%d-%d", &y, &m, &d) == 3) {
        snprintf(out, cap, "%02d.%02d.%04d", d, m, y);
    } else {
        snprintf(out, cap, "%s", in ? in : "");
    }
}

/* ==================================================================
 *  Основная логика: Пришла / Ушла
 * ================================================================== */

/* Ищет строку сотрудника в текущем месяце.
 * Схема листа: блок сотрудника начинается со строки, где в A стоит ФИО,
 * далее идут строки с датами. Блоки разных сотрудников разделены пустой строкой.
 * Возвращает индекс строки с ФИО или -1. */
static int find_employee_block(TRows *t, const char *name) {
    for (int i = 0; i < t->count; i++) {
        const char *a = t->row[i].cell[0];
        if (cell_empty(a)) continue;
        /* строка-заголовок блока: A = ФИО, остальные пусты */
        int rest_empty = 1;
        for (int c = 1; c < 6; c++) {
            if (!cell_empty(t->row[i].cell[c])) { rest_empty = 0; break; }
        }
        if (!rest_empty) continue;
        /* ФИО не должно быть датой */
        int d, m, y;
        if (sscanf(a, "%d.%d.%d", &d, &m, &y) == 3) continue;
        if (sscanf(a, "%d/%d/%d", &d, &m, &y) == 3) continue;
        if (strcmp(a, name) == 0) return i;
        if (strstr(a, name)) return i;
    }
    return -1;
}

/* Находит строку с датой внутри блока сотрудника (начиная с block_idx+1).
 * Возвращает индекс или -1. */
static int find_date_row(TRows *t, int block_idx, const char *date) {
    char norm[64];
    normalize_date(date, norm, sizeof(norm));
    for (int i = block_idx + 1; i < t->count; i++) {
        if (row_is_empty(&t->row[i])) break;   /* конец блока */
        const char *a = t->row[i].cell[0];
        if (cell_empty(a)) continue;
        char n2[64];
        normalize_date(a, n2, sizeof(n2));
        if (strcmp(n2, norm) == 0) return i;
    }
    return -1;
}

/* Собирает значения строки в JSON-массив вида [["a","b",...]] */
static char *build_values_json(const char *const cells[6]) {
    size_t cap = 1024;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    size_t o = 0;
    o += snprintf(buf + o, cap - o, "[");
    for (int c = 0; c < 6; c++) {
        char *esc = json_escape_local(cells[c] ? cells[c] : "");
        o += snprintf(buf + o, cap - o, "%s\"%s\"", c ? "," : "", esc ? esc : "");
        free(esc);
    }
    snprintf(buf + o, cap - o, "]");
    return buf;
}

/* Читает лист месяца; если листа нет — создаёт (с заголовками). */
static int load_or_create_month_sheet(const char *month_sheet, TRows *rows) {
    char *resp = sheets_get_values(month_sheet);
    if (resp) {
        parse_values_json(resp, rows);
        free(resp);
    } else {
        /* листа нет — создаём и перечитываем (пустой лист) */
        if (sheets_create_sheet(month_sheet) != 0) return -1;
        char *r2 = sheets_get_values(month_sheet);
        if (r2) { parse_values_json(r2, rows); free(r2); }
    }

    /* Если лист пустой (или состоит только из заголовка) — добавляем/проверяем
       строку заголовков колонок. */
    int need_header = 1;
    if (rows->count > 0) {
        const char *a = rows->row[0].cell[0];
        if (a && strcmp(a, "Дата") == 0) need_header = 0;
    }
    if (need_header) {
        TRow *h = trows_add(rows);
        static const char *HEAD[6] = {
            "Дата", "Время прихода", "Время ухода",
            "Время в часах", "Ставка в час", "Сумма за день"
        };
        for (int cc = 0; cc < 6; cc++) {
            free(h->cell[cc]);
            h->cell[cc] = strdup(HEAD[cc]);
        }
    }
    return 0;
}

/* Находит последнюю строку блока сотрудника (индекс) или -1 */
static int find_block_end(TRows *t, int block_idx) {
    int last = block_idx;
    for (int i = block_idx + 1; i < t->count; i++) {
        if (row_is_empty(&t->row[i])) break;
        last = i;
    }
    return last;
}

/* Вставляет пустую строку в позицию idx */
static void trows_insert(TRows *t, int idx) {
    if (idx < 0) idx = 0;
    if (idx > t->count) idx = t->count;
    trows_add(t);
    for (int i = t->count - 1; i > idx; i--) {
        t->row[i] = t->row[i - 1];
    }
    for (int cc = 0; cc < 6; cc++) {
        free(t->row[idx].cell[cc]);
        t->row[idx].cell[cc] = strdup("");
    }
}

/* Полностью перезаписывает лист указанными строками (A1:F{n}).
   Нужна, чтобы вставлять строки в середину (новый сотрудник / новый день)
   и сохранять порядок блоков. */
static int sheets_write_rows(const char *sheet_name, TRows *rows) {
    if (rows->count == 0) return 0;

    size_t cap = 1024 + (size_t)rows->count * 256;
    char *body = malloc(cap);
    if (!body) return -1;
    size_t o = 0;
    /* Строим полный объект {"values":[[...],[...]]} — ровно то, что ждёт API.
       Обёртка добавляется здесь, чтобы не получить двойную обёртку
       ("Invalid values[0][0]: list_value"). */
    o += snprintf(body + o, cap - o, "{\"values\":[");
    for (int i = 0; i < rows->count; i++) {
        o += snprintf(body + o, cap - o, "%s[", i ? "," : "");
        for (int cc = 0; cc < 6; cc++) {
            char *esc = json_escape_local(rows->row[i].cell[cc]);
            o += snprintf(body + o, cap - o, "%s\"%s\"", cc ? "," : "", esc ? esc : "");
            free(esc);
        }
        o += snprintf(body + o, cap - o, "]");
    }
    o += snprintf(body + o, cap - o, "]}");

    char suffix[64];
    snprintf(suffix, sizeof(suffix), "!A1:F%d", rows->count);
    int rc = sheets_update_values_raw(sheet_name, suffix, body);
    free(body);
    return rc;
}

/* Обновляет строку листа (1-based номер строки в Google Sheets) */
static int write_row(int sheet_row_number, const char *const cells[6]) {
    char *json = build_values_json(cells);
    if (!json) return -1;
    char suffix[64];
    snprintf(suffix, sizeof(suffix), "!A%d:F%d", sheet_row_number, sheet_row_number);
    int rc = sheets_update_range(g_current_sheet, suffix, json);
    free(json);
    return rc;
}

/* Добавляет новую строку в конец листа (через append) */
static int append_row(const char *const cells[6]) {
    char *json = build_values_json(cells);
    if (!json) return -1;
    int rc = sheets_append_row(g_current_sheet, json);
    free(json);
    return rc;
}

static int tracking_ensure_spreadsheet(void);
static void api_tracking_diag(int fd, const char *query);

/* Ответ клиенту */
static void trk_reply(int fd, const char *status, const char *body) {
    char header[256];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %s\r\nContent-Type: application/json; charset=utf-8\r\n"
        "Content-Length: %zu\r\nConnection: close\r\n\r\n", status, strlen(body));
    write(fd, header, hlen);
    write(fd, body, strlen(body));
}

/* Определяет сотрудника по initData: сперва EMPLOYEE_MAP, затем имя из Telegram */
static char *resolve_employee(const char *init_data) {
    const char *bot_token = getenv("BOT_TOKEN");
    char *uid = telegram_verify_initdata(init_data, bot_token);
    char *name = NULL;
    if (uid) {
        name = employee_name_by_chat(uid);
        if (!name) {
            /* нет в карте — используем имя из Telegram */
            name = telegram_user_fullname(init_data);
            if (name) trk_log("chat %s not in EMPLOYEE_MAP, using telegram name '%s'", uid, name);
        }
        free(uid);
    }
    return name;
}

/* POST /api/tracking/clock?action=in|out
   Тело: initData=... (application/x-www-form-urlencoded) */
static void api_tracking_clock(int fd, const char *query, const char *body) {
    char *action = query_param(query, "action");
    char *init_data = body ? query_param(body, "initData") : NULL;

    if (!action) {
        trk_reply(fd, "400 Bad Request", "{\"ok\":false,\"error\":\"no action\"}");
        goto done;
    }
    if (!init_data || !*init_data) {
        trk_reply(fd, "400 Bad Request", "{\"ok\":false,\"error\":\"no initData\"}");
        goto done;
    }

    char *name = resolve_employee(init_data);
    if (!name) {
        trk_reply(fd, "403 Forbidden", "{\"ok\":false,\"error\":\"unauthorized\"}");
        goto done;
    }

    char date[32], time_s[16];
    current_datetime(date, sizeof(date), time_s, sizeof(time_s));

    current_month_name(g_current_sheet, sizeof(g_current_sheet));

    TRows rows;
    memset(&rows, 0, sizeof(rows));
    if (load_or_create_month_sheet(g_current_sheet, &rows) != 0) {
        trk_reply(fd, "500 Internal Server Error",
                  "{\"ok\":false,\"error\":\"cannot access sheet\"}");
        trows_free(&rows);
        free(name);
        goto done;
    }

    int block = find_employee_block(&rows, name);
    int date_row = (block >= 0) ? find_date_row(&rows, block, date) : -1;

    if (strcmp(action, "in") == 0) {
        if (date_row >= 0 && !cell_empty(rows.row[date_row].cell[1])) {
            char msg[512];
            char *e = json_escape_local("Уже отмечен приход сегодня");
            snprintf(msg, sizeof(msg),
                "{\"ok\":false,\"error\":\"%s\",\"time\":\"%s\"}", e ? e : "", time_s);
            trk_reply(fd, "409 Conflict", msg);
            free(e);
            trows_free(&rows);
            free(name);
            goto done;
        }
        double rate = employee_rate_by_name(name);
        char rate_buf[32];
        snprintf(rate_buf, sizeof(rate_buf), "%.0f", rate);

        if (date_row >= 0) {
            /* строка за сегодня уже есть, но время прихода пустое */
            free(rows.row[date_row].cell[1]);
            rows.row[date_row].cell[1] = strdup(time_s);
            if (cell_empty(rows.row[date_row].cell[4])) {
                free(rows.row[date_row].cell[4]);
                rows.row[date_row].cell[4] = strdup(rate_buf);
            }
        } else if (block >= 0) {
            /* блок сотрудника есть — новая строка в конец его блока */
            int last = find_block_end(&rows, block);
            trows_insert(&rows, last + 1);
            int ni = last + 1;
            free(rows.row[ni].cell[0]); rows.row[ni].cell[0] = strdup(date);
            free(rows.row[ni].cell[1]); rows.row[ni].cell[1] = strdup(time_s);
            free(rows.row[ni].cell[4]); rows.row[ni].cell[4] = strdup(rate_buf);
        } else {
            /* новый сотрудник — блок: пустая строка + ФИО + строка дня */
            trows_insert(&rows, rows.count);
            trows_insert(&rows, rows.count);
            int ni = rows.count - 1;
            free(rows.row[ni].cell[0]); rows.row[ni].cell[0] = strdup(name);
            trows_insert(&rows, rows.count);
            int di = rows.count - 1;
            free(rows.row[di].cell[0]); rows.row[di].cell[0] = strdup(date);
            free(rows.row[di].cell[1]); rows.row[di].cell[1] = strdup(time_s);
            free(rows.row[di].cell[4]); rows.row[di].cell[4] = strdup(rate_buf);
        }

        if (sheets_write_rows(g_current_sheet, &rows) != 0) {
            trk_reply(fd, "500 Internal Server Error",
                      "{\"ok\":false,\"error\":\"write failed\"}");
            trows_free(&rows); free(name);
            goto done;
        }
        char msg[512];
        char *en = json_escape_local(name);
        snprintf(msg, sizeof(msg),
            "{\"ok\":true,\"action\":\"in\",\"name\":\"%s\",\"date\":\"%s\",\"time\":\"%s\"}",
            en ? en : "", date, time_s);
        trk_reply(fd, "200 OK", msg);
        free(en);
        trk_log("CLOCK IN: %s date=%s time=%s", name, date, time_s);

    } else if (strcmp(action, "out") == 0) {
        if (date_row < 0 || cell_empty(rows.row[date_row].cell[1])) {
            trk_reply(fd, "409 Conflict",
                      "{\"ok\":false,\"error\":\"no clock-in for today\"}");
            trows_free(&rows); free(name);
            goto done;
        }
        if (!cell_empty(rows.row[date_row].cell[2])) {
            trk_reply(fd, "409 Conflict",
                      "{\"ok\":false,\"error\":\"already clocked out\"}");
            trows_free(&rows); free(name);
            goto done;
        }
        /* заполняем время ухода и формулы */
        free(rows.row[date_row].cell[2]);
        rows.row[date_row].cell[2] = strdup(time_s);

        int in_min  = parse_hhmm(rows.row[date_row].cell[1]);
        int out_min = parse_hhmm(time_s);
        double hours = 0;
        if (in_min >= 0 && out_min >= 0) {
            hours = (out_min - in_min) / 60.0;
            if (hours < 0) hours += 24.0;
        }
        char hours_buf[32], sum_buf[32];
        snprintf(hours_buf, sizeof(hours_buf), "%.2f", hours);
        double rate = atof(rows.row[date_row].cell[4]);
        snprintf(sum_buf, sizeof(sum_buf), "%.2f", hours * rate);

        free(rows.row[date_row].cell[3]);
        rows.row[date_row].cell[3] = strdup(hours_buf);
        free(rows.row[date_row].cell[5]);
        rows.row[date_row].cell[5] = strdup(sum_buf);
        if (cell_empty(rows.row[date_row].cell[4])) {
            char rb[32];
            snprintf(rb, sizeof(rb), "%.0f", rate);
            free(rows.row[date_row].cell[4]);
            rows.row[date_row].cell[4] = strdup(rb);
        }

        if (sheets_write_rows(g_current_sheet, &rows) != 0) {
            trk_reply(fd, "500 Internal Server Error",
                      "{\"ok\":false,\"error\":\"write failed\"}");
            trows_free(&rows); free(name);
            goto done;
        }
        char msg[512];
        char *en = json_escape_local(name);
        snprintf(msg, sizeof(msg),
            "{\"ok\":true,\"action\":\"out\",\"name\":\"%s\",\"date\":\"%s\",\"time\":\"%s\","
            "\"hours\":\"%s\",\"sum\":\"%s\"}",
            en ? en : "", date, time_s, hours_buf, sum_buf);
        trk_reply(fd, "200 OK", msg);
        free(en);
        trk_log("CLOCK OUT: %s date=%s time=%s hours=%s sum=%s",
                name, date, time_s, hours_buf, sum_buf);

    } else {
        trk_reply(fd, "400 Bad Request", "{\"ok\":false,\"error\":\"bad action\"}");
    }

    trows_free(&rows);
    free(name);

done:
    free(action);
    free(init_data);
}

/* GET /api/tracking/summary?month=...&name=... — сводка по сотруднику */
static void api_tracking_summary(int fd, const char *query) {
    char *name = query_param(query, "name");
    char *month = query_param(query, "month");

    char sheet[256];
    if (month && *month) snprintf(sheet, sizeof(sheet), "%s", month);
    else current_month_name(sheet, sizeof(sheet));

    if (!g_sheet_id) {
        trk_reply(fd, "503 Service Unavailable",
                  "{\"ok\":false,\"error\":\"tracking sheet not configured\"}");
        free(name); free(month);
        return;
    }

    TRows rows;
    memset(&rows, 0, sizeof(rows));
    char *resp = sheets_get_values(sheet);
    if (!resp) {
        trk_reply(fd, "503 Service Unavailable",
                  "{\"ok\":false,\"error\":\"sheet not ready\"}");
        free(name); free(month);
        return;
    }
    parse_values_json(resp, &rows);
    free(resp);

    double total_hours = 0, total_sum = 0;
    int days = 0;
    int block = name ? find_employee_block(&rows, name) : -1;
    if (block >= 0) {
        for (int i = block + 1; i < rows.count; i++) {
            if (row_is_empty(&rows.row[i])) break;
            const char *a = rows.row[i].cell[0];
            if (cell_empty(a)) continue;
            double h = atof(rows.row[i].cell[3]);
            double s = atof(rows.row[i].cell[5]);
            if (h > 0) { total_hours += h; days++; }
            if (s > 0) total_sum += s;
        }
    }

    char msg[1024];
    char *en = json_escape_local(name ? name : "");
    char *em = json_escape_local(sheet);
    snprintf(msg, sizeof(msg),
        "{\"ok\":true,\"name\":\"%s\",\"month\":\"%s\",\"days\":%d,"
        "\"total_hours\":\"%.2f\",\"total_sum\":\"%.2f\"}",
        en ? en : "", em ? em : "", days, total_hours, total_sum);
    trk_reply(fd, "200 OK", msg);
    free(en); free(em);
    trows_free(&rows);
    free(name); free(month);
}

/* Точка входа */
void tracking_handle(int fd, const char *method, const char *path, const char *body) {
    if (!g_inited) {
        g_inited = 1;
        g_sa_json     = getenv("GOOGLE_SERVICE_ACCOUNT_JSON");
        g_emp_map     = getenv("EMPLOYEE_MAP");
        g_rates_map   = getenv("EMPLOYEE_RATES");
        const char *r = getenv("HOURLY_RATE");
        if (r) g_default_rate = atof(r);
        const char *tz = getenv("TZ_OFFSET_SECONDS");
        if (tz) g_tz_offset = atoi(tz);
        const char *base = getenv("GOOGLE_API_BASE");
        g_api_base = (base && *base) ? (char *)base : (char *)"https://sheets.googleapis.com";
        const char *dbase = getenv("GOOGLE_DRIVE_BASE");
        g_drive_base = (dbase && *dbase) ? (char *)dbase : (char *)"https://www.googleapis.com";
        trk_log("init: sheet=%s sa=%s emp_map=%s rate=%.0f tz=%d",
                g_sheet_id ? g_sheet_id : "(null)",
                g_sa_json ? "yes" : "no",
                g_emp_map ? "yes" : "no",
                g_default_rate, g_tz_offset);
    }

    /* Первый вызов: находим существующую таблицу или создаём новую
       (владельцем будет Service Account). */
    if (!g_sheet_id) {
        int rc = tracking_ensure_spreadsheet();
        if (rc != 0 && strcmp(path, "/diag/tracking") != 0) {
            trk_reply(fd, "503 Service Unavailable",
                      "{\"ok\":false,\"error\":\"tracking sheet not configured\"}");
            return;
        }
    }

    if (strncmp(path, "/api/tracking/clock", 18) == 0) {
        api_tracking_clock(fd, path, body);
        return;
    }
    if (strncmp(path, "/api/tracking/summary", 20) == 0) {
        api_tracking_summary(fd, path);
        return;
    }
    if (strcmp(path, "/diag/tracking") == 0) {
        api_tracking_diag(fd, path);
        return;
    }
    trk_reply(fd, "404 Not Found", "{\"ok\":false,\"error\":\"not found\"}");
}

/* ==================================================================
 *  Автосоздание таблицы "Time tracking" и выдача доступа
 * ================================================================== */

/* Создаёт новую таблицу. Возвращает её ID (в g_created_sheet_id) или NULL. */
static char *sheets_create_spreadsheet(const char *title) {
    const char *token = sa_access_token();
    if (!token) return NULL;

    char body[1024];
    char *esc = json_escape_local(title);
    snprintf(body, sizeof(body),
        "{\"properties\":{\"title\":\"%s\",\"locale\":\"ru_RU\","
        "\"autoRecalc\":\"ON_CHANGE\"}}",
        esc ? esc : title);
    free(esc);

    int status = 0;
    char url[512];
    snprintf(url, sizeof(url), "%s/v4/spreadsheets", g_api_base);
    char *resp = trk_curl("POST", url,
        "application/json", body, token, &status);
    if (!resp) return NULL;
    if (status != 200) {
        if (!detect_api_disabled(resp))
            snprintf(g_last_error, sizeof(g_last_error),
                     "sheets_create_spreadsheet status=%d resp=%.160s", status, resp);
        trk_log("%s", g_last_error);
        free(resp);
        return NULL;
    }
    char *id = json_get_string(resp, "spreadsheetId");
    char *surl = json_get_string(resp, "spreadsheetUrl");
    if (id) {
        trk_log("created spreadsheet '%s' id=%s url=%s", title, id, surl ? surl : "?");
        /* сохраняем ID, чтобы пережить перезапуск */
        FILE *f = fopen("/tmp/tracking_sheet_id", "w");
        if (f) { fprintf(f, "%s\n", id); fclose(f); }
    }
    free(surl);
    free(resp);
    return id;
}

/* Выдаёт доступ на редактирование указанному email (владелец таблицы). */
static int drive_share(const char *file_id, const char *email, const char *role) {
    const char *token = sa_access_token();
    if (!token || !email || !*email) return -1;

    char url[2048];
    snprintf(url, sizeof(url),
        "%s/drive/v3/files/%s/permissions"
        "?sendNotificationEmail=false", g_drive_base, file_id);

    char body[1024];
    char *ee = json_escape_local(email);
    snprintf(body, sizeof(body),
        "{\"role\":\"%s\",\"type\":\"user\",\"emailAddress\":\"%s\"}",
        role ? role : "writer", ee ? ee : email);
    free(ee);

    int status = 0;
    char *resp = trk_curl("POST", url, "application/json", body, token, &status);
    if (!resp) return -1;
    if (status != 200 && status != 201) {
        trk_log("drive_share status=%d resp=%.300s", status, resp);
        free(resp);
        return -1;
    }
    trk_log("shared '%s' with %s as %s", file_id, email, role ? role : "writer");
    free(resp);
    return 0;
}

/* Проверяет, что таблица существует и доступна */
static int sheets_sheet_exists(const char *file_id) {
    const char *token = sa_access_token();
    if (!token) return 0;
    char url[2048];
    snprintf(url, sizeof(url),
        "%s/v4/spreadsheets/%s?fields=spreadsheetId", g_api_base, file_id);
    int status = 0;
    char *resp = trk_curl("GET", url, NULL, NULL, token, &status);
    if (!resp) return 0;
    int ok = (status == 200);
    if (!ok) detect_api_disabled(resp);
    free(resp);
    return ok;
}

/* Полная инициализация: найти или создать таблицу, выдать доступ.
 * g_sheet_id обновляется. Возвращает 0 при успехе. */
static int tracking_ensure_spreadsheet(void) {
    const char *env_id = getenv("TRACKING_SHEET_ID");
    const char *share_email = getenv("TRACKING_SHARE_EMAIL");
    const char *title = getenv("TRACKING_SHEET_TITLE");
    if (!title || !*title) title = "Time tracking";

    /* 1) ID из окружения */
    if (env_id && *env_id) {
        if (sheets_sheet_exists(env_id)) {
            g_sheet_id = strdup(env_id);
            if (share_email && *share_email) drive_share(env_id, share_email, "writer");
            return 0;
        }
        snprintf(g_last_error, sizeof(g_last_error),
                 "TRACKING_SHEET_ID=%s not accessible with current credentials", env_id);
        trk_log("%s", g_last_error);
    }

    /* 2) ID из локального кэша (создан ранее этим же сервис-аккаунтом) */
    FILE *cf = fopen("/tmp/tracking_sheet_id", "r");
    if (cf) {
        char cached[128] = {0};
        if (fgets(cached, sizeof(cached), cf)) {
            char *nl = strchr(cached, '\n'); if (nl) *nl = '\0';
            if (*cached && sheets_sheet_exists(cached)) {
                fclose(cf);
                g_sheet_id = strdup(cached);
                trk_log("using cached spreadsheet id=%s", cached);
                if (share_email && *share_email) drive_share(cached, share_email, "writer");
                return 0;
            }
        }
        fclose(cf);
    }

    /* 3) создаём новую таблицу */
    char *new_id = sheets_create_spreadsheet(title);
    if (!new_id) {
        if (!g_last_error[0])
            snprintf(g_last_error, sizeof(g_last_error),
                     "cannot create spreadsheet (need Service Account with spreadsheets scope)");
        if (strstr(g_last_error, "does not have permission"))
            snprintf(g_last_error, sizeof(g_last_error),
                     "Service Account cannot create spreadsheets. Set TRACKING_SHEET_ID "
                     "to the existing Time tracking spreadsheet id.");
        trk_log("%s", g_last_error);
        return -1;
    }
    g_sheet_id = new_id;

    /* 4) выдаём доступ владельцу */
    if (share_email && *share_email) {
        if (drive_share(g_sheet_id, share_email, "writer") != 0) {
            trk_log("WARNING: could not share with %s — share manually", share_email);
        }
    }
    return 0;
}

/* ==================================================================
 *  Диагностика трекинга: /diag/tracking[?write=1]
 * ================================================================== */
static void api_tracking_diag(int fd, const char *query) {
    size_t cap = 16384;
    char *out = malloc(cap);
    if (!out) { trk_reply(fd, "500 Internal Server Error", "{}"); return; }
    size_t o = 0;
    /* APPEND безопасно: snprintf возвращает длину, которую БЫ записал бы,
       поэтому при переполнении указатель o может выйти за пределы буфера. */
    #define APPEND(...) do { \
        if (o + 32 >= cap) break; \
        int _n = snprintf(out + o, cap - o, __VA_ARGS__); \
        if (_n < 0) break; \
        size_t _need = (size_t)_n; \
        if (_need >= cap - o) { o = cap - 1; break; } \
        o += _need; \
    } while(0)

    APPEND("{\"ok\":true");

    /* 1) наличие Service Account */
    APPEND(",\"sa_configured\":%s", g_sa_json ? "true" : "false");
    if (g_sa_json) {
        char *em = json_get_string(g_sa_json, "client_email");
        char *ee = json_escape_local(em ? em : "");
        APPEND(",\"sa_email\":\"%s\"", ee ? ee : "");
        free(ee); free(em);
    }

    /* 2) получение access token */
    const char *tok = sa_access_token();
    APPEND(",\"token_ok\":%s", tok ? "true" : "false");

    /* 3) таблица */
    APPEND(",\"sheet_id\":\"%s\"",
                  g_sheet_id ? g_sheet_id : "");
    APPEND(",\"env_sheet_id\":\"%s\"",
                  getenv("TRACKING_SHEET_ID") ? getenv("TRACKING_SHEET_ID") : "");
    APPEND(",\"share_email\":\"%s\"",
                  getenv("TRACKING_SHARE_EMAIL") ? getenv("TRACKING_SHARE_EMAIL") : "");
    APPEND(",\"emp_map\":\"%s\"",
                  getenv("EMPLOYEE_MAP") ? "set" : "MISSING");
    APPEND(",\"rate\":\"%.0f\"", g_default_rate);

    /* 4) список вкладок */
    if (g_sheet_id && tok) {
        char url[2048];
        snprintf(url, sizeof(url), "%s/v4/spreadsheets/%s?fields=sheets.properties.title",
                 g_api_base, g_sheet_id);
        int status = 0;
        char *resp = trk_curl("GET", url, NULL, NULL, tok, &status);
        APPEND(",\"tabs\":[");
        if (resp && status == 200) {
            const char *p = resp;
            int first = 1;
            while ((p = strstr(p, "\"title\"")) != NULL) {
                p = strchr(p, ':');
                if (!p) break;
                while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
                if (*p != '"') break;
                p++;
                const char *st = p;
                while (*p && *p != '"') p++;
                size_t len = (size_t)(p - st);
                char *name = malloc(len + 1);
                memcpy(name, st, len); name[len] = '\0';
                char *ee = json_escape_local(name);
                APPEND("%s\"%s\"", first ? "" : ",", ee ? ee : "");
                free(ee); free(name);
                first = 0;
                p++;
            }
        }
        APPEND("]");
        free(resp);
    }

    /* 5) проверка чтения вкладки месяца */
    char month[256];
    current_month_name(month, sizeof(month));
    APPEND(",\"month_tab\":\"%s\"", month);
    if (g_sheet_id && tok) {
        TRows rows;
        memset(&rows, 0, sizeof(rows));
        char *resp = sheets_get_values(month);
        APPEND(",\"read_ok\":%s", resp ? "true" : "false");
        if (resp) {
            parse_values_json(resp, &rows);
            APPEND(",\"rows\":%d", rows.count);
            trows_free(&rows);
            free(resp);
        }
    }

    /* 6) пробная запись (только по явному запросу ?write=1) */
    char *w = query_param(query, "write");
    if (w && strcmp(w, "1") == 0 && g_sheet_id && tok) {
        /* создаём вкладку месяца, если её нет, и пишем тестовую строку */
        TRows rows;
        memset(&rows, 0, sizeof(rows));
        char *resp = sheets_get_values(month);
        if (resp) { parse_values_json(resp, &rows); free(resp); }
        else {
            /* вкладки нет — создаём */
            if (sheets_create_sheet(month) == 0) {
                APPEND(",\"tab_created\":true");
                static const char *HEAD[6] = {
                    "Дата", "Время прихода", "Время ухода",
                    "Время в часах", "Ставка в час", "Сумма за день"
                };
                char vals[6][64];
                for (int c = 0; c < 6; c++) snprintf(vals[c], 64, "%s", HEAD[c]);
                const char *ptr[6] = { vals[0],vals[1],vals[2],vals[3],vals[4],vals[5] };
                char *hj = build_values_json(ptr);
                if (hj) { sheets_update_range(month, "!A1:F1", hj); free(hj); }
            }
        }
        trows_free(&rows);

        char date[32], ts[16];
        current_datetime(date, sizeof(date), ts, sizeof(ts));
        const char *cells[6] = { date, ts, "", "", "500", "" };
        int rc = append_row(cells);
        APPEND(",\"write_ok\":%s", rc == 0 ? "true" : "false");
        APPEND(",\"write_test\":{\"date\":\"%s\",\"time\":\"%s\"}",
                      date, ts);
    }
    free(w);

    /* 7) Подсказка по действиям */
    if (strstr(g_last_error, "DISABLED")) {
        o += snprintf(out + o, cap - o,
            ",\"hint\":\"Enable Google Sheets API in the project that owns the "
            "Service Account (project id women-health-684ad). The API key project "
            "already has it enabled, but the OAuth2 consumer project does not.\"");
    } else if (strstr(g_last_error, "not accessible")) {
        o += snprintf(out + o, cap - o,
            ",\"hint\":\"Share the Time tracking spreadsheet with the Service "
            "Account email as Editor.\"");
    } else if (strstr(g_last_error, "does not have permission")) {
        o += snprintf(out + o, cap - o,
            ",\"hint\":\"Service Account cannot create new spreadsheets. "
            "Set TRACKING_SHEET_ID to the existing Time tracking spreadsheet id "
            "(1zhWZRFI6GYYnh6PNQn37KiSyPLvjkSwJz-CKwF0xEnw).\"");
    } else if (!g_sheet_id[0]) {
        o += snprintf(out + o, cap - o,
            ",\"hint\":\"TRACKING_SHEET_ID is not set. Set it to the Time "
            "tracking spreadsheet id so the server can read/write it.\"");
    }

    if (g_last_error[0]) {
        char *ee = json_escape_local(g_last_error);
        APPEND(",\"last_error\":\"%s\"", ee ? ee : "");
        free(ee);
    }
    APPEND("}");
    #undef APPEND
    trk_reply(fd, "200 OK", out);
    free(out);
}

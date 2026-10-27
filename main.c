/*
 * Telegram-бот на чистом C с webhook.
 * Показывает график смен из Google Sheets в виде картинки PNG (ImageMagick + librsvg).
 * Деплой: Docker на Render (Web Service).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <time.h>
#include <fcntl.h>
#include <ctype.h>
#include <stdarg.h>

#include "tracking.h"
#include "http.h"

#define CACHE_TTL 60
#define BUFFER_SIZE (512 * 1024)
#define MAX_SHEETS 50
#define MAX_SHEET_NAME 128
#define MAX_MONTHS MAX_SHEETS
#define MAX_CHATS 128
#define MONTHS_FILE "/tmp/months.dat"
#define CABINET_INDEX_FILE "/tmp/cabinet_index.txt"

static const char *BOT_TOKEN;
static const char *SPREADSHEET_ID;
static const char *GOOGLE_API_KEY;
static const char *FROM_COL;
static const char *TO_COL;
static const char *SEARCH_NAME;
static const char *MINIAPP_URL;

static void init_config(void) {
    BOT_TOKEN      = getenv("BOT_TOKEN");
    SPREADSHEET_ID = getenv("SPREADSHEET_ID");
    GOOGLE_API_KEY = getenv("GOOGLE_API_KEY");
    FROM_COL       = getenv("FROM_COL");
    TO_COL         = getenv("TO_COL");
    SEARCH_NAME    = getenv("SEARCH_NAME");
    MINIAPP_URL    = getenv("MINIAPP_URL");
}


/* ================================================================
 * Утилиты для ручного разбора JSON по ключам (Google Sheets)
 * ================================================================ */
static const char *find_key_bounded(const char *json, size_t len, const char *key) {
    char search[256];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *end = json + len;
    const char *p = json;
    while (p < end) {
        const char *found = strstr(p, search);
        if (!found || found >= end) return NULL;
        const char *val = found + strlen(search);
        while (val < end && *val && *val != ':') val++;
        if (val < end && *val == ':') val++;
        while (val < end && *val && (*val == ' ' || *val == '\t')) val++;
        if (val < end) return val;
        p = found + 1;
    }
    return NULL;
}

static char *extract_string(const char *pos) {
    if (!pos || *pos != '"') return NULL;
    pos++;
    const char *end = pos;
    while (*end && *end != '"') {
        if (*end == '\\') end++;
        end++;
    }
    int len = end - pos;
    char *s = malloc(len + 1);
    if (!s) return NULL;
    memcpy(s, pos, len);
    s[len] = '\0';
    return s;
}

static int is_true(const char *pos) {
    return (pos && strncmp(pos, "true", 4) == 0);
}


/* ================================================================
 * Google Sheets: список видимых листов
 * ================================================================ */
static int get_visible_sheets(char sheets[][MAX_SHEET_NAME], int max_sheets) {
    char url[1024];
    snprintf(url, sizeof(url),
        "https://sheets.googleapis.com/v4/spreadsheets/%s"
        "?fields=sheets.properties(sheetId,title,hidden)&key=%s",
        SPREADSHEET_ID, GOOGLE_API_KEY);

        int http_status = 0;
    char *response = http_get(url, &http_status);
    if (!response) { fprintf(stderr, "[diag] google metadata fetch failed (curl/newtwork)\n"); return 0; }

    int count = 0;
    const char *p = response;

    while ((p = strstr(p, "\"properties\"")) && count < max_sheets) {
        p += 12;
        while (*p && *p != '{') p++;
        if (!*p) break;

        const char *prop_start = p;
        int brace = 1;
        const char *prop_end = prop_start + 1;
        while (*prop_end && brace > 0) {
            if (*prop_end == '{') brace++;
            if (*prop_end == '}') brace--;
            prop_end++;
        }
        size_t prop_len = prop_end - prop_start;

        const char *hidden_pos = find_key_bounded(prop_start, prop_len, "hidden");
        if (hidden_pos && is_true(hidden_pos)) {
            p = prop_end;
            continue;
        }

        const char *title_pos = find_key_bounded(prop_start, prop_len, "title");
        char *title = extract_string(title_pos);
        if (title) {
            strncpy(sheets[count], title, MAX_SHEET_NAME - 1);
            sheets[count][MAX_SHEET_NAME - 1] = '\0';
            free(title);
            count++;
        }

        p = prop_end;
    }

    if (count == 0) fprintf(stderr, "[diag] google metadata head: %.300s\n", response);
    free(response);
    return count;
}

/* ================================================================
 * Google Sheets: разбор JSON "values" (массив массивов строк)
 * ================================================================ */
static int extract_string_array(const char *pos, char ***out) {
    if (!pos || *pos != '[') return 0;
    pos++;

    int cap = 16;
    char **arr = malloc(sizeof(char *) * cap);
    int count = 0;

    while (*pos && *pos != ']') {
        if (*pos == '"') {
            char *s = extract_string(pos);
            if (s) {
                if (count >= cap) {
                    cap *= 2;
                    arr = realloc(arr, sizeof(char *) * cap);
                }
                arr[count++] = s;
                pos++;
                while (*pos && *pos != '"') {
                    if (*pos == '\\') pos++;
                    pos++;
                }
            }
        }
        if (*pos) pos++;
    }

    *out = arr;
    return count;
}

static void free_string_array(char **arr, int count) {
    for (int i = 0; i < count; i++) free(arr[i]);
    free(arr);
}

/* Parse a JSON array of arrays (rows) from the "values" field. */
static int parse_rows(const char *json, char ***rows_out, int **row_lens_out) {
    if (!json) return 0;

    const char *outer = strchr(json, '[');
    if (!outer) return 0;
    outer++;

    int cap = 32;
    char **rows = malloc(sizeof(char *) * cap);
    int *lens = malloc(sizeof(int) * cap);
    int count = 0;

    while (*outer && *outer != ']') {
        while (*outer && (*outer == ' ' || *outer == '\t' || *outer == '\n')) outer++;
        if (!*outer || *outer == ']') break;

        if (*outer == '[') {
            char **arr = NULL;
            int len = extract_string_array(outer, &arr);
            if (len > 0) {
                if (count >= cap) {
                    cap *= 2;
                    rows = realloc(rows, sizeof(char *) * cap);
                    lens = realloc(lens, sizeof(int) * cap);
                }
                rows[count] = (char *)arr;
                lens[count] = len;
                count++;
            } else {
                free(arr);
            }

            int brace = 0;
            while (*outer) {
                if (*outer == '[') brace++;
                if (*outer == ']') brace--;
                outer++;
                if (brace == 0) break;
            }
        } else {
            outer++;
        }
    }

    *rows_out = rows;
    *row_lens_out = lens;
    return count;
}

static void free_rows(char **rows, int *lens, int count) {
    for (int i = 0; i < count; i++) {
        free_string_array((char **)rows[i], lens[i]);
    }
    free(rows);
    free(lens);
}

/* ================================================================
 * Рекурсивный JSON-парсер в дерево (для Telegram update)
 * ================================================================ */
#include "cJSON.h"

typedef cJSON JNode;

static JNode *jparse(const char *json) {
    return cJSON_Parse(json);
}

static void jfree(JNode *n) {
    cJSON_Delete(n);
}

static const char *jstr(JNode *n) {
    return n ? cJSON_GetStringValue(n) : NULL;
}

static long long jll(JNode *n) {
    return n ? (long long)cJSON_GetNumberValue(n) : 0;
}

/* Находит узел по пути через точки, напр. "message.chat.id" */
static JNode *jfind(JNode *root, const char *path) {
    if (!root || !path) return NULL;
    JNode *cur = root;
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", path);
    char *save = NULL;
    for (char *tok = strtok_r(buf, ".", &save); tok; tok = strtok_r(NULL, ".", &save)) {
        cur = cJSON_GetObjectItemCaseSensitive(cur, tok);
        if (!cur) return NULL;
    }
    return cur;
}

/* ================================================================
 * Данные по листам
 * ================================================================ */
typedef struct {
    char title[MAX_SHEET_NAME];
    char **shift1; int shift1_len;
    char **shift2; int shift2_len;
} MonthEntry;

static char **copy_row(char **src, int len) {
    char **out = malloc(sizeof(char *) * (len > 0 ? len : 1));
    for (int i = 0; i < len; i++) out[i] = src[i] ? strdup(src[i]) : NULL;
    return out;
}

/* Собирает данные каждого видимого листа(=месяца) в массив. Возвращает кол-во. */
static int build_months(MonthEntry *out, int max, FILE *idx) {
    char sheets[MAX_SHEETS][MAX_SHEET_NAME];
    int sheet_count = get_visible_sheets(sheets, MAX_SHEETS);
    fprintf(stderr, "[updater] visible sheets=%d SEARCH_NAME='%s'\n", sheet_count, SEARCH_NAME ? SEARCH_NAME : "");
    int count = 0;

    for (int i = 0; i < sheet_count && count < max; i++) {
        char range[MAX_SHEET_NAME + 128];
        /* %.*s ограничивает имя листа — иначе gcc видит «плоский» массив
           sheets[50][128] строкой до 6399 байт и warns -Wformat-truncation.
           FROM_COL/TO_COL из getenv() — могут быть NULL. */
        snprintf(range, sizeof(range), "%.*s!%s:%s",
                 MAX_SHEET_NAME - 1, sheets[i],
                 FROM_COL ? FROM_COL : "", TO_COL ? TO_COL : "");

        char *encoded_range = url_encode(range);
        char url[2048];
        snprintf(url, sizeof(url),
            "https://sheets.googleapis.com/v4/spreadsheets/%s/values/%s?key=%s",
            SPREADSHEET_ID, encoded_range, GOOGLE_API_KEY);
        free(encoded_range);

            int http_status = 0;
    char *response = http_get(url, &http_status);
        if (!response) continue;

        const char *values_pos = strstr(response, "\"values\"");
        if (!values_pos) { free(response); continue; }

        char **rows = NULL;
        int *row_lens = NULL;
        int row_count = parse_rows(values_pos, &rows, &row_lens);
        if (row_count == 0) { free(response); continue; }

        int found_idx = -1;
        for (int r = 0; r < row_count; r++) {
            char **row = (char **)rows[r];
            int rlen = row_lens[r];
            if (rlen > 0 && row[0] && strcmp(row[0], SEARCH_NAME) == 0) {
                found_idx = r;
                break;
            }
        }
        if (found_idx < 0) { free_rows(rows, row_lens, row_count); free(response); continue; }

        /* Индекс кабинетов по ВСЕМ строкам листа.
           У человека две строки: утренняя (в графе C имя) и вечерняя
           (имя пустое — наследуется от предыдущей строки). */
        if (idx) {
            const char *cur_name = "";
            for (int r = 0; r < row_count; r++) {
                char **row = (char **)rows[r];
                int rlen = row_lens[r];
                if (rlen < 3) continue;
                const char *nm = row[0] ? row[0] : "";
                if (*nm) cur_name = nm;
                if (!*cur_name) continue;
                if (SEARCH_NAME && strcmp(cur_name, SEARCH_NAME) == 0) continue;
                const char *shift = row[1] ? row[1] : "";
                for (int d = 2; d < rlen; d++) {
                    const char *cabinet = row[d] ? row[d] : "";
                    if (!*cabinet) continue;
                    fprintf(idx, "%s\t%d\t%s\t%s\t%s\n",
                            sheets[i], d - 1, cabinet, cur_name, shift);
                }
            }
        }

        MonthEntry *m = &out[count];
        memset(m, 0, sizeof(MonthEntry));
        /* копирование с ограничением длины: strncpy от gcc-14/15 даёт
           -Wstringop-truncation (см. выше про плоский массив) */
        snprintf(m->title, sizeof(m->title), "%.*s", MAX_SHEET_NAME - 1, sheets[i]);

        m->shift1 = copy_row((char **)rows[found_idx], row_lens[found_idx]);
        m->shift1_len = row_lens[found_idx];
        if (found_idx + 1 < row_count) {
            m->shift2 = copy_row((char **)rows[found_idx + 1], row_lens[found_idx + 1]);
            m->shift2_len = row_lens[found_idx + 1];
        }

        free_rows(rows, row_lens, row_count);
        free(response);
        count++;
    }
    return count;
}

static void fwstr(FILE *fp, const char *s) {
    int len = s ? (int)strlen(s) : 0;
    fwrite(&len, sizeof(int), 1, fp);
    if (len) fwrite(s, 1, len, fp);
}

static void fwrow(FILE *fp, char **row, int len) {
    fwrite(&len, sizeof(int), 1, fp);
    for (int i = 0; i < len; i++) fwstr(fp, row[i]);
}

static int save_months(MonthEntry *mo, int count) {
    FILE *fp = fopen(MONTHS_FILE, "wb");
    if (!fp) return -1;
    fwrite(&count, sizeof(int), 1, fp);
    for (int i = 0; i < count; i++) {
        fwstr(fp, mo[i].title);
        fwrow(fp, mo[i].shift1, mo[i].shift1_len);
        fwrow(fp, mo[i].shift2, mo[i].shift2_len);
    }
    fclose(fp);
    return 0;
}

/* Освобождает строки месяца после сохранения в файл/кэш.
   Фоновый процесс работает постоянно, поэтому без этого была утечка. */
static void free_months(MonthEntry *mo, int count) {
    for (int i = 0; i < count; i++) {
        for (int j = 0; j < mo[i].shift1_len; j++) free(mo[i].shift1[j]);
        for (int j = 0; j < mo[i].shift2_len; j++) free(mo[i].shift2[j]);
        free(mo[i].shift1);
        free(mo[i].shift2);
        mo[i].shift1 = NULL;
        mo[i].shift2 = NULL;
        mo[i].shift1_len = 0;
        mo[i].shift2_len = 0;
    }
}

static char *json_escape(const char *s);

/* Разбирает смену вида "7:30-14:40" в минуты от полуночи.
   Возвращает 0 при успехе, -1 если формат не распознан. */
static int parse_shift(const char *s, int *start, int *end) {
    if (!s || !*s) return -1;
    char tmp[64];
    size_t j = 0;
    for (size_t i = 0; s[i] && j + 1 < sizeof(tmp); i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch == ' ') continue;
        if (ch >= 0xC0) {
            /* начало UTF-8 последовательности (тире – —) -> один '-' */
            tmp[j++] = '-';
            while (s[i + 1] && ((unsigned char)s[i + 1] & 0xC0) == 0x80) i++;
            continue;
        }
        if (ch >= 0x80) continue;
        tmp[j++] = (char)ch;
    }
    tmp[j] = '\0';
    int h1 = 0, m1 = 0, h2 = 0, m2 = 0;
    if (sscanf(tmp, "%d:%d-%d:%d", &h1, &m1, &h2, &m2) != 4) return -1;
    *start = h1 * 60 + m1;
    *end   = h2 * 60 + m2;
    if (*end < *start) { int t = *start; *start = *end; *end = t; }
    return 0;
}

/* Пересекаются ли две смены по времени (7:30-14:40 и 8:00-14:30 -> да).
   Если формат не распознан — сравниваем строки. */
static int shifts_overlap(const char *a, const char *b) {
    int s1, e1, s2, e2;
    if (parse_shift(a, &s1, &e1) != 0 || parse_shift(b, &s2, &e2) != 0) {
        if (!a || !b) return 0;
        return strcmp(a, b) == 0;
    }
    return s1 < e2 && s2 < e1;
}

/* Читает готовый индекс и возвращает JSON-массив коллег для месяца/дня/кабинета.
   out должен быть буфером достаточного размера. Возвращает длину JSON. */
static int find_cabinet_colleagues_from_index(const char *month, int day,
                                              const char *cabinet,
                                              const char *shift,
                                              char *out, size_t outcap) {
    FILE *fp = fopen(CABINET_INDEX_FILE, "r");
    if (!fp) {
        return snprintf(out, outcap, "[]");
    }
    int off = 0;
    int first = 1;
    char line[1024];
    char emitted_names[64][256];
    char emitted_shifts[64][128];
    int emitted = 0;
    while (fgets(line, sizeof(line), fp)) {
        char m[128] = "";
        int d = 0;
        char c[128] = "";
        char name[256] = "";
        char s[128] = "";
        char *p = line;
        // месяц
        char *t = strchr(p, '\t');
        if (!t) continue;
        size_t l = t - p;
        if (l >= sizeof(m)) l = sizeof(m) - 1;
        memcpy(m, p, l); m[l] = '\0';
        p = t + 1;
        // день
        t = strchr(p, '\t');
        if (!t) continue;
        l = t - p;
        if (l >= 16) l = 15;
        char ds[16] = "";
        memcpy(ds, p, l); ds[l] = '\0';
        d = atoi(ds);
        p = t + 1;
        // кабинет
        t = strchr(p, '\t');
        if (!t) continue;
        l = t - p;
        if (l >= sizeof(c)) l = sizeof(c) - 1;
        memcpy(c, p, l); c[l] = '\0';
        p = t + 1;
        // имя
        t = strchr(p, '\t');
        if (!t) continue;
        l = t - p;
        if (l >= sizeof(name)) l = sizeof(name) - 1;
        memcpy(name, p, l); name[l] = '\0';
        p = t + 1;
        // shift (до конца строки)
        l = strlen(p);
        while (l && (p[l-1] == '\n' || p[l-1] == '\r')) p[--l] = '\0';
        if (l >= sizeof(s)) l = sizeof(s) - 1;
        memcpy(s, p, l); s[l] = '\0';

        if (strcmp(m, month) != 0) continue;
        if (d != day) continue;
        if (strcmp(c, cabinet) != 0) continue;
        // фильтр по смене: коллега должен работать в пересекающемся интервале
        if (shift && *shift && !shifts_overlap(s, shift)) continue;

        /* убираем дубли (один человек может встретиться в двух строках) */
        int dup = 0;
        for (int q = 0; q < emitted; q++) {
            if (strcmp(emitted_names[q], name) == 0 &&
                strcmp(emitted_shifts[q], s) == 0) { dup = 1; break; }
        }
        if (dup) continue;

        if (emitted < 64) {
            snprintf(emitted_names[emitted], sizeof(emitted_names[0]), "%s", name);
            snprintf(emitted_shifts[emitted], sizeof(emitted_shifts[0]), "%s", s);
            emitted++;
        }

        if (first) {
            /* открываем JSON-массив */
            off += snprintf(out + off, outcap - off, "[");
        } else {
            off += snprintf(out + off, outcap - off, ",");
        }
        first = 0;
        char *en = json_escape(name);
        char *es = json_escape(s);
        off += snprintf(out + off, outcap - off,
            "{\"name\":\"%s\",\"shift\":\"%s\"}",
            en ? en : name, es ? es : s);
        free(en);
        free(es);
    }
    fclose(fp);
    if (first) {
        // пустой результат
        off = snprintf(out, outcap, "[]");
    } else {
        // закрываем JSON-массив
        off += snprintf(out + off, outcap - off, "]");
    }
    return off;
}

static int frstr(FILE *fp, char **out) {
    int len;
    if (fread(&len, sizeof(int), 1, fp) != 1) return -1;
    char *s = malloc(len + 1);
    if (len && fread(s, 1, len, fp) != (size_t)len) { free(s); return -1; }
    s[len] = '\0';
    *out = s;
    return 0;
}

static int frrow(FILE *fp, char ***out) {
    int len;
    if (fread(&len, sizeof(int), 1, fp) != 1) return -1;
    char **row = malloc(sizeof(char *) * (len > 0 ? len : 1));
    for (int i = 0; i < len; i++) {
        char *s = NULL;
        if (frstr(fp, &s) < 0) {
            for (int k = 0; k < i; k++) free(row[k]);
            free(row);
            return -1;
        }
        row[i] = s;
    }
    *out = row;
    return len;
}

static int read_months(MonthEntry *mo, int max) {
    FILE *fp = fopen(MONTHS_FILE, "rb");
    if (!fp) return 0;
    int count;
    if (fread(&count, sizeof(int), 1, fp) != 1) { fclose(fp); return 0; }
    if (count > max) count = max;
    int got = 0;
    for (int i = 0; i < count; i++) {
        MonthEntry *m = &mo[i];
        memset(m, 0, sizeof(MonthEntry));
        char *title = NULL;
        if (frstr(fp, &title) < 0) break;
        strncpy(m->title, title ? title : "", MAX_SHEET_NAME - 1);
        m->title[MAX_SHEET_NAME - 1] = '\0';
        free(title);
        int l1 = frrow(fp, &m->shift1);
        if (l1 < 0) break;
        m->shift1_len = l1;
        int l2 = frrow(fp, &m->shift2);
        if (l2 < 0) break;
        m->shift2_len = l2;
        got++;
    }
    fclose(fp);
    return got;
}

/* Строит SVG месяца в *out (malloc). Возвращает 1 при успехе. */
static char *json_escape(const char *s) {
    size_t cap = strlen(s ? s : "") * 2 + 16, len = 0;
    char *o = malloc(cap);
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        if (len + 6 > cap) { cap *= 2; o = realloc(o, cap); }
        if (*p == '"' || *p == '\\') { o[len++] = '\\'; o[len++] = *p; }
        else if (*p == '\n') { o[len++] = '\\'; o[len++] = 'n'; }
        else if (*p == '\r') { o[len++] = '\\'; o[len++] = 'r'; }
        else o[len++] = *p;
    }
    o[len] = '\0';
    return o;
}

static void set_chat_menu_button(void);

static void tg_send_text(long long chat_id, const char *text) {
    char url[512];
    char *enc_text = url_encode(text);
    char post[4096];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", BOT_TOKEN);
    snprintf(post, sizeof(post), "chat_id=%lld&text=%s", chat_id, enc_text ? enc_text : text);
    free(enc_text);
    http_post_form(url, post);
}

/* Клавиатура учёта времени (Пришла / Ушла / Деньги) в JSON-виде. */
#define TRACK_KEYBOARD_JSON "{\"keyboard\":[[{\"text\":\"Пришла\"},{\"text\":\"Ушла\"},{\"text\":\"Деньги\"}]],\"resize_keyboard\":true,\"is_persistent\":true}"

/* Отправляет текст вместе с клавиатурой — так клавиатура всегда остаётся
   на экране, даже если пользователь её раньше скрыл. */
static void tg_send_text_kb(long long chat_id, const char *text) {
    char url[512];
    char *enc_text = url_encode(text);
    char *enc_markup = url_encode(TRACK_KEYBOARD_JSON);
    char post[8192];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", BOT_TOKEN);
    snprintf(post, sizeof(post), "chat_id=%lld&text=%s&reply_markup=%s",
        chat_id, enc_text ? enc_text : text,
        enc_markup ? enc_markup : TRACK_KEYBOARD_JSON);
    free(enc_text);
    free(enc_markup);
    http_post_form(url, post);
}

/* Подтверждает нажатие на inline-кнопку.
   id в Telegram — строка (например "4382bfdwdsb323b2d9"), поэтому принимаем const char*.
   text передаётся как POST-параметр с --data-urlencode (корректный URL-кодинг). */
static void tg_answer(const char *cq_id, const char *text) {
    if (!cq_id || !*cq_id) return;
    char url[512];
    char *enc_cq = url_encode(cq_id);
    char *enc_text = text ? url_encode(text) : NULL;
    char post[2048];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/answerCallbackQuery", BOT_TOKEN);
    if (enc_text) {
        snprintf(post, sizeof(post), "callback_query_id=%s&text=%s", enc_cq, enc_text);
        free(enc_text);
    } else {
        snprintf(post, sizeof(post), "callback_query_id=%s", enc_cq);
    }
    free(enc_cq);
    http_post_form(url, post);
}


/* ================================================================
 * Состояние чатов и отдача графика
 * ================================================================ */
typedef struct { long long chat_id; int idx; } ChatState;
static ChatState chats[MAX_CHATS];
static int chat_count = 0;

static int get_chat(long long id) {
    for (int i = 0; i < chat_count; i++) if (chats[i].chat_id == id) return i;
    if (chat_count < MAX_CHATS) { chats[chat_count].chat_id = id; chats[chat_count].idx = -1; return chat_count++; }
    return -1;
}

static int pick_current_index(MonthEntry *mo, int count) {
    (void)mo;
    if (count <= 0) return -1;
    return count - 1; /* последний лист по умолчанию */
}

static void show_index(long long cid, int idx, MonthEntry *mo, int count) {
    if (idx < 0 || idx >= count) { tg_send_text_kb(cid, "Нет данных по графикам."); return; }
    char label[64];
    tracking_month_label(mo[idx].title, label, sizeof(label));
    char msg[256];
    snprintf(msg, sizeof(msg), "График: %s", label);
    tg_send_text_kb(cid, msg);
}

static const char HELP_MSG[] =
    "Привет! Я бот-график смен.\n\n"
    "График — кнопка меню чата\n"
    "Отметить время — кнопки внизу\n"
    "Деньги — заработок по месяцам";

/* ------------------------------------------------------------------
 *  Кнопки учёта времени прямо в чате бота
 * ------------------------------------------------------------------ */

/* Отправляет сообщение с кнопками «Пришла»/«Ушла» (reply-клавиатура). */
static void tg_send_track_menu(long long chat_id) {
    char url[512];
    char *enc_text = url_encode("Отметьте рабочее время:");
    char *enc_markup = url_encode(TRACK_KEYBOARD_JSON);
    char post[8192];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", BOT_TOKEN);
    snprintf(post, sizeof(post), "chat_id=%lld&text=%s&reply_markup=%s",
        chat_id, enc_text ? enc_text : "Отметьте рабочее время:",
        enc_markup ? enc_markup : TRACK_KEYBOARD_JSON);
    free(enc_text);
    free(enc_markup);
    http_post_form(url, post);
}
static void tg_send_track_inline(long long chat_id) {
    char url[512];
    char *enc_text = url_encode("Отметьте рабочее время:");
    char *enc_markup = url_encode("{\"inline_keyboard\":[[{\"text\":\"Пришла\",\"callback_data\":\"trk_in\"},{\"text\":\"Ушла\",\"callback_data\":\"trk_out\"}],{\"text\":\"Деньги\",\"callback_data\":\"trk_money\"}]}");
    char post[8192];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", BOT_TOKEN);
    snprintf(post, sizeof(post), "chat_id=%lld&text=%s&reply_markup=%s",
        chat_id, enc_text ? enc_text : "Отметьте рабочее время:",
        enc_markup ? enc_markup : "{\"inline_keyboard\":[[{\"text\":\"Пришла\",\"callback_data\":\"trk_in\"},{\"text\":\"Ушла\",\"callback_data\":\"trk_out\"}],{\"text\":\"Деньги\",\"callback_data\":\"trk_money\"}]}");
    free(enc_text);
    free(enc_markup);
    http_post_form(url, post);
}

static void tg_send_money_menu(long long chat_id) {
    char **months = NULL;
    int count = 0;
    char kb[4096];
    size_t o = 0;
    int in_row = 0;

    if (tracking_list_months(&months, &count, 24) != 0 || count == 0) {
        tg_send_text(chat_id, "Не удалось получить список месяцев. Проверьте доступ к таблице.");
        return;
    }
    o += (size_t)snprintf(kb + o, sizeof(kb) - o, "{\"inline_keyboard\":[[");
    for (int i = 0; i < count && o < sizeof(kb) - 256; i++) {
        char label[64];
        tracking_month_label(months[i], label, sizeof(label));
        char *el = json_escape(label);
        char *cd = json_escape(months[i]);
        o += (size_t)snprintf(kb + o, sizeof(kb) - o,
                              "%s{\"text\":\"%s\",\"callback_data\":\"mon:%s\"}",
                              in_row ? "," : "", el ? el : "", cd ? cd : "");
        free(el);
        free(cd);
        in_row++;
        if (in_row == 3 && i < count - 1) {
            o += (size_t)snprintf(kb + o, sizeof(kb) - o, "],[");
            in_row = 0;
        }
    }
    snprintf(kb + o, sizeof(kb) - o, "]]}");

    char url[512];
    char *enc_text = url_encode("Заработок по месяцам — выберите месяц:");
    char *enc_markup = url_encode(kb);
    char post[8192];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", BOT_TOKEN);
    snprintf(post, sizeof(post), "chat_id=%lld&text=%s&reply_markup=%s",
        chat_id, enc_text ? enc_text : "Заработок по месяцам — выберите месяц:",
        enc_markup ? enc_markup : kb);
    free(enc_text);
    free(enc_markup);
    http_post_form(url, post);

    for (int i = 0; i < count; i++) free(months[i]);
    free(months);
}

/* Присылает в чат итог по выбранному месяцу. */
static void tg_send_money_total(long long chat_id, const char *month) {
    double hours = 0, sum = 0;
    int days = 0;
    char label[64];
    tracking_month_label(month, label, sizeof(label));

    if (tracking_month_total(month, &hours, &sum, &days) != 0) {
        tg_send_text(chat_id, "Не удалось посчитать заработок за этот месяц.");
        return;
    }
    if (days <= 0 && sum <= 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "%s: отметок нет.", label);
        tg_send_text_kb(chat_id, msg);
        return;
    }
    char msg[512];
    snprintf(msg, sizeof(msg),
             "%s\nОтработано: %.2f ч. за %d дн.\nЗаработано: %.2f",
             label, hours, days, sum);
    tg_send_text_kb(chat_id, msg);
}

/* Выполняет отметку и отправляет результат в чат. */
static void bot_track(long long chat_id, const char *action) {
    char json[1024];
    int code = tracking_do_clock(action, json, sizeof(json));

    JNode *r = jparse(json);
    const char *err = r ? jstr(jfind(r, "error")) : NULL;
    char msg[768];

    if (code == 200) {
        const char *t = r ? jstr(jfind(r, "time")) : NULL;
        if (strcmp(action, "in") == 0) {
            snprintf(msg, sizeof(msg), "Записано! Приход в %s", t ? t : "");
        } else {
            const char *h = r ? jstr(jfind(r, "hours")) : NULL;
            const char *s = r ? jstr(jfind(r, "sum")) : NULL;
            snprintf(msg, sizeof(msg), "Записано! Уход в %s", t ? t : "");
            if (h && *h) {
                size_t l = strlen(msg);
                snprintf(msg + l, sizeof(msg) - l, "\nОтработано: %s ч.", h);
            }
            if (s && *s) {
                size_t l = strlen(msg);
                snprintf(msg + l, sizeof(msg) - l, "\nСумма: %s", s);
            }
        }
    } else {
        snprintf(msg, sizeof(msg), "Ошибка: %s",
                 (err && *err) ? err : "Не удалось записать");
    }
    if (r) jfree(r);

    /* Ответ идёт вместе с клавиатурой: она всегда остаётся на экране */
    tg_send_text_kb(chat_id, msg);
}

/* ================================================================
 * Обработка Telegram-update
 * ================================================================ */
static void process_update(const char *body) {
    JNode *root = jparse(body);
    if (!root) return;

    JNode *msg = jfind(root, "message");
    if (msg && jfind(root, "message.chat.id")) {
        long long cid = jll(jfind(root, "message.chat.id"));
        const char *text = jstr(jfind(root, "message.text"));
        if (text && *text) {
            static char low[1024];
            int j = 0;
            for (int i = 0; text[i] && j < 1022; i++) low[j++] = (char)tolower((unsigned char)text[i]);
            low[j] = '\0';

            /* Кнопки учёта времени (reply-клавиатура) */
            if (strcmp(low, "/start") == 0 || strcmp(low, "/help") == 0 ||
                strcmp(low, "/start@") == 0) {
                /* /start и /help — принудительно обновляют клавиатуру */
                tg_send_text_kb(cid, HELP_MSG);
                tg_send_track_menu(cid);
            } else if (strstr(text, "Пришла") || strcmp(low, "/in") == 0) {
                bot_track(cid, "in");
            } else if (strstr(text, "Ушла") || strcmp(low, "/out") == 0) {
                bot_track(cid, "out");
            } else if (strstr(text, "Деньги") || strcmp(low, "/money") == 0 ||
                       strcmp(low, "/zarabotok") == 0) {
                tg_send_money_menu(cid);
            } else if (strstr(low, "/track") || strstr(low, "/time") ||
                       strstr(text, "Отметить") || strstr(text, "время")) {
                tg_send_track_inline(cid);
                tg_send_track_menu(cid);
            } else {
                tg_send_text(cid, HELP_MSG);
                tg_send_track_menu(cid);
            }
        }
        jfree(root);
        return;
    }

    if (jfind(root, "callback_query")) {
        /* id в callback_query — строка, не число: читаем через jstr() */
        const char *cqid = jstr(jfind(root, "callback_query.id"));
        long long cid  = jll(jfind(root, "callback_query.message.chat.id"));
        const char *data = jstr(jfind(root, "callback_query.data"));

        MonthEntry mo[MAX_MONTHS];
        memset(mo, 0, sizeof(mo));
        int count = read_months(mo, MAX_MONTHS);

        int gi = get_chat(cid);
        int idx = (gi >= 0) ? chats[gi].idx : -1;
        if (idx < 0) idx = pick_current_index(mo, count);

        if (data) {
            /* Кнопки учёта времени */
            if (strcmp(data, "trk_in") == 0) {
                bot_track(cid, "in");
                if (cqid) tg_answer(cqid, "Записано");
                jfree(root);
                return;
            }
            if (strcmp(data, "trk_out") == 0) {
                bot_track(cid, "out");
                if (cqid) tg_answer(cqid, "Записано");
                jfree(root);
                return;
            }
            /* Кнопка «Деньги»: показать список месяцев */
            if (strcmp(data, "trk_money") == 0) {
                tg_send_money_menu(cid);
                if (cqid) tg_answer(cqid, NULL);
                jfree(root);
                return;
            }
            /* Выбран месяц: присылаем сумму заработка за него */
            if (strncmp(data, "mon:", 4) == 0) {
                tg_send_money_total(cid, data + 4);
                if (cqid) tg_answer(cqid, NULL);
                jfree(root);
                return;
            }
            if (strcmp(data, "prev") == 0) { if (idx > 0) idx--; }
            else if (strcmp(data, "next") == 0) { if (idx < count - 1) idx++; }
        }
        if (gi >= 0) chats[gi].idx = idx;
        show_index(cid, idx, mo, count);
        if (cqid) tg_answer(cqid, NULL);
        jfree(root);
        return;
    }

    jfree(root);
}

/* ================================================================
 * URL decode (query string)
 * ================================================================ */
static void url_decode(char *dst, size_t dstcap, const char *src) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j < dstcap - 1; i++) {
        if (src[i] == '%' && src[i+1] && src[i+2]) {
            unsigned int v = 0;
            if (sscanf(src + i + 1, "%2x", &v) == 1) {
                dst[j++] = (char)v;
                i += 2;
                continue;
            }
        }
        if (src[i] == '+') { dst[j++] = ' '; continue; }
        dst[j++] = src[i];
    }
    dst[j] = '\0';
}

/* ================================================================
 * Mini App: HTML + API
 * ================================================================ */
static void serve_miniapp(int fd) {
    const char html_prefix[] =
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no'>"
        "<title>График смен</title>"
        "<style>"
        ":root{--bg:#f5f5f5;--fg:#222;--card:#fff;--border:#e0e0e0;--head:#4CAF50;--head-fg:#fff;--name-bg:#f1f8e9;--stripe:#f9f9f9}"
        "body.dark,body[data-theme='dark']{--bg:#1e1e1e;--fg:#e0e0e0;--card:#2d2d2d;--border:#444;--head:#2E7D32;--head-fg:#fff;--name-bg:#1b5e20;--stripe:#3a3a3a}"
        "body{margin:0;font-family:DejaVu Sans,Arial,sans-serif;background:var(--bg);color:var(--fg)}"
        ".c{padding:10px}"
        ".m{display:flex;flex-wrap:wrap;gap:8px;margin-bottom:12px}"
        ".b{padding:8px 12px;border:none;border-radius:6px;background:var(--head);color:var(--head-fg);font-size:14px;cursor:pointer}"
        ".b:hover{opacity:.85}"
        ".b.active{outline:3px solid #FF9800;outline-offset:2px;background:#FF9800;color:#fff;font-weight:600}"
        ".t{overflow-x:auto;background:var(--card);border-radius:8px;box-shadow:0 2px 8px rgba(0,0,0,.1)}"
        "table{border-collapse:collapse;width:100%;min-width:520px}"
        "th,td{border:1px solid var(--border);padding:8px;text-align:center;font-size:14px}"
        "thead th{background:var(--head);color:var(--head-fg);font-weight:bold}"
        ".cal-info{display:flex;flex-wrap:wrap;gap:6px 14px;padding:10px 10px 6px;align-items:center;font-size:13px}"
        ".cal-title{font-weight:bold;font-size:15px}"
        ".cal-legend{opacity:.9}"
        ".cal{display:grid;grid-template-columns:repeat(7,1fr);gap:4px;padding:4px 10px 10px}"
        ".cal-h{text-align:center;font-weight:bold;font-size:12px;padding:6px 2px;border-radius:4px;background:var(--head);color:var(--head-fg)}"
        ".cal-h.we{background:#C62828}"
        ".cal-d{position:relative;min-height:82px;box-sizing:border-box;border:1px solid var(--border);border-radius:6px;background:var(--card);padding:6px 4px 7px;display:flex;flex-direction:column;align-items:center;gap:4px;overflow:hidden}"
        ".cal-d.pad{visibility:hidden}"
        ".cal-d[data-has]{cursor:pointer}"
        ".cal-d.we{background:#ffebee;border-color:#f3c6c6}"
        ".cal-d.we .cal-num{color:#C62828}"
        ".cal-d.we-work::after{content:'раб.';position:absolute;right:3px;bottom:2px;font-size:9px;font-weight:bold;color:#2E7D32}"
        ".cal-d.done{background:#e8f5e9;border-color:#c8e6c9}"
        ".cal-today{outline:2px solid #FF9800;outline-offset:-2px}"
        ".cal-num{font-size:13px;font-weight:bold;line-height:1.2}"
        ".cal-mark{position:absolute;top:3px;right:4px;color:#2E7D32;font-size:12px;font-weight:bold}"
        ".cal-cab{display:block;width:100%;box-sizing:border-box;font-size:12px;font-weight:600;line-height:1.2;padding:6px 2px;border-radius:5px;background:rgba(26,115,232,.10);color:#1a73e8;cursor:pointer;text-align:center;user-select:none;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}"
        ".cal-cab:hover{background:rgba(26,115,232,.20)}"
        ".cal-sum{display:none;font-size:11px;font-weight:bold;color:#2E7D32;letter-spacing:-.2px}"
        ".cal.show-sum .cal-sum{display:block}"
        "#trk-money.on{outline:3px solid #FF9800;outline-offset:2px;background:#FF9800}"
        "body[data-theme='dark'] .cal-cab{background:rgba(100,180,255,.16);color:#64b4ff}"
        "body[data-theme='dark'] .cal-cab:hover{background:rgba(100,180,255,.28)}"
        "body[data-theme='dark'] .cal-sum{color:#81c784}"
        "body[data-theme='dark'] .cal-d.we{background:#3a2424;border-color:#5c3535}"
        "body[data-theme='dark'] .cal-d.done{background:#1c3325;border-color:#2e5b3d}"
        "tbody tr:nth-child(even){background:var(--stripe)}"
        ".n{font-weight:bold;background:var(--name-bg);min-width:180px}"
        ".x{display:flex;gap:8px;margin-top:12px}"
        ".x button{flex:1;padding:10px;border:none;border-radius:6px;background:var(--head);color:var(--head-fg);font-size:16px;cursor:pointer}"
        ".h{display:none}"
        ".trk{margin-top:16px;padding:12px;background:var(--card);border-radius:8px}"
        ".trk-banner{display:none;margin-top:10px;padding:12px;border-radius:6px;font-weight:bold;font-size:16px}"
        ".trk-banner.ok{background:#e8f5e9;color:#2E7D32;border:2px solid #2E7D32}"
        ".trk-banner.err{background:#ffebee;color:#C62828;border:2px solid #C62828}"
        ".trk-banner.ok{display:block}"
        ".trk-banner.err{display:block}"
        ".trk h3{margin:0 0 10px;font-size:15px}"
        ".trk .row{display:flex;gap:8px}"
        ".trk button{flex:1;padding:12px;border:none;border-radius:6px;color:#fff;font-size:15px;cursor:pointer}"
        ".trk .in{background:#2E7D32}"
        ".trk .mn{background:#1565C0}"
        ".trk .out{background:#C62828}"
        ".trk button:disabled{opacity:.5}"
        ".trk .st{margin-top:8px;font-size:13px;color:var(--fg);opacity:.8}"
        ".trk-modal{display:none;position:fixed;inset:0;background:rgba(0,0,0,.55);z-index:100;align-items:center;justify-content:center;padding:16px}"
        ".trk-modal.open{display:flex}"
        ".trk-box{background:var(--card);color:var(--fg);border-radius:10px;padding:16px;width:100%;max-width:360px;box-sizing:border-box;box-shadow:0 8px 24px rgba(0,0,0,.35)}"
        ".trk-box h4{margin:0 0 12px;font-size:16px}"
        ".trk-box label{display:block;margin:10px 0 4px;font-size:13px;opacity:.8}"
        ".trk-box input{width:100%;box-sizing:border-box;padding:10px;border:1px solid var(--border);border-radius:6px;background:var(--bg);color:var(--fg);font-size:16px}"
        ".trk-box .row{display:flex;gap:8px;margin-top:14px;align-items:center}"
        ".trk-box button{flex:1;padding:11px;border:none;border-radius:6px;font-size:15px;cursor:pointer}"
        ".trk-box .save{background:#2E7D32;color:#fff}"
        ".trk-box .cancel{background:#777;color:#fff}"
        ".trk-box button:disabled{opacity:.5}"
        ".trk-err{min-height:18px;margin-top:8px;font-size:13px;color:#C62828}"
        "</style>"
        "<script>"
        "function applyTheme(){var s='light';if(window.Telegram&&window.Telegram.WebApp&&window.Telegram.WebApp.colorScheme)s=window.Telegram.WebApp.colorScheme;document.body.setAttribute('data-theme',s);}"
        "function esc(s){if(!s)return '';return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/\"/g,'&quot;');}"
        "function parseMonthTitle(t){"
        "if(!t)return null;var s=(''+t).trim();"
        "var m=s.match(/^(\\d{1,2})\\.(\\d{4})$/);if(m)return{m:parseInt(m[1],10),y:parseInt(m[2],10)};"
        "var NM=['Январь','Февраль','Март','Апрель','Май','Июнь','Июль','Август','Сентябрь','Октябрь','Ноябрь','Декабрь'];"
        "for(var i=0;i<12;i++){if(s.indexOf(NM[i])===0){var y=s.match(/(\\d{4})/);return {m:i+1,y:y?parseInt(y[1],10):new Date().getFullYear()};}}"
        "return null;}"
        "function showTable(d){"
        "var w=document.getElementById('table');if(!w)return;"
        "var s1=d.s1||[];var s2=d.s2||[];"
        "var n=Math.max(s1.length,s2.length,1);"
        "var days=n>3?n-3:0;"
        "var sh1=(s1[1]||'').trim(),sh2=(s2[1]||'').trim(),total=s1[n-1]||s2[n-1]||'';"
        "var pm=parseMonthTitle(d.t);"
        "var dim=days;"
        "if(pm){var md=new Date(pm.y,pm.m,0).getDate();if(md>dim)dim=md;}"
        "var lead=pm?(new Date(pm.y,pm.m-1,1).getDay()+6)%7:0;"
        "var h='<div class=\"cal-info\">';"
        "h+='<span class=\"cal-title\">'+esc(d.t||'')+'</span>';"
        "if(sh1)h+='<span class=\"cal-legend\">Смена 1: <b>'+esc(sh1)+'</b></span>';"
        "if(sh2)h+='<span class=\"cal-legend\">Смена 2: <b>'+esc(sh2)+'</b></span>';"
        "if(total)h+='<span class=\"cal-legend\">Всего смен: <b>'+esc(total)+'</b></span>';"
        "h+='<span class=\"cal-legend\" id=\"cal-money\"></span>';"
        "h+='</div>';"
        "h+='<div class=\"cal\">';"
        "var WD=['Пн','Вт','Ср','Чт','Пт','Сб','Вс'];"
        "for(var i=0;i<7;i++){h+='<div class=\"cal-h'+(i>4?' we':'')+'\">'+WD[i]+'</div>';}"
        "for(var i=0;i<lead;i++)h+='<div class=\"cal-d pad\"></div>';"
        "CAL_HAS={};"
        "for(var day=1;day<=dim;day++){"
        "var c1=(s1[day+1]||'').trim();"
        "var c2=(s2[day+1]||'').trim();"
        "var has=!!(c1||c2);if(has)CAL_HAS[day]=1;"
        "var we=pm?((lead+day-1)%7>4):false;"
        "h+='<div class=\"cal-d'+(we?' we':'')+(we&&has?' we-work':'')+'\" data-day=\"'+day+'\"'+(has?' data-has=\"1\"':'')+'>';"
        "h+='<div class=\"cal-num\">'+day+'</div>';"
        "if(c1)h+='<span class=\"cal-cab\" data-day=\"'+day+'\" data-cabinet=\"'+esc(c1)+'\" data-shift=\"'+esc(sh1)+'\" title=\"'+esc(sh1)+'\">'+esc(c1)+'</span>';"
        "if(c2)h+='<span class=\"cal-cab\" data-day=\"'+day+'\" data-cabinet=\"'+esc(c2)+'\" data-shift=\"'+esc(c2===c1?'':sh2)+'\" title=\"'+esc(sh2)+'\">'+esc(c2)+'</span>';"
        "h+='<span class=\"cal-sum\"></span>';"
        "h+='</div>';"
        "}"
        "var tail=(lead+dim)%7;if(tail){for(var i=tail;i<7;i++)h+='<div class=\"cal-d pad\"></div>';}"
        "h+='</div>';"
        "w.innerHTML=h;w.classList.remove('h');"
        "w.querySelectorAll('.cal-cab').forEach(function(el){"
        "el.onclick=function(e){if(e&&e.stopPropagation)e.stopPropagation();"
        "showColleagues(parseInt(this.dataset.day,10),this.dataset.cabinet,this.dataset.shift);};"
        "});"
        "w.querySelectorAll('.cal-d[data-day]').forEach(function(cell){"
        "cell.onclick=function(){dayAction(parseInt(cell.getAttribute('data-day'),10));};"
        "});"
        "trkSelectedMonth=(d.t||M[cur]||'');"
        "var tk=d.trk;"
        "if(tk&&tk.ok){TRK=tk.days||{};"
        "TRK_META={month:tk.month||'',label:tk.label||trkSelectedMonth,today:tk.today||''};"
        "renderTracking();"
        "}else{TRK={};TRK_META={};loadTracking(trkSelectedMonth);}"
        "}"
        "function showColleagues(day,cabinet,shift){if(!cabinet)return;var m=M[cur];fetch('/api/cabinet?month='+encodeURIComponent(m)+'&cabinet='+encodeURIComponent(cabinet)+'&day='+day+(shift?'&shift='+encodeURIComponent(shift):'')).then(function(r){return r.json();}).then(function(d){var l=d.colleagues||[];var t='Кабинет '+cabinet+', день '+day+(m?' ('+m+')':'')+(shift?', смена '+shift:'');t+=l.length?'\\n\\nВместе с Бинкевич Татьяна Эдуардовна:\\n'+l.map(function(c){return '• '+c.name+(c.shift?' — '+c.shift:'');}).join('\\n'):'\\n\\nНикто другой не работает в этом кабинете в этот день.';alert(t);});}"
        "var M=[];var cur=-1;var CAL_HAS={};var MONEY_VIEW=false;"
        "var TRK={},TRK_META={},trkSelectedMonth='',trkPendingDay=0,trkPendingMonth='';"
        "function pad2(n){return (n<10?'0':'')+n;}"
        "function shiftTimes(s){"
        "if(!s)return ['',''];"
        "var t=(s+'').replace(/[–—−]/g,'-').replace(/\\s/g,'');"
        "var m=t.match(/(\\d{1,2}):(\\d{2})-(\\d{1,2}):(\\d{2})/);"
        "if(!m)return ['',''];"
        "return [pad2(parseInt(m[1],10))+':'+m[2],pad2(parseInt(m[3],10))+':'+m[4]];"
        "}"
        "function dayAllowed(day){"
        "if(!TRK_META.month||!TRK_META.today)return true;"
        "var tp=TRK_META.today.split('.'),mp=TRK_META.month.split('.');"
        "if(tp.length<3||mp.length<2)return true;"
        "var td=parseInt(tp[0],10),tm=parseInt(tp[1],10),ty=parseInt(tp[2],10);"
        "var mm=parseInt(mp[0],10),my=parseInt(mp[1],10),d=parseInt(day,10);"
        "if(my<ty)return true;if(my>ty)return false;"
        "if(mm<tm)return true;if(mm>tm)return false;"
        "return d<=td;"
        "}"
        "function renderTracking(){"
        "var cells=document.querySelectorAll('.cal-d[data-day]');"
        "var td=0;"
        "if(TRK_META.today&&TRK_META.month){var tp=(''+TRK_META.today).split('.'),mp=(''+TRK_META.month).split('.');"
        "if(tp.length===3&&mp.length===2&&parseInt(mp[0],10)===parseInt(tp[1],10)&&parseInt(mp[1],10)===parseInt(tp[2],10))td=parseInt(tp[0],10);}"
        "for(var i=0;i<cells.length;i++){(function(cell){"
        "var day=parseInt(cell.getAttribute('data-day'),10);"
        "var old=cell.querySelector('.cal-mark');if(old)old.parentNode.removeChild(old);"
        "cell.classList.remove('cal-done','cal-today');"
        "if(td&&day===td)cell.classList.add('cal-today');"
        "var rec=TRK[day]||null;var done=!!(rec&&rec.in&&rec.out);"
        "var has=cell.getAttribute('data-has')==='1';"
        "if(done){"
        "cell.classList.add('cal-done');"
        "var mk=document.createElement('span');mk.className='cal-mark';mk.textContent='✓';"
        "mk.title='Приход '+rec.in+', уход '+rec.out+(rec.hours?', '+rec.hours+' ч.':'');"
        "cell.appendChild(mk);"
        "}"
        "var sumEl=cell.querySelector('.cal-sum');"
        "if(sumEl){var sv=parseFloat(rec&&rec.sum);sumEl.textContent=(isFinite(sv)&&sv>0)?sv.toFixed(2):'';}"
        "cell.title=has?(done?'Просмотр отработанного времени':'Тап — внести время за день'):'Смен в этот день нет';"
        "})(cells[i]);}"
        "updateMoneyInfo();"
        "}"
        "function loadTracking(month){"
        "if(!month)return;"
        "fetch('/api/tracking/days?month='+encodeURIComponent(month)).then(function(r){return r.json();})"
        ".then(function(d){"
        "if(!d||!d.ok||trkSelectedMonth!==month)return;"
        "TRK=d.days||{};TRK_META={month:d.month||'',label:d.label||month,today:d.today||''};"
        "renderTracking();"
        "}).catch(function(){});"
        "}"
        "function dayAction(day){"
        "if(!CAL_HAS||!CAL_HAS[day]){return;}"
        "var rec=TRK[day]||null;"
        "if(rec&&rec.in&&rec.out){"
        "var t='День '+day+' ('+(TRK_META.label||trkSelectedMonth||'')+'):\\nПриход: '+rec.in+'\\nУход: '+rec.out;"
        "if(rec.hours)t+='\\nОтработано: '+rec.hours+' ч.';"
        "if(rec.sum)t+='\\nСумма: '+rec.sum;"
        "alert(t);return;"
        "}"
        "openTrkDialog(day,'','');"
        "}"
        "function openTrkDialog(day,cabinet,shift){"
        "if(!CAL_HAS||!CAL_HAS[day]){trkMsg('В этот день смен нет — время не вносится',false);return;}"
        "var ex=TRK[day]||null;if(ex&&ex.in&&ex.out){trkMsg('За день '+day+' время уже отмечено: '+ex.in+'–'+ex.out,false);return;}"
        "trkPendingDay=day;trkPendingMonth=M[cur]||TRK_META.label||'';"
        "var rec=TRK[day]||{};var st=shiftTimes(shift);"
        "var i=document.getElementById('trk-in-time'),o=document.getElementById('trk-out-time');"
        "if(i)i.value=rec.in||st[0]||'';if(o)o.value=rec.out||st[1]||'';"
        "var t=document.getElementById('trk-modal-title');"
        "if(t)t.textContent=(cabinet?'Кабинет '+cabinet+', ':'')+'день '+day+' ('+trkPendingMonth+')';"
        "var e=document.getElementById('trk-modal-err');if(e)e.textContent='';"
        "var m=document.getElementById('trk-modal');if(m)m.classList.add('open');"
        "}"
        "function closeTrkDialog(){"
        "var m=document.getElementById('trk-modal');if(m)m.classList.remove('open');"
        "var e=document.getElementById('trk-modal-err');if(e)e.textContent='';"
        "trkPendingDay=0;trkPendingMonth='';"
        "}"
        "function saveTrkDialog(){"
        "var i=document.getElementById('trk-in-time'),o=document.getElementById('trk-out-time');"
        "var e=document.getElementById('trk-modal-err'),b=document.getElementById('trk-modal-save');"
        "var iv=i?i.value:'',ov=o?o.value:'';"
        "if(!iv||!ov){if(e)e.textContent='Укажите время прихода и ухода.';return;}"
        "if(!trkPendingDay||!trkPendingMonth){if(e)e.textContent='Не выбран день.';return;}"
        "if(b)b.disabled=true;if(e)e.textContent='';"
        "fetch('/api/tracking/fill?month='+encodeURIComponent(trkPendingMonth)+"
        "'&day='+trkPendingDay+'&in='+encodeURIComponent(iv)+'&out='+encodeURIComponent(ov),"
        "{method:'POST'}).then(function(r){return r.json();}).then(function(d){"
        "if(d&&d.ok){"
        "TRK[trkPendingDay]={in:d.in,out:d.out,hours:d.hours,sum:d.sum};"
        "renderTracking();closeTrkDialog();"
        "trkMsg('Записано: '+d.date+', '+d.hours+' ч., '+d.sum,true);"
        "try{trkStatus();}catch(x){}"
        "}else if(e){e.textContent=(d&&d.error)?d.error:'Не удалось сохранить';}"
        "if(b)b.disabled=false;"
        "}).catch(function(){if(e)e.textContent='Сеть недоступна';if(b)b.disabled=false;});"
        "}"
        "function load(i){"
        "cur=i;showMonths();"
        "var xhr=new XMLHttpRequest();"
        "xhr.open('GET','/api/month?name='+encodeURIComponent(M[i]),true);"
        "xhr.onreadystatechange=function(){"
        "if(xhr.readyState===4){"
        "if(xhr.status===200){try{var d=JSON.parse(xhr.responseText);showTable(d);}catch(e){alert('Ошибка JSON');}}"
        "else{alert('HTTP '+xhr.status);}"
        "}"
        "};"
        "xhr.send();"
        "}"
        "function showMonths(){var c=document.getElementById('months');c.innerHTML='';"
        "M.forEach(function(m,i){var b=document.createElement('button');b.className='b'+(i===cur?' active':'');"
        "b.textContent=m;b.onclick=function(){load(i);};c.appendChild(b);});}"
        "function init(){"
        "applyTheme();"
        "try{trkInit();}catch(e){var el=document.getElementById('trk-st');if(el)el.textContent='Ошибка: '+e.message;}"
        "var xhr=new XMLHttpRequest();"
        "xhr.open('GET','/api/months',true);"
        "xhr.onreadystatechange=function(){"
        "if(xhr.readyState===4){"
        "if(xhr.status===200){"
        "try{var d=JSON.parse(xhr.responseText);M=d.months||[];"
        "if(M.length)load(M.length-1);else alert('Нет данных');"
        "}catch(e){alert('Ошибка JSON');}"
        "}else{alert('Ошибка сети '+xhr.status);}"
        "}"
        "};"
        "xhr.send();"
        "}"
        "function trkMsg(txt,ok){var el=document.getElementById('trk-st');if(el){el.textContent=txt;el.className=ok?'trk-banner ok':'trk-banner err';el.style.display='block';}}"
        "function tgAlert(m){var tg=window.Telegram&&window.Telegram.WebApp;if(tg&&tg.showAlert){try{tg.showAlert(m);}catch(e){}}}"
        "function trkPost(action,cb){"
        "var xhr=new XMLHttpRequest();"
        "xhr.open('POST','/api/tracking/clock?action='+action,true);"
        "xhr.setRequestHeader('Content-Type','application/x-www-form-urlencoded');"
        "xhr.onreadystatechange=function(){"
        "if(xhr.readyState!==4)return;"
        "var r;try{r=JSON.parse(xhr.responseText);}catch(e){r={ok:false,error:'Ошибка JSON'};}"
        "if(r.ok){var msg='Записано! '+(action==='in'?'Приход ':'Уход ')+r.time;"
        "if(r.hours!==undefined&&r.hours!==null)msg+='\\nОтработано: '+r.hours+' ч.';"
        "if(r.sum!==undefined&&r.sum!==null&&r.sum!=='')msg+='\\nСумма: '+r.sum;"
        "trkMsg(msg,true);tgAlert(msg);"
        "try{if(typeof loadTracking==='function')loadTracking(trkSelectedMonth);}catch(e){}"
        "}else{var err=r.error||('HTTP '+xhr.status);trkMsg('Ошибка: '+err,false);tgAlert(err);}"
        "if(cb)cb(r);"
        "};"
        "xhr.onerror=function(){var e='Сеть недоступна';trkMsg('Ошибка: '+e,false);tgAlert(e);if(cb)cb({ok:false,error:e});};"
        "xhr.send('');"
        "}"
        "function trkStatus(){"
        "fetch('/api/tracking/summary').then(function(r){return r.json();}).then(function(dd){"
        "if(!dd||!dd.ok)return;"
        "trkMsg('Отработано за '+(dd.month||'')+': '+dd.total_hours+' ч., сумма '+dd.total_sum,true);"
        "}).catch(function(){trkMsg('Не удалось загрузить статистику',false);});}"
        "function trkMonthSum(){var s=0;for(var k in TRK){var r=TRK[k];if(r&&r.sum){var v=parseFloat(r.sum);if(isFinite(v))s+=v;}}return s;}"
        "function updateMoneyInfo(){"
        "var el=document.getElementById('cal-money');"
        "if(el){var s=trkMonthSum();el.innerHTML=s>0?('Заработано: <b>'+s.toFixed(2)+'</b>'):'';}"
        "var cal=document.querySelector('.cal');if(cal)cal.classList.toggle('show-sum',!!MONEY_VIEW);"
        "var b=document.getElementById('trk-money');if(b)b.classList.toggle('on',!!MONEY_VIEW);"
        "}"
        "function trkMoney(){"
        "MONEY_VIEW=!MONEY_VIEW;"
        "updateMoneyInfo();"
        "if(MONEY_VIEW){var s=trkMonthSum();"
        "trkMsg(s>0?('Заработано за '+(TRK_META.label||trkSelectedMonth||' месяц')+': '+s.toFixed(2)+'. Суммы по дням — в календаре.'):'За этот месяц пока нет отработанных дней',s>0);"
        "}else{try{trkStatus();}catch(x){trkMsg('Заработок по дням скрыт',true);}}"
        "}"
        "function trkInit(){"
        "var b1=document.getElementById('trk-in'),b2=document.getElementById('trk-out'),bm=document.getElementById('trk-money');"
        "var sv=document.getElementById('trk-modal-save'),cn=document.getElementById('trk-modal-cancel');"
        "if(sv)sv.onclick=saveTrkDialog;if(cn)cn.onclick=closeTrkDialog;"
        "var md=document.getElementById('trk-modal');"
        "if(md)md.onclick=function(e){if(e.target===md)closeTrkDialog();};"
        "if(!b1||!b2)return;"
        "b1.onclick=function(){b1.disabled=true;trkMsg('Сохраняем...',true);trkPost('in',function(){b1.disabled=false;trkStatus();});};"
        "b2.onclick=function(){b2.disabled=true;trkMsg('Сохраняем...',true);trkPost('out',function(){b2.disabled=false;trkStatus();});};"
        "if(bm)bm.onclick=trkMoney;"
        "trkStatus();}"

        "</script>"
        "</head><body onload='init()'>"
        "<div class='c'><div id='months' class='m'></div>"
        "<div id='table' class='t h'></div>"
        "<div class='trk'><h3>Учёт рабочего времени</h3>"
        "<div class='row'><button id='trk-in' class='in'>Пришла</button>"
        "<button id='trk-out' class='out'>Ушла</button></div>"
        "<div class='row' style='margin-top:8px'><button id='trk-money' class='mn'>Деньги</button></div>"
        "<div class='st' id='trk-st'></div>"
        "<div class='st'>Тап по дню — внести время (день со сменой, ещё не отмеченный) или посмотреть отработанное. Тап по кабинету — с кем работаете. Кнопка «Деньги» — заработок по дням в календаре. Выходные сб/вс помечены красным, «раб.» — рабочая смена в выходной.</div></div>"
        "<div id='trk-modal' class='trk-modal'><div class='trk-box'>"
        "<h4 id='trk-modal-title'>Отметить время</h4>"
        "<label for='trk-in-time'>Приход</label><input id='trk-in-time' type='time' step='60'>"
        "<label for='trk-out-time'>Уход</label><input id='trk-out-time' type='time' step='60'>"
        "<div id='trk-modal-err' class='trk-err'></div>"
        "<div class='row'><button id='trk-modal-save' class='save'>Сохранить</button>"
        "<button id='trk-modal-cancel' class='cancel'>Отмена</button></div>"
        "</div></div>"
        "</div></body></html>";

    char header[256];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
        sizeof(html_prefix) - 1);
    write(fd, header, hlen);
    write(fd, html_prefix, sizeof(html_prefix) - 1);
}

static void http_reply(int fd, const char *status, const char *body) {
    char header[256];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
        status, strlen(body));
    write(fd, header, hlen);
    write(fd, body, strlen(body));
}

/* Отдаёт JSON: {"months":[...]} */
static void api_months(int fd) {
    MonthEntry mo[MAX_MONTHS];
    memset(mo, 0, sizeof(mo));
    int count = read_months(mo, MAX_MONTHS);

    size_t cap = 64 + count * 64;
    char *json = malloc(cap);
    if (!json) { http_reply(fd, "500 Internal Server Error", "{}"); return; }
    int off = snprintf(json, cap, "{\"months\":[");
    for (int i = 0; i < count; i++) {
        const char *esc = json_escape(mo[i].title);
        int need = snprintf(json + off, cap - off, "%s\"%s\"", i ? "," : "", esc);
        off += need;
        free((void*)esc);
    }
    snprintf(json + off, cap - off, "]}");

    char hdr[256];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
        strlen(json));
    write(fd, hdr, hlen);
    write(fd, json, strlen(json));
    free(json);
}

/* Отдаёт JSON месяца по ?name=... */
static void api_month(int fd, const char *query) {
    const char *p = strstr(query, "name=");
    if (!p) { http_reply(fd, "400 Bad Request", "{\"error\":\"не указано имя\"}"); return; }
    p += 5;
    char name[256];
    url_decode(name, sizeof(name), p);

    MonthEntry mo[MAX_MONTHS];
    memset(mo, 0, sizeof(mo));
    int count = read_months(mo, MAX_MONTHS);
    int idx = -1;
    for (int i = 0; i < count; i++) {
        if (strcmp(mo[i].title, name) == 0) { idx = i; break; }
    }
    if (idx < 0) { http_reply(fd, "404 Not Found", "{\"error\":\"не найдено\"}"); return; }

    MonthEntry *m = &mo[idx];
    int c1 = m->shift1_len, c2 = m->shift2_len;
    int n = c1 > c2 ? c1 : c2; if (n < 1) n = 1;

    /* Отметки Time tracking берём из локального кэша (его обновляет
       background_updater раз в 60 с). Если кэша нет — клиент запросит
       /api/tracking/days и получит данные напрямую из Google. */
    char trk[16384];
    int have_trk = (tracking_days_cached_json(m->title, trk, sizeof(trk)) == 0);

    size_t cap = 256 + (size_t)n * 160 + (have_trk ? strlen(trk) : 4);
    char *json = malloc(cap);
    if (!json) { http_reply(fd, "500 Internal Server Error", "{}"); return; }
    int off = snprintf(json, cap, "{\"t\":");
    const char *et = json_escape(m->title);
    off += snprintf(json + off, cap - off, "\"%s\"", et);
    free((void*)et);
    off += snprintf(json + off, cap - off, ",\"s1\":[");
    for (int i = 0; i < n; i++) {
        const char *v = (i < c1 && m->shift1[i]) ? m->shift1[i] : "";
        const char *ev = json_escape(v);
        off += snprintf(json + off, cap - off, "%s\"%s\"", i ? "," : "", ev);
        free((void*)ev);
    }
    off += snprintf(json + off, cap - off, "],\"s2\":[");
    for (int i = 0; i < n; i++) {
        const char *v = (i < c2 && m->shift2[i]) ? m->shift2[i] : "";
        const char *ev = json_escape(v);
        off += snprintf(json + off, cap - off, "%s\"%s\"", i ? "," : "", ev);
        free((void*)ev);
    }
    off += snprintf(json + off, cap - off, "],\"trk\":%s}",
                    have_trk ? trk : "null");

    char hdr[256];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
        strlen(json));
    write(fd, hdr, hlen);
    write(fd, json, strlen(json));
    free(json);
}

/* Отдаёт JSON коллег по кабинету и дню:
   GET /api/cabinet?month=...&cabinet=...&day=...&shift=7:30-14:40 */
static void api_cabinet(int fd, const char *query) {
    const char *p_month = strstr(query, "month=");
    const char *p_cab   = strstr(query, "cabinet=");
    const char *p_day   = strstr(query, "day=");
    const char *p_shift = strstr(query, "shift=");
    if (!p_month || !p_cab || !p_day) {
        http_reply(fd, "400 Bad Request", "{\"error\":\"не хватает параметров\"}");
        return;
    }
    p_month += 6;
    p_cab   += 8;
    p_day   += 4;
    if (p_shift) p_shift += 6;

    char tmp[512];
    char month[256] = "";
    char cabinet[128] = "";
    char shift[128] = "";
    int day = 0;

    #define EXTRACT_PARAM(src, dst, dstcap) do { \
        const char *amp = strchr(src, '&'); \
        size_t l = amp ? (size_t)(amp - src) : strlen(src); \
        if (l >= sizeof(tmp)) l = sizeof(tmp) - 1; \
        memcpy(tmp, src, l); tmp[l] = '\0'; \
        url_decode(dst, dstcap, tmp); \
    } while(0)

    EXTRACT_PARAM(p_month, month, sizeof(month));
    EXTRACT_PARAM(p_cab, cabinet, sizeof(cabinet));
    if (p_shift) EXTRACT_PARAM(p_shift, shift, sizeof(shift));

    {
        const char *amp = strchr(p_day, '&');
        size_t l = amp ? (size_t)(amp - p_day) : strlen(p_day);
        char ds[16] = "";
        if (l >= sizeof(ds)) l = sizeof(ds) - 1;
        memcpy(ds, p_day, l); ds[l] = '\0';
        day = atoi(ds);
    }
    #undef EXTRACT_PARAM

    fprintf(stderr, "[cabinet] month='%s' cabinet='%s' day=%d shift='%s'\n", month, cabinet, day, shift);

    size_t cap = 65536;
    char *json = malloc(cap);
    if (!json) { http_reply(fd, "500 Internal Server Error", "{}"); return; }

    int off = snprintf(json, cap, "{\"cabinet\":\"");
    char *ec = json_escape(cabinet);
    off += snprintf(json + off, cap - off, "%s", ec ? ec : cabinet);
    free(ec);
    off += snprintf(json + off, cap - off, "\",\"month\":\"");
    char *em = json_escape(month);
    off += snprintf(json + off, cap - off, "%s", em ? em : month);
    free(em);
    off += snprintf(json + off, cap - off, "\",\"day\":%d,\"shift\":\"%s\",\"colleagues\":", day, shift);

    int n = find_cabinet_colleagues_from_index(month, day, cabinet, shift[0] ? shift : NULL, json + off, cap - off - 1);
    off += n;

    snprintf(json + off, cap - off, "}");

    char hdr[256];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
        strlen(json));
    write(fd, hdr, hlen);
    write(fd, json, strlen(json));
    free(json);
}

/* Отправляет web_app кнопку */

/* Отправляет web_app кнопку */
/* Устанавливает кнопку меню чата (кнопка в строке ввода Telegram),
   которая открывает Mini App без промежуточных шагов. */
static void set_chat_menu_button(void) {
    if (!MINIAPP_URL || !*MINIAPP_URL) return;
    char *eu = json_escape(MINIAPP_URL);
    char json[8192];
    char url[512];
    snprintf(json, sizeof(json),
        "{\"menu_button\":{\"type\":\"web_app\",\"text\":\"График\",\"web_app\":{\"url\":\"%s\"}}}", eu);
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/setChatMenuButton", BOT_TOKEN);
    int code = http_post_json(url, json);
    free(eu);
    (void)code;
}

/* Диагностика: пробуем достучаться до Google Sheets и отдаём голову ответа */
static char *diag_google(void) {
    int status = 0;
    char url[1024];
    snprintf(url, sizeof(url),
        "https://sheets.googleapis.com/v4/spreadsheets/%s"
        "?fields=sheets.properties(title,hidden)&key=%s",
        SPREADSHEET_ID ? SPREADSHEET_ID : "", GOOGLE_API_KEY ? GOOGLE_API_KEY : "");
    return http_get(url, &status);
}

static int handle_conn(int client_fd) {
    char buf[BUFFER_SIZE];
    size_t total = 0;
    while (total < BUFFER_SIZE - 1) {
        ssize_t n = read(client_fd, buf + total, BUFFER_SIZE - 1 - total);
        if (n <= 0) break;
        total += n;
        buf[total] = '\0';
        char *hdr = strstr(buf, "\r\n\r\n");
        if (hdr) {
            long clen = 0;
            char *cl = strstr(buf, "Content-Length:");
            if (cl) { clen = strtol(cl + 15, NULL, 10); if (clen < 0) clen = 0; }
            size_t hl = hdr + 4 - buf;
            if (total >= hl + (size_t)clen) break;
        }
    }
    if (total == 0) return -1;

    char method[16] = {0}, path[512] = {0};
    sscanf(buf, "%15s %511s", method, path);

    char *body = NULL;
    char *hdr = strstr(buf, "\r\n\r\n");
    if (hdr) body = hdr + 4;

    if (strcmp(path, "/health") == 0) {
        http_reply(client_fd, "200 OK", "OK");
        return 0;
    }
    if (strcmp(path, "/version") == 0) {
        /* Показывает, какой коммит реально задеплоен (Render прокидывает
           RENDER_GIT_COMMIT / RENDER_GIT_BRANCH в окружение).
           miniapp_url: задан ли MINIAPP_URL — без него не будет кнопки меню
           чата, то есть не будет доступа к графику-таблице из Telegram. */
        const char *sha    = getenv("RENDER_GIT_COMMIT");
        const char *branch = getenv("RENDER_GIT_BRANCH");
        char json[512];
        snprintf(json, sizeof(json),
                 "{\"commit\":\"%s\",\"branch\":\"%s\","
                 "\"track_buttons\":\"Пришла/Ушла\","
                 "\"miniapp_url\":\"%s\",\"menu_button\":\"%s\"}",
                 sha    ? sha    : "неизвестно",
                 branch ? branch : "неизвестно",
                 (MINIAPP_URL && *MINIAPP_URL) ? "задан" : "не задан",
                 (MINIAPP_URL && *MINIAPP_URL) ? "web_app График" : "нет");
        http_reply(client_fd, "200 OK", json);
        return 0;
    }
    if (strcmp(path, "/") == 0) {
        serve_miniapp(client_fd);
        return 0;
    }
    if (strcmp(path, "/api/months") == 0) {
        api_months(client_fd);
        return 0;
    }
    if (strncmp(path, "/api/month", 10) == 0) {
        api_month(client_fd, path);
        return 0;
    }
    if (strncmp(path, "/api/cabinet", 12) == 0) {
        api_cabinet(client_fd, path);
        return 0;
    }
    if (strncmp(path, "/api/tracking/", 14) == 0 ||
        strcmp(path, "/diag/tracking") == 0 ||
        strncmp(path, "/diag/tracking/", 15) == 0) {
        tracking_handle(client_fd, method, path, body);
        return 0;
    }
    if (strcmp(path, "/diag/cabinet") == 0) {
        /* Диагностика индекса кабинетов: количество строк и пример */
        FILE *fp = fopen(CABINET_INDEX_FILE, "r");
        if (!fp) {
            http_reply(client_fd, "503 Service Unavailable",
                       "{\"error\":\"индекс ещё не готов\"}");
            return 0;
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
        http_reply(client_fd, "200 OK", body);
        return 0;
    }
    if (strcmp(path, "/diag") == 0) {
        char body[1024];
        char *resp = diag_google();
        if (resp) {
            snprintf(body, sizeof(body), "google OK. head:\n%.700s", resp);
            free(resp);
        } else {
            snprintf(body, sizeof(body), "google FAILED (curl error - см. лог: status и вывод curl)");
        }
        http_reply(client_fd, "200 OK", body);
        return 0;
    }
    if (strcmp(path, "/webhook") == 0) {
        /* сразу отвечаем 200, чтобы Telegram не ретраил, потом обрабатываем */
        http_reply(client_fd, "200 OK", "OK");
        if (body && *body) process_update(body);
        return 0;
    }
    http_reply(client_fd, "404 Not Found", "Not Found");
    return 0;
}

/* ================================================================
 * Фоновое обновление кэша каждые CACHE_TTL секунд
 * ================================================================ */
static void background_updater(void) {
    MonthEntry mo[MAX_MONTHS];
    for (;;) {
        memset(mo, 0, sizeof(mo));
        FILE *idx = fopen(CABINET_INDEX_FILE, "w");
        int c = build_months(mo, MAX_MONTHS, idx);
        if (idx) { fclose(idx); fprintf(stderr, "[cabinet] index saved\n"); }
        save_months(mo, c);
        fprintf(stderr, "[updater] months cached: %d\n", c);

        /* Тот же цикл обновляет и кэш отметок Time tracking, чтобы Mini App
           не ждал Google при переключении месяца. */
        if (c > 0) {
            const char *labels[MAX_MONTHS];
            int lc = 0;
            for (int i = 0; i < c && i < MAX_MONTHS; i++) labels[lc++] = mo[i].title;
            int tn = tracking_cache_refresh(labels, lc);
            if (tn >= 0) fprintf(stderr, "[updater] tracking cache months: %d\n", tn);
        }
        free_months(mo, c);

        /* Кнопка меню чата (доступ к графику-таблице) ставится периодически:
           если при старте запрос не прошёл, доступ к таблице восстановится. */
        set_chat_menu_button();
        sleep(CACHE_TTL);
    }
}

int main(void) {
    int port = 8080;
    char *port_env = getenv("PORT");
    if (port_env) port = atoi(port_env);

    init_config();

    fprintf(stderr, "[build] cabinet-index-v2 (all rows, name inheritance, shift overlap)\n");
    fprintf(stderr, "[config] SHEET=%s FROM=%s TO=%s SEARCH_NAME='%s'\n",
            SPREADSHEET_ID ? SPREADSHEET_ID : "", FROM_COL ? FROM_COL : "",
            TO_COL ? TO_COL : "", SEARCH_NAME ? SEARCH_NAME : "");

    if (!BOT_TOKEN || !SPREADSHEET_ID || !GOOGLE_API_KEY || !SEARCH_NAME) {
        fprintf(stderr, "[ERROR] Required env vars missing:\n");
        if (!BOT_TOKEN)      fprintf(stderr, "  - BOT_TOKEN\n");
        if (!SPREADSHEET_ID) fprintf(stderr, "  - SPREADSHEET_ID\n");
        if (!GOOGLE_API_KEY) fprintf(stderr, "  - GOOGLE_API_KEY\n");
        if (!SEARCH_NAME)    fprintf(stderr, "  - SEARCH_NAME\n");
        return 1;
    }

    /* Устанавливаем кнопку меню чата, которая открывает Mini App одним кликом */
    set_chat_menu_button();

    /* ВАЖНО: НЕ игнорируем SIGCHLD — на Linux это ломает waitpid() внутри
       pclose() для popen/curl: pclose вернёт -1, и все fetch_url будут
       ошибочно считаться проваленными. Детей рейпим в accept-цикле. */

    pid_t upd = fork();
    if (upd == 0) { background_updater(); _exit(0); }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(port)
    };
    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(server_fd, 10) < 0) { perror("listen"); return 1; }

    printf("[INFO] Bot webhook listening on 0.0.0.0:%d (path /webhook)\n", port);

    for (;;) {
        /* рейпим завершившихся детей-обработчиков webhook, не блокируясь */
        while (waitpid(-1, NULL, WNOHANG) > 0) {}

        struct sockaddr_in ca;
        socklen_t clen = sizeof(ca);
        int client_fd = accept(server_fd, (struct sockaddr *)&ca, &clen);
        if (client_fd < 0) continue;
        pid_t c = fork();
        if (c == 0) { handle_conn(client_fd); close(client_fd); _exit(0); }
        close(client_fd);
    }

    close(server_fd);
    return 0;
}

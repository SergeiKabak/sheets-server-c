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
#include <errno.h>
#include <sys/stat.h>

#include "tracking.h"
#include "http.h"
#include "supabase.h"

#define CACHE_TTL 60
/* Раз в сутки: выгрузка БД -> Google (архив, включая правки дашборда
   в Supabase) и освежение кэша коллег (mcol) */
#define DAY_SYNC_TTL (24 * 3600)
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
           (имя пустое — наследуется от предыдущей строки).
           Те же строки параллельно накапливаются для буфера Supabase (этап 1). */
        if (idx || sb_enabled()) {
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
                    if (idx)
                        fprintf(idx, "%s\t%d\t%s\t%s\t%s\n",
                                sheets[i], d - 1, cabinet, cur_name, shift);
                    if (sb_enabled())
                        sb_row(sheets[i], d - 1, cur_name, shift, cabinet);
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
 * Mini App: статические файлы (web/) + API
 * ================================================================ */
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
/* ==== Этап 2 плана Supabase: чтение графика из SQL с фоллбэком на файлы ==== */

/* Месяц из БД -> массивы s1/s2 в формате строк листа. 0 — успех, -1 — нет
   данных / ошибка (вызывающая сторона уходит в файловый кэш). */
static int sb_read_month(const char *title, const char *employee,
                         int sheet_days, const char *total,
                         char *s1, size_t s1cap, char *s2, size_t s2cap) {
    char *em = url_encode(title);
    char *ee = url_encode(employee);
    char path[1024];
    int rc = -1;
    if (em && ee && *em && *ee) {
        snprintf(path, sizeof(path),
                 "/schedule?month=eq.%s&employee=eq.%s"
                 "&select=day,shift_time,cabinet&order=shift_time,day",
                 em, ee);
        int st = 0;
        char *body = sb_get(path, &st);
        if (body && st >= 200 && st < 300)
            rc = sb_month_arrays(body, employee, sheet_days, total, s1, s1cap, s2, s2cap);
        else
            fprintf(stderr, "[supabase] read month '%s' -> HTTP %d (фоллбэк на файлы)\n",
                    title, st);
        free(body);
    }
    free(em);
    free(ee);
    return rc;
}

/* Коллеги из БД -> JSON-массив. Длина или -1 (тогда — индексный файл). */
static int sb_read_colleagues(const char *month, int day, const char *cabinet,
                              const char *shift, char *out, size_t outcap) {
    if (day < 1 || day > 31) return -1;
    char *em = url_encode(month);
    char *ec = url_encode(cabinet);
    char path[1024];
    int n = -1;
    if (em && ec && *em && *ec) {
        snprintf(path, sizeof(path),
                 "/schedule?month=eq.%s&day=eq.%d&cabinet=eq.%s"
                 "&select=employee,shift_time",
                 em, day, ec);
        int st = 0;
        char *body = sb_get(path, &st);
        if (body && st >= 200 && st < 300)
            n = sb_colleagues_json(body, SEARCH_NAME, shift, shifts_overlap,
                                   out, outcap);
        else
            fprintf(stderr, "[supabase] read colleagues -> HTTP %d (фоллбэк на индекс)\n", st);
        free(body);
    }
    free(em);
    free(ec);
    return n;
}

/* Фоллбэк: массивы из файлового кэша месяцев (логика до этапа 2). */
static int build_month_arrays_json(const MonthEntry *m,
                                   char *s1, size_t s1cap, char *s2, size_t s2cap) {
    int c1 = m->shift1_len, c2 = m->shift2_len;
    int n = c1 > c2 ? c1 : c2;
    if (n < 1) n = 1;
    size_t o1 = 0, o2 = 0;
    for (int i = 0; i < n; i++) {
        const char *v = (i < c1 && m->shift1[i]) ? m->shift1[i] : "";
        char *ev = json_escape(v);
        int w = snprintf(s1 + o1, s1cap - o1, "%s\"%s\"", i ? "," : "[", ev ? ev : v);
        free(ev);
        if (w < 0 || (size_t)w >= s1cap - o1) return -1;
        o1 += (size_t)w;
    }
    for (int i = 0; i < n; i++) {
        const char *v = (i < c2 && m->shift2[i]) ? m->shift2[i] : "";
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

/* ==== Кэш готовых ответов месяцев: ускорение /api/month ====
   Сам запрос раньше ходил в Supabase REST (плюс TLS-handshake на каждый
   fork-процесс) и стоил ~0.7-1 c TTFB. Теперь готовые массивы s1/s2 кладутся
   в /tmp/mresp_<hex>.json: их пишет фоновый цикл (свежесть <= 60 с, как у
   всех кэшей) и лениво сам запрос при промахе. Отметки (trk) в кэш НЕ входят:
   они берутся из своего кэша, который инвалидируется при записи. */
#define MONTH_RESP_TTL 600   /* старше 10 минут — не доверяем, строим заново */

/* Hex первых 48 байт заголовка листа — безопасное имя файла без коллизий */
static int month_hex(const char *title, char *out, size_t cap) {
    if (!title || !*title || cap < 97) return -1;
    static const char *H = "0123456789abcdef";
    int o = 0;
    for (const unsigned char *p = (const unsigned char *)title; *p && o < 96; p++) {
        out[o++] = H[*p >> 4];
        out[o++] = H[*p & 0x0F];
    }
    out[o] = '\0';
    return 0;
}

static int month_resp_path(const char *title, char *out, size_t cap) {
    char hex[112];
    if (month_hex(title, hex, sizeof(hex)) != 0) return -1;
    int n = snprintf(out, cap, "/tmp/mresp_%s.json", hex);
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

/* ==== Кэш коллег месяца (/tmp/mcol_<hex>.json): ускорение /api/cabinet ==== */

/* Собирает строки ВСЕХ сотрудников месяца из Supabase (для ответа коллег)
   и кладёт в файл. PostgREST отдаёт максимум 1000 строк на запрос
   (max-rows) — ходим страницами по стабильному порядку PK. */
static int month_col_refresh(const char *title) {
    char hex[112];
    if (month_hex(title, hex, sizeof(hex)) != 0) return -1;
    char *em = url_encode(title);
    if (!em) return -1;

    const size_t cap = 1024 * 1024;   /* ~1500 строк * ~150 Б + запас */
    char *acc = malloc(cap);
    if (!acc) { free(em); return -1; }
    size_t alen = 0;
    acc[alen++] = '[';
    acc[alen] = '\0';

    int offset = 0, pages = 0, ok = 1;
    while (pages++ < 12) {
        char path[1024];
        snprintf(path, sizeof(path),
                 "/schedule?month=eq.%s&select=day,employee,shift_time,cabinet"
                 "&order=day,employee,shift_time,cabinet&limit=1000&offset=%d",
                 em, offset);
        int st = 0;
        char *body = sb_get(path, &st);
        if (!body || st < 200 || st > 299) { ok = 0; free(body); break; }
        size_t bl = strlen(body);
        int got = 0;
        if (bl >= 2 && body[0] == '[' && body[bl - 1] == ']') {
            cJSON *arr = cJSON_Parse(body);
            if (arr) { got = cJSON_GetArraySize(arr); cJSON_Delete(arr); }
            size_t inner = bl - 2;
            if (inner > 0) {
                if (alen + inner + 2 >= cap) {
                    ok = 0;
                } else {
                    if (alen > 1) acc[alen++] = ',';   /* разделитель страниц */
                    memcpy(acc + alen, body + 1, inner);
                    alen += inner;
                    acc[alen] = '\0';
                }
            }
        } else {
            ok = 0;
        }
        free(body);
        if (!ok || got < 1000) break;
        offset += 1000;
    }
    free(em);
    if (!ok) { free(acc); return -1; }
    acc[alen++] = ']';
    acc[alen] = '\0';

    char mpath[192], tmp[224];
    snprintf(mpath, sizeof(mpath), "/tmp/mcol_%s.json", hex);
    snprintf(tmp, sizeof(tmp), "%s.%d.tmp", mpath, (int)getpid());
    FILE *f = fopen(tmp, "wb");
    if (!f) { remove(tmp); free(acc); return -1; }
    int bad = fwrite(acc, 1, alen, f) != alen;
    fclose(f);
    free(acc);
    if (bad || rename(tmp, mpath) != 0) { remove(tmp); return -1; }
    return 0;
}

/* Читает полный массив строк месяца; -1 если нет/устарел/повреждён */
static int month_col_read(const char *title, char *out, size_t cap) {
    char hex[112], path[192];
    if (month_hex(title, hex, sizeof(hex)) != 0) return -1;
    snprintf(path, sizeof(path), "/tmp/mcol_%s.json", hex);
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    if (time(NULL) - st.st_mtime > MONTH_RESP_TTL) return -1;
    if (st.st_size < 2 || (size_t)st.st_size >= cap) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t got = fread(out, 1, (size_t)st.st_size, f);
    fclose(f);
    out[got] = '\0';
    if (got < 2 || out[0] != '[' || out[got - 1] != ']') return -1;
    return 0;
}

/* Из полного массива — только строки нужного дня и кабинета,
   в формате, который ждёт sb_colleagues_json. */
static int month_col_filter(const char *all, int day, const char *cabinet,
                            char *out, size_t outcap) {
    if (!all || !cabinet || !out || outcap < 4) return -1;
    out[0] = '\0';
    cJSON *arr = cJSON_Parse(all);
    if (!arr || !cJSON_IsArray(arr)) { cJSON_Delete(arr); return -1; }
    int off = snprintf(out, outcap, "[");
    int n = 0;
    cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr) {
        cJSON *jd = cJSON_GetObjectItemCaseSensitive(it, "day");
        cJSON *jc = cJSON_GetObjectItemCaseSensitive(it, "cabinet");
        if (!cJSON_IsNumber(jd) || !cJSON_IsString(jc) || !jc->valuestring) continue;
        if ((int)jd->valuedouble != day || strcmp(jc->valuestring, cabinet) != 0) continue;
        char *printed = cJSON_PrintUnformatted(it);
        if (!printed) continue;
        int w = snprintf(out + off, outcap - (size_t)off, "%s%s", n ? "," : "", printed);
        free(printed);
        if (w < 0 || (size_t)w >= outcap - (size_t)off) {
            cJSON_Delete(arr);
            out[0] = '\0';
            return -1;
        }
        off += w;
        n++;
    }
    cJSON_Delete(arr);
    if ((size_t)off + 2 >= outcap) { out[0] = '\0'; return -1; }
    snprintf(out + off, outcap - off, "]");
    return 0;
}

/* Пишет {"t":...,"s1":[...],"s2":[...]} атомарно (tmp + rename) */
static int month_resp_write(const char *title, const char *s1, const char *s2) {
    char path[256], tmp[300];
    if (month_resp_path(title, path, sizeof(path)) != 0) return -1;
    snprintf(tmp, sizeof(tmp), "%s.%d.tmp", path, (int)getpid());
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    const char *et = json_escape(title);
    int n = fprintf(f, "{\"t\":\"%s\",\"s1\":%s,\"s2\":%s}", et ? et : "", s1, s2);
    free((void*)et);
    fclose(f);
    if (n < 0 || rename(tmp, path) != 0) { remove(tmp); return -1; }
    return 0;
}

/* Читает кэш ответа (без trk) в out; -1 если нет/устарел/повреждён */
static int month_resp_read(const char *title, char *out, size_t cap) {
    char path[256];
    if (month_resp_path(title, path, sizeof(path)) != 0) return -1;
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    if (time(NULL) - st.st_mtime > MONTH_RESP_TTL) return -1;
    if (st.st_size < 16 || (size_t)st.st_size >= cap) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t got = fread(out, 1, (size_t)st.st_size, f);
    fclose(f);
    out[got] = '\0';
    if (got < 16 || out[got - 1] != '}') return -1;
    return 0;
}

/* Массивы s1/s2 месяца: Supabase (этап 2), при недоступности — файловый кэш.
   0 — успех (структура листа: n = max(длин строк), дней = n-3,
   счётчик «Всего смен» — из листа). */
static int month_s1s2(const MonthEntry *m,
                      char *s1, size_t s1cap, char *s2, size_t s2cap) {
    int c1 = m->shift1_len, c2 = m->shift2_len;
    int n = c1 > c2 ? c1 : c2;
    if (n < 1) n = 1;
    char total1[64] = "";
    if (m->shift1 && c1 > 0 && m->shift1[c1 - 1])
        snprintf(total1, sizeof(total1), "%s", m->shift1[c1 - 1]);

    if (sb_enabled() && SEARCH_NAME && *SEARCH_NAME &&
        sb_read_month(m->title, SEARCH_NAME, n - 3, total1, s1, s1cap, s2, s2cap) == 0)
        return 0;
    return build_month_arrays_json(m, s1, s1cap, s2, s2cap);
}

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
    if (idx < 0) { free_months(mo, count); http_reply(fd, "404 Not Found", "{\"error\":\"не найдено\"}"); return; }

    MonthEntry *m = &mo[idx];

    /* Отметки Time tracking — из своего кэша (обновляется фоновым циклом,
       инвалидируется при записи — поэтому их НЕ кэшируем в mresp) */
    char trk[16384];
    int have_trk = (tracking_days_cached_json(m->title, trk, sizeof(trk)) == 0);

    /* Ответ графика: кэш готового ответа -> мгновенно, без HTTP к Supabase;
       при промахе строим (Supabase -> файловый кэш) и кладём в кэш */
    char cached[16384];
    char s1txt[16384] = "";
    char s2txt[16384] = "";
    int use_cache = (month_resp_read(m->title, cached, sizeof(cached)) == 0);
    if (!use_cache) {
        if (month_s1s2(m, s1txt, sizeof(s1txt), s2txt, sizeof(s2txt)) != 0) {
            free_months(mo, count);
            http_reply(fd, "500 Internal Server Error", "{\"error\":\"нет данных месяца\"}");
            return;
        }
        month_resp_write(m->title, s1txt, s2txt);
        fprintf(stderr, "[cache] month '%s' -> mresp (supabase/file)\n", m->title);
    }

    size_t base = use_cache ? strlen(cached) : (strlen(s1txt) + strlen(s2txt) + 64);
    size_t cap = 256 + base + (have_trk ? strlen(trk) : 4);
    char *json = malloc(cap);
    if (!json) { free_months(mo, count); http_reply(fd, "500 Internal Server Error", "{}"); return; }
    if (use_cache) {
        /* cached = {"t":..,"s1":[..],"s2":[..]} — срезаем последнюю скобку
           и вставляем trk из своего кэша */
        snprintf(json, cap, "%.*s,\"trk\":%s}", (int)(strlen(cached) - 1), cached,
                 have_trk ? trk : "null");
    } else {
        const char *et = json_escape(m->title);
        snprintf(json, cap, "{\"t\":\"%s\",\"s1\":%s,\"s2\":%s,\"trk\":%s}",
                 et ? et : "", s1txt, s2txt, have_trk ? trk : "null");
        free((void*)et);
    }

    char hdr[256];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
        strlen(json));
    write(fd, hdr, hlen);
    write(fd, json, strlen(json));
    free(json);
    free_months(mo, count);
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

    /* Ускорение тапа по кабинету: коллеги из кэша месяца (mcol) — без HTTP;
       при промахе/устаревании — Supabase, затем индексный файл (фоллбэки
       сохранены полностью) */
    int n = -1;
    int col_cache_hit = 0;
    {
        char *all = malloc(512 * 1024);
        if (all) {
            if (month_col_read(month, all, 512 * 1024) == 0) {
                char sub[65536];
                if (month_col_filter(all, day, cabinet, sub, sizeof(sub)) == 0) {
                    n = sb_colleagues_json(sub, SEARCH_NAME,
                                           shift[0] ? shift : NULL, shifts_overlap,
                                           json + off, cap - off - 1);
                    if (n >= 0) col_cache_hit = 1;
                }
            }
            free(all);
        }
    }
    if (n < 0 && sb_enabled())
        n = sb_read_colleagues(month, day, cabinet, shift[0] ? shift : NULL,
                               json + off, cap - off - 1);
    if (n < 0)
        n = find_cabinet_colleagues_from_index(month, day, cabinet,
                                               shift[0] ? shift : NULL,
                                               json + off, cap - off - 1);
    off += n;

    snprintf(json + off, cap - off, "}");

    char hdr[256];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
        strlen(json));
    write(fd, hdr, hlen);
    write(fd, json, strlen(json));
    free(json);

    /* Ответ уже отправлен — клиент не ждёт. Без кэша коллег (первый тап
       или устарел >TTL) освежаем полный массив строк месяца под следующие
       тапы: трафик пропорционален реальной активности. */
    if (col_cache_hit == 0 && n >= 0)
        month_col_refresh(month);
}

/* ---- Раздача статики Mini App (web/) ---- */
#define WEB_CACHE_HTML "no-cache"
#define WEB_CACHE      "public, max-age=300"

static const char *web_dir(void) {
    const char *d = getenv("WEB_DIR");
    return (d && *d) ? d : "web";
}

/* Только [A-Za-z0-9._-/], без ".." и ведущего "/": защита от path traversal. */
static int web_path_safe(const char *p) {
    if (!p || !*p || *p == '/') return 0;
    for (const char *q = p; *q; q++) {
        unsigned char c = (unsigned char)*q;
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                 c == '-' || c == '/';
        if (!ok) return 0;
        if (q[0] == '.' && q[1] == '.') return 0;
    }
    return 1;
}

static const char *web_mime(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return "application/octet-stream";
    if (!strcmp(dot, ".html")) return "text/html; charset=utf-8";
    if (!strcmp(dot, ".css"))  return "text/css; charset=utf-8";
    if (!strcmp(dot, ".js"))   return "text/javascript; charset=utf-8";
    if (!strcmp(dot, ".json")) return "application/json; charset=utf-8";
    if (!strcmp(dot, ".svg"))  return "image/svg+xml";
    if (!strcmp(dot, ".ico"))  return "image/x-icon";
    if (!strcmp(dot, ".png"))  return "image/png";
    return "application/octet-stream";
}

/* write() может записать меньше — досылаем целиком. */
static void write_all(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) { if (errno == EINTR) continue; return; }
        p += n;
        len -= (size_t)n;
    }
}

/* Отдаёт файл из web/: Content-Type + ETag + Cache-Control + Connection: close.
   HTML — без кэша (при деплое сразу новый), css/js — с кэшем на 5 минут.
   req — исходный запрос: при совпадении If-None-Match с ETag отвечаем
   304 без тела (повторные открытия не переносят байты файлов). */
static void serve_file(int fd, const char *rel, const char *cache, const char *req) {
    if (!web_path_safe(rel)) { http_reply(fd, "400 Bad Request", "Bad Request"); return; }

    char full[1024];
    int n = snprintf(full, sizeof(full), "%s/%s", web_dir(), rel);
    if (n <= 0 || n >= (int)sizeof(full)) {
        http_reply(fd, "414 URI Too Long", "Too Long");
        return;
    }

    FILE *fp = fopen(full, "rb");
    if (!fp) { http_reply(fd, "404 Not Found", "Not Found"); return; }
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); http_reply(fd, "500 Internal Server Error", "read"); return; }
    long sz = ftell(fp);
    if (sz < 0 || sz > 4L * 1024 * 1024) {
        fclose(fp);
        http_reply(fd, "500 Internal Server Error", "read");
        return;
    }
    rewind(fp);

    char etag[64];
    struct stat st;
    if (fstat(fileno(fp), &st) == 0)
        snprintf(etag, sizeof(etag), "\"%llx-%llx\"",
                 (unsigned long long)sz, (unsigned long long)st.st_mtime);
    else
        snprintf(etag, sizeof(etag), "\"%llx\"", (unsigned long long)sz);

    if (req) {
        const char *inm = strstr(req, "If-None-Match:");
        if (inm) {
            const char *eol = strstr(inm, "\r\n");
            size_t vlen = eol ? (size_t)(eol - inm) : strlen(inm);
            if (vlen < 512) {
                char val[512];
                memcpy(val, inm, vlen); val[vlen] = '\0';
                if (strstr(val, etag)) {
                    char h304[256];
                    int n304 = snprintf(h304, sizeof(h304),
                        "HTTP/1.1 304 Not Modified\r\nETag: %s\r\n"
                        "Cache-Control: %s\r\nConnection: close\r\n\r\n",
                        etag, cache);
                    fclose(fp);
                    write_all(fd, h304, (size_t)n304);
                    return;
                }
            }
        }
    }

    char *data = malloc((size_t)sz + 1);
    if (!data) { fclose(fp); http_reply(fd, "500 Internal Server Error", "oom"); return; }
    size_t got = fread(data, 1, (size_t)sz, fp);
    fclose(fp);

    char hdr[512];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "ETag: %s\r\n"
        "Cache-Control: %s\r\n"
        "Connection: close\r\n\r\n",
        web_mime(rel), got, etag, cache);
    write_all(fd, hdr, (size_t)hlen);
    write_all(fd, data, got);
    free(data);
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
        serve_file(client_fd, "index.html", WEB_CACHE_HTML, buf);
        return 0;
    }
    /* Статика Mini App: /app.css, файлы из js/, /favicon.ico —
       отдётся из web/ с Cache-Control (браузер не перетаскивает их каждый раз). */
    {
        const char *q = strchr(path, '?');
        size_t plen = q ? (size_t)(q - path) : strlen(path);
        if (plen == 8 && strncmp(path, "/app.css", 8) == 0) {
            serve_file(client_fd, "app.css", WEB_CACHE, buf);
            return 0;
        }
        if (plen > 4 && plen < 240 && strncmp(path, "/js/", 4) == 0) {
            char rel[256];
            memcpy(rel, path + 1, plen - 1);
            rel[plen - 1] = '\0';
            serve_file(client_fd, rel, WEB_CACHE, buf);
            return 0;
        }
        if (plen == 12 && strncmp(path, "/favicon.ico", 12) == 0) {
            serve_file(client_fd, "favicon.ico", WEB_CACHE, buf);
            return 0;
        }
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
    if (strcmp(path, "/diag/supabase") == 0) {
        /* Диагностика буфера Supabase: точные счётчики (Content-Range),
           пульс sync_log, состояние time_entries. Только чтение. */
        MonthEntry mo[MAX_MONTHS];
        memset(mo, 0, sizeof(mo));
        int c = read_months(mo, MAX_MONTHS);
        const char *titles[MAX_MONTHS];
        int nt = 0;
        for (int i = 0; i < c && nt < MAX_MONTHS; i++) titles[nt++] = mo[i].title;
        char body[8192];
        if (sb_diag(titles, nt, body, sizeof(body)) != 0 && !strstr(body, "\"ok\"")) {
            snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"диагностика недоступна\"}");
        }
        free_months(mo, c);
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

/* ==== Этап 1 плана Supabase: строки самого сотрудника в буфер ==== */
/* Строка листа: [0]=имя, [1]=смена, [2..]=кабинеты по дням,
   позиция n-1 (n = max длины строк) — ячейка-счётчик «Всего смен».
   Ровно по этой логике Mini App считает дни: days = n - 3.
   limit — индекс счётчика (не включительно). */
static void sb_add_row_cells(const char *month, char **cells, int len, int limit) {
    if (!cells || len < 3) return;
    const char *name = (cells[0] && cells[0][0]) ? cells[0] : SEARCH_NAME;
    if (!name || !name[0]) return;
    const char *shift = cells[1] ? cells[1] : "";
    int last = len < limit ? len : limit;
    for (int d = 2; d < last; d++)
        if (cells[d] && cells[d][0])
            sb_row(month, d - 1, name, shift, cells[d]);
}

static void sb_add_month_rows(const MonthEntry *m) {
    /* счётчик — по позиции max(длин строк): хвостовые пустые ячейки
       одной строки не должны «съедать» последний день другой */
    int n = m->shift1_len > m->shift2_len ? m->shift1_len : m->shift2_len;
    sb_add_row_cells(m->title, m->shift1, m->shift1_len, n - 1);
    sb_add_row_cells(m->title, m->shift2, m->shift2_len, n - 1);
}

/* ================================================================
 * Фоновое обновление кэша каждые CACHE_TTL секунд
 * ================================================================ */
static void background_updater(void) {
    MonthEntry mo[MAX_MONTHS];
    for (;;) {
        memset(mo, 0, sizeof(mo));
        sb_begin_cycle();   /* updated_at текущего цикла — ДО сбора строк */
        FILE *idx = fopen(CABINET_INDEX_FILE, "w");
        int c = build_months(mo, MAX_MONTHS, idx);
        if (idx) { fclose(idx); fprintf(stderr, "[cabinet] index saved\n"); }
        save_months(mo, c);
        fprintf(stderr, "[updater] months cached: %d\n", c);

        /* Буфер графика в Supabase: upsert строк месяца + уборка устаревших */
        if (sb_enabled()) {
            const char *titles[MAX_MONTHS];
            int nt = 0;
            for (int i = 0; i < c && nt < MAX_MONTHS; i++) {
                titles[nt++] = mo[i].title;
                sb_add_month_rows(&mo[i]);
            }
            int errs = 0;
            int rows = sb_flush(titles, nt, &errs);
            fprintf(stderr, "[sync] supabase schedule: rows=%d errors=%d\n", rows, errs);
            static int cycle = 0;
            cycle++;
            /* пульс в sync_log каждые ~10 мин + любая ошибка отдельно */
            if (errs || cycle % 10 == 0)
                sb_log("schedule", "", rows, errs == 0,
                       errs ? "HTTP error при синхронизации графика" : "");
        }

        /* Тот же цикл обновляет и кэш отметок Time tracking, чтобы Mini App
           не ждал Google при переключении месяца. */
        if (c > 0) {
            const char *labels[MAX_MONTHS];
            int lc = 0;
            for (int i = 0; i < c && i < MAX_MONTHS; i++) labels[lc++] = mo[i].title;
            int tn = tracking_cache_refresh(labels, lc);
            if (tn >= 0) fprintf(stderr, "[updater] tracking cache months: %d\n", tn);
        }

        /* Выгрузка БД -> Google (Google = архив для людей):
           - каждый цикл: месяцы, помеченные dirty_put() после записей в БД;
           - раз в сутки: ВСЕ месяцы (подхватывает ручные правки дашборда
             в Supabase) + освежение кэша коллег (mcol).
           Ночной restore time_entries из Google УДАЛЁН: БД — первоисточник,
           зеркалирование из Google затёрло бы правки, сделанные в БД. */
        if (sb_enabled() && c > 0) {
            tracking_push_pending();

            static time_t last_daily = 0;
            if (time(NULL) - last_daily >= DAY_SYNC_TTL) {
                last_daily = time(NULL);

                int col_n = 0;
                for (int i2 = 0; i2 < c; i2++)
                    if (month_col_refresh(mo[i2].title) == 0) col_n++;
                fprintf(stderr, "[updater] month colleagues cached: %d\n", col_n);

                const char *titles[MAX_MONTHS];
                int nt = 0;
                for (int i2 = 0; i2 < c && nt < MAX_MONTHS; i2++)
                    titles[nt++] = mo[i2].title;
                int pushed = tracking_push_all(titles, nt);
                fprintf(stderr, "[sync] google push months: %d\n", pushed);
            }
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
    sb_init();               /* буфер графика в Supabase (этап 1) */

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

#include "domain/timesheet.h"

#include "core/date_time.h"
#include "core/json_text.h"
#include "core/num.h"
#include "core/strbuf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *const SHEET_HEADERS[SHEET_COLS] = {
    "Дата", "Время прихода", "Время ухода",
    "Время в часах", "Ставка в час", "Сумма за день"
};

void sheet_rows_init(SheetRows *rows) {
    if (!rows) return;
    rows->row = NULL;
    rows->count = 0;
    rows->cap = 0;
}

void sheet_rows_free(SheetRows *rows) {
    if (!rows) return;
    for (int i = 0; i < rows->count; i++)
        for (int c = 0; c < SHEET_COLS; c++)
            free(rows->row[i].cell[c]);
    free(rows->row);
    sheet_rows_init(rows);
}

static SheetRow *sheet_rows_add(SheetRows *rows) {
    if (rows->count >= rows->cap) {
        rows->cap = rows->cap ? rows->cap * 2 : 32;
        rows->row = realloc(rows->row, sizeof(SheetRow) * (size_t)rows->cap);
    }
    SheetRow *r = &rows->row[rows->count++];
    for (int c = 0; c < SHEET_COLS; c++) r->cell[c] = strdup("");
    return r;
}

void sheet_rows_insert(SheetRows *rows, int idx) {
    if (!rows) return;
    if (idx < 0) idx = 0;
    if (idx > rows->count) idx = rows->count;
    if (rows->count >= rows->cap) {
        rows->cap = rows->cap ? rows->cap * 2 : 32;
        rows->row = realloc(rows->row, sizeof(SheetRow) * (size_t)rows->cap);
    }
    /* сдвигаем вниз: владение указателями переходит к новым позициям */
    for (int i = rows->count; i > idx; i--)
        rows->row[i] = rows->row[i - 1];
    rows->count++;
    for (int c = 0; c < SHEET_COLS; c++)
        rows->row[idx].cell[c] = strdup("");
}

void sheet_row_set(SheetRow *row, int col, const char *value) {
    if (!row || col < 0 || col >= SHEET_COLS) return;
    free(row->cell[col]);
    row->cell[col] = strdup(value ? value : "");
}

const char *sheet_row_get(const SheetRow *row, int col) {
    if (!row || col < 0 || col >= SHEET_COLS) return "";
    return row->cell[col] ? row->cell[col] : "";
}

int sheet_row_is_empty(const SheetRow *row) {
    if (!row) return 1;
    for (int c = 0; c < SHEET_COLS; c++) {
        const char *v = row->cell[c];
        if (v && *v) return 0;
    }
    return 1;
}

int sheet_row_is_date(const SheetRow *row) {
    if (!row) return 0;
    const char *a = sheet_row_get(row, 0);
    if (!*a) return 0;
    int d = 0, m = 0, y = 0;
    if (sscanf(a, "%d.%d.%d", &d, &m, &y) == 3) return 1;
    if (sscanf(a, "%d/%d/%d", &d, &m, &y) == 3) return 1;
    return 0;
}

/* ---------- разбор ответа Google Sheets ---------- */

int sheet_rows_parse_values_json(SheetRows *rows, const char *json) {
    if (!rows || !json) return 0;
    const char *v = strstr(json, "\"values\"");
    if (!v) return 0;
    const char *arr = strchr(v, '[');
    if (!arr) return 0;
    arr++;

    while (*arr && *arr != ']') {
        while (*arr == ' ' || *arr == '\t' || *arr == '\n' || *arr == '\r' || *arr == ',') arr++;
        if (*arr != '[') break;
        arr++;

        SheetRow *r = sheet_rows_add(rows);
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
                    /* базовая распаковка escape-последовательностей */
                    char *dec = malloc(len + 1);
                    size_t o = 0;
                    if (dec) {
                        for (size_t i = 0; i < len; i++) {
                            if (raw[i] == '\\' && i + 1 < len) {
                                i++;
                                switch (raw[i]) {
                                    case 'n': dec[o++] = '\n'; break;
                                    case 't': dec[o++] = '\t'; break;
                                    case 'r': dec[o++] = '\r'; break;
                                    default:  dec[o++] = raw[i];
                                }
                            } else dec[o++] = raw[i];
                        }
                        dec[o] = '\0';
                    }
                    free(raw);
                    if (dec) {
                        if (cell < SHEET_COLS) {
                            free(r->cell[cell]);
                            r->cell[cell] = dec;
                        } else {
                            free(dec);
                        }
                        cell++;
                    }
                }
                if (*arr == '"') arr++;
            } else {
                /* число/булево без кавычек */
                const char *start = arr;
                while (*arr && *arr != ',' && *arr != ']') arr++;
                size_t len = (size_t)(arr - start);
                while (len && (start[len - 1] == ' ' || start[len - 1] == '\n')) len--;
                char *num = malloc(len + 1);
                if (num) {
                    memcpy(num, start, len);
                    num[len] = '\0';
                    if (cell < SHEET_COLS) {
                        free(r->cell[cell]);
                        r->cell[cell] = num;
                    } else {
                        free(num);
                    }
                    cell++;
                }
            }
        }
        while (*arr && *arr != ']') arr++;
        if (*arr == ']') arr++;
    }
    return rows->count;
}

/* ---------- поиск по листу ---------- */

int sheet_find_employee(const SheetRows *rows, const char *name) {
    if (!rows || !name || !*name) return -1;
    for (int i = 0; i < rows->count; i++) {
        const char *a = sheet_row_get(&rows->row[i], 0);
        if (!*a) continue;
        /* строка-заголовок блока: A = ФИО, остальные пустые */
        int rest_empty = 1;
        for (int c = 1; c < SHEET_COLS; c++) {
            if (*sheet_row_get(&rows->row[i], c)) {
                rest_empty = 0;
                break;
            }
        }
        if (!rest_empty) continue;
        /* ФИО не должно быть датой */
        int d = 0, m = 0, y = 0;
        if (sscanf(a, "%d.%d.%d", &d, &m, &y) == 3) continue;
        if (sscanf(a, "%d/%d/%d", &d, &m, &y) == 3) continue;
        if (strcmp(a, name) == 0) return i;
        if (strstr(a, name)) return i;
    }
    return -1;
}

int sheet_find_date_row(const SheetRows *rows, int block_idx, const char *date) {
    if (!rows || block_idx < 0) return -1;
    char norm[64];
    date_normalize(date, norm, sizeof(norm));
    for (int i = block_idx + 1; i < rows->count; i++) {
        if (sheet_row_is_empty(&rows->row[i])) break;   /* конец блока */
        const char *a = sheet_row_get(&rows->row[i], 0);
        if (!*a) continue;
        char n2[64];
        date_normalize(a, n2, sizeof(n2));
        if (strcmp(n2, norm) == 0) return i;
    }
    return -1;
}

int sheet_find_block_end(const SheetRows *rows, int block_idx) {
    if (!rows) return block_idx;
    int last = block_idx;
    for (int i = block_idx + 1; i < rows->count; i++) {
        if (sheet_row_is_empty(&rows->row[i])) break;
        last = i;
    }
    return last;
}

/* ---------- сортировка дат внутри блоков ---------- */

void sheet_rows_sort_dates(SheetRows *rows) {
    if (!rows) return;
    int i = 0;
    while (i < rows->count) {
        if (!sheet_row_is_date(&rows->row[i]) && *sheet_row_get(&rows->row[i], 0)) {
            int block_start = i;
            int block_end = block_start + 1;
            while (block_end < rows->count && !sheet_row_is_empty(&rows->row[block_end]))
                block_end++;
            int first_date = block_start + 1;
            int last_date = block_end - 1;
            if (first_date < last_date) {
                for (int a = first_date; a <= last_date; a++) {
                    for (int b = first_date; b < last_date; b++) {
                        if (!sheet_row_is_date(&rows->row[b]) ||
                            !sheet_row_is_date(&rows->row[b + 1]))
                            continue;
                        if (date_compare(sheet_row_get(&rows->row[b], 0),
                                         sheet_row_get(&rows->row[b + 1], 0)) > 0) {
                            SheetRow tmp = rows->row[b];
                            rows->row[b] = rows->row[b + 1];
                            rows->row[b + 1] = tmp;
                        }
                    }
                }
            }
            i = block_end;
        } else {
            i++;
        }
    }
}

/* ---------- сборка JSON ---------- */

char *sheet_row_to_values_json(const SheetRow *row) {
    if (!row) return NULL;
    StrBuf sb;
    sbuf_init(&sb);
    sbuf_append(&sb, "[");
    for (int c = 0; c < SHEET_COLS; c++) {
        if (c) sbuf_append(&sb, ",");
        sbuf_append_json_string(&sb, sheet_row_get(row, c));
    }
    sbuf_append(&sb, "]");
    return sbuf_take(&sb);
}

char *sheet_rows_to_body_json(const SheetRows *rows) {
    if (!rows) return NULL;
    StrBuf sb;
    sbuf_init(&sb);
    sbuf_append(&sb, "{\"values\":[");
    for (int i = 0; i < rows->count; i++) {
        if (i) sbuf_append(&sb, ",");
        sbuf_append(&sb, "[");
        for (int c = 0; c < SHEET_COLS; c++) {
            if (c) sbuf_append(&sb, ",");
            sbuf_append_json_string(&sb, sheet_row_get(&rows->row[i], c));
        }
        sbuf_append(&sb, "]");
    }
    sbuf_append(&sb, "]}");
    return sbuf_take(&sb);
}

/* ---------- итоги по блоку ---------- */

void sheet_block_totals(const SheetRows *rows, int block_idx,
                        double *hours, double *sum, int *days) {
    if (hours) *hours = 0;
    if (sum) *sum = 0;
    if (days) *days = 0;
    if (!rows || block_idx < 0) return;

    for (int i = block_idx + 1; i < rows->count; i++) {
        if (sheet_row_is_empty(&rows->row[i])) break;
        if (!*sheet_row_get(&rows->row[i], 0)) continue;
        double h = num_parse(sheet_row_get(&rows->row[i], 3));
        double s = num_parse(sheet_row_get(&rows->row[i], 5));
        if (h > 0) {
            if (hours) *hours += h;
            if (days) (*days)++;
        }
        if (s > 0 && sum) *sum += s;
    }
}

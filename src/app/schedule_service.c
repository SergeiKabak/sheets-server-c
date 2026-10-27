#include "app/schedule_service.h"

#include "app/month_cache.h"
#include "vendor/cJSON.h"
#include "core/shift.h"
#include "core/url_codec.h"
#include "domain/schedule.h"
#include "infra/http_client.h"
#include "infra/supabase.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- список видимых листов ---------- */

int schedule_visible_sheets(const AppConfig *cfg,
                            char sheets[][SCHEDULE_MAX_NAME], int max_sheets) {
    if (!cfg || !sheets || max_sheets <= 0) return 0;

    char url[1024];
    snprintf(url, sizeof(url),
        "https://sheets.googleapis.com/v4/spreadsheets/%s"
        "?fields=sheets.properties(sheetId,title,hidden)&key=%s",
        cfg->spreadsheet_id ? cfg->spreadsheet_id : "",
        cfg->google_api_key ? cfg->google_api_key : "");

    int http_status = 0;
    char *response = http_get(url, &http_status);
    if (!response) {
        fprintf(stderr, "[diag] google metadata fetch failed (curl/network)\n");
        return 0;
    }

    int count = 0;
    cJSON *root = cJSON_Parse(response);
    if (root) {
        cJSON *list = cJSON_GetObjectItemCaseSensitive(root, "sheets");
        cJSON *it = NULL;
        cJSON_ArrayForEach(it, list) {
            if (count >= max_sheets) break;
            cJSON *props = cJSON_GetObjectItemCaseSensitive(it, "properties");
            if (!props) continue;
            cJSON *hidden = cJSON_GetObjectItemCaseSensitive(props, "hidden");
            if (cJSON_IsTrue(hidden)) continue;      /* скрытые листы не показываем */
            cJSON *title = cJSON_GetObjectItemCaseSensitive(props, "title");
            if (!cJSON_IsString(title) || !title->valuestring) continue;
            snprintf(sheets[count], SCHEDULE_MAX_NAME, "%.*s",
                     SCHEDULE_MAX_NAME - 1, title->valuestring);
            count++;
        }
        cJSON_Delete(root);
    }

    if (count == 0)
        fprintf(stderr, "[diag] google metadata head: %.300s\n", response);
    free(response);
    return count;
}

/* ---------- разбор values API ---------- */

int schedule_parse_rows(const char *json, SheetCells **rows_out, int **row_lens_out) {
    if (!json || !rows_out || !row_lens_out) return 0;
    *rows_out = NULL;
    *row_lens_out = NULL;

    cJSON *root = cJSON_Parse(json);
    if (!root) return 0;
    cJSON *values = cJSON_GetObjectItemCaseSensitive(root, "values");
    if (!cJSON_IsArray(values)) { cJSON_Delete(root); return 0; }

    int count = 0, cap = 32;
    SheetCells *rows = malloc(sizeof(SheetCells) * (size_t)cap);
    int *lens = malloc(sizeof(int) * (size_t)cap);
    if (!rows || !lens) {
        free(rows); free(lens); cJSON_Delete(root);
        return 0;
    }

    cJSON *row_item = NULL;
    cJSON_ArrayForEach(row_item, values) {
        if (!cJSON_IsArray(row_item)) continue;
        if (count >= cap) {
            cap *= 2;
            rows = realloc(rows, sizeof(SheetCells) * (size_t)cap);
            lens = realloc(lens, sizeof(int) * (size_t)cap);
            if (!rows || !lens) break;
        }
        int len = 0;
        char **cells = NULL;
        cJSON *cell = NULL;
        cJSON_ArrayForEach(cell, row_item) {
            if (!cJSON_IsString(cell) || !cell->valuestring) continue;
            cells = realloc(cells, sizeof(char *) * (size_t)(len + 1));
            if (!cells) break;
            cells[len++] = strdup(cell->valuestring);
        }
        if (!cells) continue;
        rows[count] = cells;
        lens[count] = len;
        count++;
    }
    cJSON_Delete(root);

    if (count == 0) {
        free(rows);
        free(lens);
        return 0;
    }
    *rows_out = rows;
    *row_lens_out = lens;
    return count;
}

void schedule_free_rows(SheetCells *rows, int *lens, int count) {
    if (rows) {
        for (int i = 0; i < count; i++) {
            if (!rows[i]) continue;
            for (int j = 0; j < lens[i]; j++) free(rows[i][j]);
            free(rows[i]);
        }
        free(rows);
    }
    free(lens);
}

/* ---------- сбор месяцев из Google Sheets ---------- */

int schedule_build_months(const AppConfig *cfg, MonthSchedule *out, int max,
                           FILE *idx) {
    if (!cfg || !out || max <= 0) return 0;

    char sheets[SCHEDULE_MAX_MONTHS][SCHEDULE_MAX_NAME];
    int sheet_count = schedule_visible_sheets(cfg, sheets, SCHEDULE_MAX_MONTHS);
    fprintf(stderr, "[updater] visible sheets=%d SEARCH_NAME='%s'\n",
            sheet_count, cfg->search_name ? cfg->search_name : "");
    int count = 0;

    for (int i = 0; i < sheet_count && count < max; i++) {
        char range[SCHEDULE_MAX_NAME + 128];
        /* %.*s ограничивает имя листа — иначе gcc видит «плоский» массив
           sheets[50][128] строкой до 6399 байт и warns -Wformat-truncation.
           FROM_COL/TO_COL из getenv() — могут быть NULL. */
        snprintf(range, sizeof(range), "%.*s!%s:%s",
                 SCHEDULE_MAX_NAME - 1, sheets[i],
                 cfg->from_col ? cfg->from_col : "", cfg->to_col ? cfg->to_col : "");

        char *encoded_range = url_encode(range);
        char url[2048];
        snprintf(url, sizeof(url),
            "https://sheets.googleapis.com/v4/spreadsheets/%s/values/%s?key=%s",
            cfg->spreadsheet_id ? cfg->spreadsheet_id : "",
            encoded_range ? encoded_range : "", cfg->google_api_key ? cfg->google_api_key : "");
        free(encoded_range);

        int http_status = 0;
        char *response = http_get(url, &http_status);
        if (!response) continue;

        SheetCells *rows = NULL;
        int *row_lens = NULL;
        int row_count = schedule_parse_rows(response, &rows, &row_lens);        if (row_count == 0) { free(response); continue; }

        int found_idx = -1;
        for (int r = 0; r < row_count; r++) {
            char **row = rows[r];
            int rlen = row_lens[r];
            if (rlen > 0 && row[0] && cfg->search_name &&
                strcmp(row[0], cfg->search_name) == 0) {
                found_idx = r;
                break;
            }
        }
        if (found_idx < 0) {
            schedule_free_rows(rows, row_lens, row_count);
            free(response);
            continue;
        }

        /* Индекс кабинетов по ВСЕМ строкам листа.
           У человека две строки: утренняя (в графе C имя) и вечерняя
           (имя пустое — наследуется от предыдущей строки).
           Те же строки параллельно накапливаются для буфера Supabase. */
        if (idx || sb_enabled()) {
            const char *cur_name = "";
            for (int r = 0; r < row_count; r++) {
                char **row = rows[r];
                int rlen = row_lens[r];
                if (rlen < 3) continue;
                const char *nm = row[0] ? row[0] : "";
                if (*nm) cur_name = nm;
                if (!*cur_name) continue;
                if (cfg->search_name && strcmp(cur_name, cfg->search_name) == 0) continue;
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

        MonthSchedule *m = &out[count];
        memset(m, 0, sizeof(*m));
        snprintf(m->title, sizeof(m->title), "%.*s", SCHEDULE_MAX_NAME - 1, sheets[i]);

        m->shift1 = month_schedule_copy_row(rows[found_idx], row_lens[found_idx]);
        m->shift1_len = row_lens[found_idx];
        if (found_idx + 1 < row_count) {
            m->shift2 = month_schedule_copy_row(rows[found_idx + 1], row_lens[found_idx + 1]);
            m->shift2_len = row_lens[found_idx + 1];
        }

        schedule_free_rows(rows, row_lens, row_count);
        free(response);
        count++;
    }
    return count;
}

/* ---------- коллеги из файлового индекса ---------- */

int schedule_colleagues_from_index(const char *month, int day, const char *cabinet,
                                   const char *shift, char *out, size_t outcap) {
    if (!out || outcap < 4) return -1;
    FILE *fp = fopen(cabinet_index_path(), "r");
    if (!fp) {
        snprintf(out, outcap, "[]");
        return 2;
    }

    ColleagueList list;
    colleague_list_init(&list);

    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        /* строка: месяц \t день \t кабинет \t имя \t смена */
        char m[128] = "", c[128] = "", name[256] = "", s[128] = "";
        int d = 0;
        char *p = line;

        char *t = strchr(p, '\t');
        if (!t) continue;
        size_t l = (size_t)(t - p);
        if (l >= sizeof(m)) l = sizeof(m) - 1;
        memcpy(m, p, l); m[l] = '\0';
        p = t + 1;

        t = strchr(p, '\t');
        if (!t) continue;
        l = (size_t)(t - p);
        if (l >= 16) l = 15;
        char ds[16] = "";
        memcpy(ds, p, l); ds[l] = '\0';
        d = atoi(ds);
        p = t + 1;

        t = strchr(p, '\t');
        if (!t) continue;
        l = (size_t)(t - p);
        if (l >= sizeof(c)) l = sizeof(c) - 1;
        memcpy(c, p, l); c[l] = '\0';
        p = t + 1;

        t = strchr(p, '\t');
        if (!t) continue;
        l = (size_t)(t - p);
        if (l >= sizeof(name)) l = sizeof(name) - 1;
        memcpy(name, p, l); name[l] = '\0';
        p = t + 1;

        l = strlen(p);
        while (l && (p[l - 1] == '\n' || p[l - 1] == '\r')) p[--l] = '\0';
        if (l >= sizeof(s)) l = sizeof(s) - 1;
        memcpy(s, p, l); s[l] = '\0';

        if (strcmp(m, month ? month : "") != 0) continue;
        if (d != day) continue;
        if (strcmp(c, cabinet ? cabinet : "") != 0) continue;
        /* фильтр по смене: коллега должен работать в пересекающемся интервале */
        if (shift && *shift && !shift_overlap(s, shift)) continue;

        colleague_list_add(&list, name, s);
    }
    fclose(fp);

    int n = colleague_list_render_json(&list, out, outcap);
    return n;
}

/* ---------- чтение из БД ---------- */

int schedule_read_month_from_db(const char *title, const char *employee,
                                 int sheet_days, const char *total,
                                 char *s1, size_t s1cap, char *s2, size_t s2cap) {
    if (!title || !employee) return -1;
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
            rc = schedule_month_arrays(body, employee, sheet_days, total, s1, s1cap, s2, s2cap);
        else
            fprintf(stderr, "[supabase] read month '%s' -> HTTP %d (фоллбэк на файлы)\n",
                    title, st);
        free(body);
    }
    free(em);
    free(ee);
    return rc;
}

int schedule_read_colleagues_from_db(const char *month, int day, const char *cabinet,
                                     const char *shift, const char *self,
                                     char *out, size_t outcap) {
    if (day < 1 || day > 31) return -1;
    char *em = url_encode(month);
    char *ec = url_encode(cabinet);
    char path[1024];
    int rc = -1;
    if (em && ec && *em && *ec) {
        snprintf(path, sizeof(path),
                 "/schedule?month=eq.%s&day=eq.%d&cabinet=eq.%s"
                 "&select=employee,shift_time",
                 em, day, ec);
        int st = 0;
        char *body = sb_get(path, &st);
        if (body && st >= 200 && st < 300) {
            ColleagueList list;
            if (colleagues_from_rows_json(body, self, shift, shift_overlap, &list) == 0)
                rc = colleague_list_render_json(&list, out, outcap);
        } else {
            fprintf(stderr, "[supabase] read colleagues -> HTTP %d (фоллбэк на индекс)\n", st);
        }
        free(body);
    }
    free(em);
    free(ec);
    return rc;
}

/* ---------- обновление кэша строк месяца ---------- */

int schedule_refresh_colleagues_cache(const char *title) {
    if (!title || !*title) return -1;
    char *em = url_encode(title);
    if (!em) return -1;

    /* PostgREST отдаёт максимум 1000 строк на запрос (max-rows) —
       ходим страницами по стабильному порядку PK. */
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

    int rc = month_colleagues_cache_write(title, acc);
    free(acc);
    return rc;
}

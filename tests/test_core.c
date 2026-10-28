/* Юнит-тесты чистого ядра (слой core + domain).
 *
 * Здесь нет ни сети, ни файлов, ни глобального состояния — только чистые
 * функции, поэтому тест работает мгновенно и всегда одинаково.
 * Запуск: make test-core */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/date_time.h"
#include "core/json_text.h"
#include "core/num.h"
#include "core/shift.h"
#include "core/strbuf.h"
#include "core/url_codec.h"
#include "domain/schedule.h"
#include "domain/time_entry.h"
#include "domain/timesheet.h"

static int fails = 0;
static void check(int cond, const char *what) {
    if (cond) {
        printf("OK   %s\n", what);
    } else {
        printf("FAIL %s\n", what);
        fails++;
    }
}

static void test_json_text(void) {
    char *e = json_escape("a\"b\\c\nd\te");
    check(e && !strcmp(e, "a\\\"b\\\\c\\nd\\te"), "json_escape: кавычки, слэш, переводы строк");
    free(e);

    e = json_escape("Привет");
    check(e && !strcmp(e, "Привет"), "json_escape: UTF-8 не трогаем");
    free(e);

    char buf[64];
    check(json_write_string(buf, sizeof(buf), "x\"y") == 0 &&
          !strcmp(buf, "\"x\\\"y\""), "json_write_string: строка с кавычками");

    check(json_write_string(buf, 4, "abcdef") == -1, "json_write_string: переполнение");
}

static void test_url_codec(void) {
    char *e = url_encode("Октябрь 2026");
    check(e && strstr(e, "%") && !strstr(e, " "), "url_encode: пробелы кодируются");
    free(e);

    char buf[64];
    url_decode_into(buf, sizeof(buf), "%D0%9E%D0%BA%D1%82%D1%8F%D0%B1%D1%80%D1%8C");
    check(!strcmp(buf, "Октябрь"), "url_decode: UTF-8 раскодируется");

    url_decode_into(buf, sizeof(buf), "a+b%20c");
    check(!strcmp(buf, "a b c"), "url_decode: '+' и %20 -> пробел");

    char *p = url_query_param("/api/month?name=%D0%9E%D0%BA%D1%82%D1%8F%D0%B1%D1%80%D1%8C&day=5",
                              "name");
    check(p && !strcmp(p, "Октябрь"), "url_query_param: значение по имени");
    free(p);

    p = url_query_param("/api/tracking/fill?month=10.2026&day=5&min=bad", "in");
    check(!p, "url_query_param: 'in' не находится внутри 'min'");

    p = url_query_param("/api/cabinet?month=10.2026&cabinet=K2.1&day=5", "cabinet");
    check(p && !strcmp(p, "K2.1"), "url_query_param: кабинет");
    free(p);
}

static void test_date_time(void) {
    char buf[64];
    month_label("10.2026", buf, sizeof(buf));
    check(!strcmp(buf, "Октябрь 2026"), "month_label: 10.2026 -> Октябрь 2026");

    month_label("ерунда", buf, sizeof(buf));
    check(!strcmp(buf, "ерунда"), "month_label: неизвестное значение не ломается");

    check(month_numeric_from_label("Октябрь 2026", 2024, buf, sizeof(buf)) == 0 &&
          !strcmp(buf, "10.2026"), "month_numeric_from_label: с годом");
    check(month_numeric_from_label("Октябрь", 2024, buf, sizeof(buf)) == 0 &&
          !strcmp(buf, "10.2024"), "month_numeric_from_label: год по умолчанию");
    check(month_numeric_from_label("10.2026", 2024, buf, sizeof(buf)) == 0 &&
          !strcmp(buf, "10.2026"), "month_numeric_from_label: уже цифры");

    int mon = 0, year = 0;
    check(month_parse("10.2026", &mon, &year) == 0 && mon == 10 && year == 2026,
          "month_parse: разбор месяца");
    check(month_parse("13.2026", &mon, &year) == -1, "month_parse: неверный месяц");

    check(days_in_month(2, 2024) == 29, "days_in_month: високосный год");
    check(days_in_month(2, 2025) == 28, "days_in_month: обычный год");
    check(days_in_month(1, 2025) == 31, "days_in_month: январь");

    date_normalize("5.1.2026", buf, sizeof(buf));
    check(!strcmp(buf, "05.01.2026"), "date_normalize: точечный формат");
    date_normalize("2026-01-05", buf, sizeof(buf));
    check(!strcmp(buf, "05.01.2026"), "date_normalize: ISO-формат");

    int d = 0, m = 0, y = 0;
    check(date_parse("05.01.2026", &d, &m, &y) == 0 && d == 5 && m == 1 && y == 2026,
          "date_parse: разбор даты");
    check(date_compare("01.02.2026", "01.03.2026") < 0, "date_compare: порядок дат");
    check(date_compare("05.01.2026", "05.01.2026") == 0, "date_compare: равные даты");

    time_normalize("8:00", buf, sizeof(buf));
    check(!strcmp(buf, "08:00"), "time_normalize: 8:00 -> 08:00");
    check(time_parse("07:30") == 450, "time_parse: минуты от полуночи");
    check(time_parse("25:00") == -1, "time_parse: неверный час");
    check(time_parse("abc") == -1, "time_parse: мусор");
}

static void test_shift(void) {
    int s = 0, e = 0;
    check(shift_parse("7:30-14:40", &s, &e) == 0 && s == 450 && e == 880,
          "shift_parse: интервал");
    check(shift_parse("15:10–21:00", &s, &e) == 0 && s == 910,
          "shift_parse: тире «–»");
    check(shift_parse("15:10—21:00", &s, &e) == 0 && s == 910,
          "shift_parse: тире «—»");
    check(shift_parse("мусор", &s, &e) == -1, "shift_parse: нераспознанное");

    check(shift_overlap("7:30-14:40", "8:00-14:30") == 1, "shift_overlap: пересечение");
    check(shift_overlap("7:30-14:40", "15:10-21:00") == 0, "shift_overlap: без пересечения");
    check(shift_overlap("7:30-14:40", "7:30-14:40") == 1, "shift_overlap: совпадение");

    check(shift_start_minutes("7:30-14:40") == 450, "shift_start_minutes: начало смены");
    check(shift_start_minutes("мусор") == SHIFT_START_UNKNOWN,
          "shift_start_minutes: нераспознанное");
}

static void test_num(void) {
    check(num_parse("7,17") == 7.17, "num_parse: запятая как точка");
    check(num_parse("7.17") == 7.17, "num_parse: точка");
    check(num_parse(NULL) == 0.0, "num_parse: NULL");
    check(num_is_valid("7.17") == 1, "num_is_valid: число");
    check(num_is_valid("-1") == 1, "num_is_valid: отрицательное");
    check(num_is_valid("") == 0, "num_is_valid: пусто");
    check(num_is_valid("abc") == 0, "num_is_valid: мусор");

    char buf[32];
    num_format2(7.1666, buf, sizeof(buf));
    check(!strcmp(buf, "7.17"), "num_format2: два знака");
}

static void test_strbuf(void) {
    StrBuf sb;
    sbuf_init(&sb);
    sbuf_append(&sb, "a");
    sbuf_appendf(&sb, "%d", 42);
    sbuf_append_json_string(&sb, "x\"y");
    char *out = sbuf_take(&sb);
    check(out && !strcmp(out, "a42\"x\\\"y\""), "strbuf: склейка и экранирование");
    free(out);

    sbuf_init(&sb);
    for (int i = 0; i < 1000; i++) sbuf_appendf(&sb, "%d,", i);
    out = sbuf_take(&sb);
    check(out && strlen(out) > 1000, "strbuf: рост буфера");
    free(out);
}

static void test_timesheet(void) {
    const char *json =
        "{\"values\":["
        "[\"Иванов Иван\",\"\",\"\",\"\",\"\",\"\"],"
        "[\"05.10.2026\",\"07:30\",\"14:40\",\"7.17\",\"500\",\"3585.00\"],"
        "[\"06.10.2026\",\"08:00\",\"\",\"\",\"500\",\"\"],"
        "[\"\",\"\",\"\",\"\",\"\",\"\"]"
        "]}";

    SheetRows rows;
    sheet_rows_init(&rows);
    int n = sheet_rows_parse_values_json(&rows, json);
    check(n == 4, "timesheet: разобрано строк");

    int block = sheet_find_employee(&rows, "Иванов Иван");
    check(block == 0, "timesheet: найден блок сотрудника");
    check(sheet_find_employee(&rows, "Петров") == -1, "timesheet: чужого сотрудника нет");

    int row = sheet_find_date_row(&rows, block, "05.10.2026");
    check(row == 1, "timesheet: найдена строка даты");
    check(sheet_find_date_row(&rows, block, "07.10.2026") == -1,
          "timesheet: отсутствующей даты нет");
    check(sheet_find_block_end(&rows, block) == 2, "timesheet: конец блока");

    check(!strcmp(sheet_row_get(&rows.row[1], 1), "07:30"), "timesheet: ячейка B");
    check(!strcmp(sheet_row_get(&rows.row[1], 5), "3585.00"), "timesheet: ячейка F");

    double hours = 0, sum = 0;
    int days = 0;
    sheet_block_totals(&rows, block, &hours, &sum, &days);
    check(hours == 7.17 && days == 1 && sum == 3585.0, "timesheet: итоги по блоку");

    char *body = sheet_rows_to_body_json(&rows);
    check(body && strstr(body, "{\"values\":[[\"Иванов Иван\""),
          "timesheet: тело запроса values.update");
    check(body && strstr(body, "\"05.10.2026\",\"07:30\",\"14:40\""),
          "timesheet: значения строки");
    free(body);
    sheet_rows_free(&rows);

    /* сортировка дат внутри блока */
    SheetRows s2;
    sheet_rows_init(&s2);
    const char *two_rows = "{\"values\":[[\"X\",\"\",\"\",\"\",\"\",\"\"],[\"06.10.2026\",\"\",\"\",\"\",\"\",\"\"],[\"05.10.2026\",\"\",\"\",\"\",\"\",\"\"]]}";
    sheet_rows_parse_values_json(&s2, two_rows);
    sheet_rows_sort_dates(&s2);
    check(!strcmp(sheet_row_get(&s2.row[1], 0), "05.10.2026"),
          "timesheet: даты отсортированы по возрастанию");
    sheet_rows_free(&s2);
}

static void test_time_entry(void) {
    check(time_entry_hours("07:30", "14:40") == 7.1666666666 ||
          (time_entry_hours("07:30", "14:40") > 7.16 &&
           time_entry_hours("07:30", "14:40") < 7.17),
          "time_entry_hours: разница времени");
    check(time_entry_hours("23:00", "01:00") == 2.0, "time_entry_hours: через полночь");
    check(time_entry_hours("bad", "14:40") == 0.0, "time_entry_hours: мусор");

    char sum_buf[32];
    num_format2(time_entry_sum("5.02", 14.2), sum_buf, sizeof(sum_buf));
    check(!strcmp(sum_buf, "71.28"),
          "time_entry_sum: сумма считается от округлённых часов");

    char days[256];
    check(time_entry_days_json("[{\"day\":5,\"t_in\":\"07:30\",\"t_out\":\"14:40\","
                               "\"hours\":7.17,\"amount\":3585.0}]", days, sizeof(days)) == 0,
          "time_entry_days_json: разбор");
    check(!strcmp(days, "{\"5\":{\"in\":\"07:30\",\"out\":\"14:40\","
                        "\"hours\":\"7.17\",\"sum\":\"3585.00\"}}"),
          "time_entry_days_json: формат дней");
    check(time_entry_days_json("[]", days, sizeof(days)) == 0 && !strcmp(days, "{}"),
          "time_entry_days_json: пусто");
    check(time_entry_days_json("[broken", days, sizeof(days)) == -1,
          "time_entry_days_json: битый JSON");
}

static void test_schedule_domain(void) {
    /* месяц-лист: копирование строк и освобождение */
    char *row[3];
    row[0] = strdup("Иванов");
    row[1] = strdup("7:30-14:40");
    row[2] = strdup("K1");
    char **copy = month_schedule_copy_row(row, 3);
    check(copy && !strcmp(copy[2], "K1") && copy[0] != row[0],
          "schedule: копирование строки листа");
    for (int i = 0; i < 3; i++) { free(copy[i]); free(row[i]); }
    free(copy);

    /* список коллег: дедуп по паре имя+смена */
    ColleagueList list;
    colleague_list_init(&list);
    check(colleague_list_add(&list, "А", "7:30-14:40") == 0, "colleagues: добавлен");
    check(colleague_list_add(&list, "А", "7:30-14:40") == 1, "colleagues: дубль отброшен");
    check(colleague_list_add(&list, "А", "15:10-21:00") == 0, "colleagues: другая смена");
    check(list.count == 2, "colleagues: две записи");

    char json[256];
    check(colleague_list_render_json(&list, json, sizeof(json)) > 0, "colleagues: печать");
    check(!strcmp(json, "[{\"name\":\"А\",\"shift\":\"7:30-14:40\"},"
                        "{\"name\":\"А\",\"shift\":\"15:10-21:00\"}]"),
          "colleagues: формат JSON");

    /* месяц-заглушка по структуре листа (month_meta): без самого листа
       календарь должен знать ширину строки и счётчик «Всего смен» */
    MonthSchedule stub;
    check(month_schedule_init_stub(&stub, "Октябрь 2026", 34, "41") == 0,
          "stub: месяц из структуры листа");
    check(stub.shift1_len == 34 && stub.shift2_len == 34, "stub: ширина строк 34");
    check(month_schedule_width(&stub) == 34, "stub: ширина месяца 34");
    check(!strcmp(month_schedule_cell(&stub, 0, 33), "41"), "stub: счётчик в конце");
    check(!strcmp(month_schedule_cell(&stub, 0, 0), ""), "stub: дни пустые");
    check(!strcmp(month_schedule_cell(&stub, 1, 33), ""), "stub: в вечерней строке пусто");
    check(!strcmp(stub.title, "Октябрь 2026"), "stub: заголовок месяца");
    month_schedule_free(&stub);

    check(month_schedule_init_stub(&stub, "X", 3, "1") == -1,
          "stub: слишком узкая строка отклонена");
    check(month_schedule_init_stub(&stub, "", 34, "1") == -1,
          "stub: пустой заголовок отклонён");
}

int main(void) {
    test_json_text();
    test_url_codec();
    test_date_time();
    test_shift();
    test_num();
    test_strbuf();
    test_timesheet();
    test_time_entry();
    test_schedule_domain();

    printf(fails ? "\nFAILED: %d\n" : "\nALL TESTS PASSED\n", fails);
    return fails ? 1 : 0;
}

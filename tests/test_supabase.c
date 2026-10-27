/* Офлайн-тест буфера Supabase (без сети): сбор строк, дедуп, экранирование,
   отбрасывание некорректных дней. Запуск: make test-supabase */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "supabase.h"


/* подсчёт запятых (фикстуры не содержат запятых внутри значений) */
static int ncommas(const char *s) {
    int c = 0;
    for (; *s; s++) if (*s == ',') c++;
    return c;
}

/* тест-фильтр смен: пропускает всё, кроме вечерней (проверка предиката) */
static int t_no_evening(const char *row_shift, const char *query) {
    (void)query;
    return strcmp(row_shift, "15:10-21:00") != 0;
}

int main(void) {
    setenv("SUPABASE_URL", "https://example-project.supabase.co/", 1);
    setenv("SUPABASE_SECRET_KEY", "test-secret-key", 1);
    unsetenv("SUPABASE_SYNC");

    sb_init();
    assert(sb_enabled());

    sb_begin_cycle();

    sb_row("10.2026", 3, "Гринь Юлия Геннадьевна", "15:10-21:00", "K2.1");
    /* дубль PK — не должен попасть в батч (PostgREST отдал бы 409) */
    sb_row("10.2026", 3, "Гринь Юлия Геннадьевна", "15:10-21:00", "K2.1");
    /* экранирование кавычек и спецсимволов */
    sb_row("10.2026", 4, "Иванов \"Иван\" И.\nПетров", "7:30-14:40", "Ц");
    /* некорректные строки отбрасываются */
    sb_row("10.2026", 0, "X", "Y", "Z");    /* день 0 */
    sb_row("10.2026", 32, "X", "Y", "Z");   /* день 32 */
    /* фильтр валидности: без смены — служебная строка, не пишем */
    sb_row("10.2026", 7, "Асситентов на смене", "", "5");
    sb_row("10.2026", 7, "Кабинет 1.1 (1-я смена)", "", "30");
    sb_row("", 5, "X", "Y", "Z");           /* нет месяца */
    sb_row("10.2026", 5, "", "Y", "Z");     /* нет сотрудника */
    sb_row("10.2026", 5, "X", "Y", "");     /* нет кабинета */

    int n = -1;
    const char *json = sb_pending("10.2026", &n);
    assert(json && n == 2);

    /* структура строки */
    assert(strstr(json, "\"month\":\"10.2026\""));
    assert(strstr(json, "\"day\":3"));
    assert(strstr(json, "\"employee\":\"Гринь Юлия Геннадьевна\""));
    assert(strstr(json, "\"shift_time\":\"15:10-21:00\""));
    assert(strstr(json, "\"cabinet\":\"K2.1\""));
    /* updated_at берётся из цикла (sb_begin_cycle) */
    assert(strstr(json, "\"updated_at\":\""));
    /* экранирование: \" и \n внутри JSON */
    assert(strstr(json, "Иванов \\\"Иван\\\""));
    assert(strstr(json, "И.\\nПетров"));
    /* массив начинается с '{', разделитель — ',', дубля день|смена нет */
    assert(json[0] == '{');
    assert(strstr(json, ",{\"month\"") != NULL);
    assert(strstr(json, "\"day\":4") != NULL);

    /* месяц без данных */
    assert(sb_pending("11.2026", &n) == NULL && n == 0);

    /* flush при недоступной базе: ошибки должны учтываться, строк не теряться.
       URL на 127.0.0.1:1 — соединение отклоняется мгновенно, без интернета. */
    setenv("SUPABASE_URL", "https://127.0.0.1:1", 1);
    sb_init();
    assert(sb_enabled());
    int errs = -1;
    int rows = sb_flush(NULL, 0, &errs);
    assert(rows == 0 && errs >= 1);   /* upsert не прошёл -> ошибки посчитаны */
    assert(sb_pending("10.2026", &n) == NULL || n == 0);  /* буфер очищен после flush */

    printf("OK   supabase buffer: дедуп, экранирование, учёт ошибок flush\n");

    /* ---------- Этап 2: разбор месяца из строк PostgREST ---------- */
    const char *mrows =
        "[{\"day\":1,\"shift_time\":\"15:10-21:00\",\"cabinet\":\"K2.2\"},"
        "{\"day\":1,\"shift_time\":\"7:30-14:40\",\"cabinet\":\"K2.1\"},"
        "{\"day\":2,\"shift_time\":\"7:30-14:40\",\"cabinet\":\"Ц\"},"
        "{\"day\":3,\"shift_time\":\"7:30-14:40\",\"cabinet\":\"K2.1\"}]";
    char a1[4096], a2[4096];
    assert(sb_month_arrays(mrows, "Бинкевич Татьяна Эдуардовна", 0, NULL,
                           a1, sizeof a1, a2, sizeof a2) == 0);
    /* утренняя смена = s1, хотя «15:10…» лексикографически меньше */
    assert(!strcmp(a1, "[\"Бинкевич Татьяна Эдуардовна\",\"7:30-14:40\","
                       "\"K2.1\",\"Ц\",\"K2.1\",\"3\"]"));
    assert(!strcmp(a2, "[\"\",\"15:10-21:00\",\"K2.2\",\"\",\"\",\"1\"]"));

    /* одна смена -> s2 пустой массив той же длины (как без строки листа) */
    const char *rows1 = "[{\"day\":2,\"shift_time\":\"9:00-18:00\",\"cabinet\":\"K7\"}]";
    assert(sb_month_arrays(rows1, "X", 0, NULL, a1, sizeof a1, a2, sizeof a2) == 0);
    assert(!strcmp(a1, "[\"X\",\"9:00-18:00\",\"\",\"K7\",\"1\"]"));
    assert(!strcmp(a2, "[\"\",\"\",\"\",\"\",\"\"]"));

    /* размер месяца из заголовка: счётчик должен стоять после дня 31,
       иначе календарь покажет его как кабинет в ячейке последнего дня */
    assert(sb_month_arrays(rows1, "X", 31, NULL, a1, sizeof a1, a2, sizeof a2) == 0);
    assert(ncommas(a1) == 33);              /* 34 элемента: имя+смена+31 день+счётчик */
    assert(strstr(a1, ",\"1\"]") != NULL); /* счётчик в конце массива, не в ячейке дня */
    assert(strstr(a1, "[\"X\",\"9:00-18:00\"") == a1);  /* имя и смена в начале */

    /* листовой счётчик «Всего смен»: берём значение Google Sheets, в s2 пусто */
    assert(sb_month_arrays(mrows, "Бинкевич Татьяна Эдуардовна", 3, "34",
                           a1, sizeof a1, a2, sizeof a2) == 0);
    assert(strstr(a1, ",\"34\"]") != NULL);
    assert(strstr(a2, ",\"\"]") != NULL);
    assert(sb_month_arrays(rows1, "X", 31, "34", a1, sizeof a1, a2, sizeof a2) == 0);
    assert(strstr(a1, ",\"34\"]") != NULL);
    assert(ncommas(a2) == 33);

    /* нет данных / битый JSON -> -1 (фоллбэк на файлы) */
    assert(sb_month_arrays("[]", "X", 0, NULL, a1, sizeof a1, a2, sizeof a2) == -1);
    assert(sb_month_arrays("{nope", "X", 0, NULL, a1, sizeof a1, a2, sizeof a2) == -1);

    /* ---------- Этап 2: коллеги ---------- */
    const char *crows =
        "[{\"employee\":\"Бинкевич Татьяна Эдуардовна\",\"shift_time\":\"7:30-14:40\"},"
        "{\"employee\":\"Яновская Юлия Алексеевна\",\"shift_time\":\"8:00-14:30\"},"
        "{\"employee\":\"Яновская Юлия Алексеевна\",\"shift_time\":\"8:00-14:30\"},"
        "{\"employee\":\"Гринь Юлия Геннадьевна\",\"shift_time\":\"15:10-21:00\"}]";
    char cj[4096];
    assert(sb_colleagues_json(crows, "Бинкевич Татьяна Эдуардовна",
                              NULL, NULL, cj, sizeof cj) > 0);
    assert(!strcmp(cj, "[{\"name\":\"Яновская Юлия Алексеевна\",\"shift\":\"8:00-14:30\"},"
                       "{\"name\":\"Гринь Юлия Геннадьевна\",\"shift\":\"15:10-21:00\"}]"));
    /* с фильтром по смене: сам сотрудник исключён, вечерняя отсеяна, дубль убран */
    assert(sb_colleagues_json(crows, "Бинкевич Татьяна Эдуардовна",
                              "7:30-14:40", t_no_evening, cj, sizeof cj) > 0);
    assert(!strcmp(cj, "[{\"name\":\"Яновская Юлия Алексеевна\",\"shift\":\"8:00-14:30\"}]"));
    assert(sb_colleagues_json("[broken", NULL, NULL, NULL, cj, sizeof cj) == -1);

    /* ---------- Этап 3: time_entries ---------- */
    const char *trows =
        "[{\"day\":5,\"t_in\":\"07:30\",\"t_out\":\"14:40\",\"hours\":7.17,\"amount\":3585.00},"
        "{\"day\":6,\"t_in\":\"08:00\",\"t_out\":\"\",\"hours\":null,\"amount\":null},"
        "{\"day\":9,\"t_in\":\"10:00\",\"t_out\":\"11:30\",\"hours\":1.5,\"amount\":700.0},"
        "{\"day\":40,\"t_in\":\"10:00\",\"t_out\":\"11:00\",\"hours\":1.0,\"amount\":1.0}]";
    char dj[4096];
    assert(sb_time_days_json(trows, dj, sizeof dj) == 0);
    /* формат в точности как tracking_days_build: 4 поля, числа -> "%.2f" */
    assert(!strcmp(dj,
        "{\"5\":{\"in\":\"07:30\",\"out\":\"14:40\",\"hours\":\"7.17\",\"sum\":\"3585.00\"},"
        "\"6\":{\"in\":\"08:00\",\"out\":\"\",\"hours\":\"\",\"sum\":\"\"},"
        "\"9\":{\"in\":\"10:00\",\"out\":\"11:30\",\"hours\":\"1.50\",\"sum\":\"700.00\"}}"));
    assert(sb_time_days_json("[]", dj, sizeof dj) == 0 && !strcmp(dj, "{}"));
    assert(sb_time_days_json("[broken", dj, sizeof dj) == -1);
    assert(sb_time_days_json(NULL, dj, sizeof dj) == -1);

    /* запись: выключена/некорректный день -> 0 без сетевых вызовов;
       на недоступной базе -> ошибка (0), но не падение */
    assert(sb_time_upsert("10.2026", 0, "07:30", "14:40", "7.17", "3585.00") == 0);
    assert(sb_time_upsert("", 5, "07:30", "14:40", NULL, NULL) == 0);
    assert(sb_time_upsert("10.2026", 5, "07:30", "14:40", "7.17", "3585.00") == 0);
    assert(sb_time_upsert("10.2026", 6, "08:00", "", NULL, NULL) == 0);

    /* ---------- Этап 4: восстановление БД из ответа дней Google ---------- */
    const char *gdays =
        "{\"ok\":true,\"month\":\"10.2026\",\"label\":\"Октябрь 2026\","
        "\"today\":\"27.10.2026\",\"days\":{"
        "\"5\":{\"in\":\"07:30\",\"out\":\"14:40\",\"hours\":\"7.17\",\"sum\":\"3585.00\"},"
        "\"6\":{\"in\":\"08:00\",\"out\":\"\",\"hours\":\"\",\"sum\":\"\"}}}";
    int er = -1;
    /* пустой месяц -> 0, без сетевых вызовов */
    assert(sb_time_restore("{\"ok\":true,\"month\":\"10.2026\",\"days\":{}}", &er) == 0 && er == 0);
    /* битый JSON / нет поля month -> ошибка */
    assert(sb_time_restore("[broken", &er) == -1 && er == 1);
    assert(sb_time_restore("{\"days\":{\"5\":{}}}", &er) == -1 && er == 1);
    /* полный ответ: батч собран (2 дня), база недоступна -> -1, ошибка учтена */
    er = 0;
    assert(sb_time_restore(gdays, &er) == -1 && er == 1);
    assert(sb_time_restore(NULL, &er) == -1);

    printf("OK   этап 2: разбор месяца и коллег\n");
    printf("OK   этап 3: формат time_entries (паритет с Google), ошибка записи без падения\n");
    printf("OK   этап 4: восстановление time_entries (пусто/битый/недоступная база)\n");
    printf("ALL TESTS PASSED\n");
    return 0;
}

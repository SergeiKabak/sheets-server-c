/* Офлайн-тест буфера Supabase (без сети): сбор строк, дедуп, экранирование,
   отбрасывание некорректных дней. Запуск: make test-supabase
 *
 * Проверяются и транспорт (infra/supabase), и чистые разборы из слоя domain. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "domain/schedule.h"
#include "domain/time_entry.h"
#include "infra/supabase.h"

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

/* Коллеги из строк БД -> JSON (чистая функция слоя domain). */
static int colleagues_json(const char *rows, const char *self,
                            const char *shift, ShiftFilter filter,
                            char *out, size_t cap) {
    ColleagueList list;
    if (colleagues_from_rows_json(rows, self, shift, filter, &list) != 0) return -1;
    return colleague_list_render_json(&list, out, cap);
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

    /* flush при недоступной базе: ошибки должны учитываться, строк не теряться.
       URL на 127.0.0.1:1 — соединение отклоняется мгновенно, без интернета. */
    setenv("SUPABASE_URL", "https://127.0.0.1:1", 1);
    sb_init();
    assert(sb_enabled());
    int errs = -1;
    int rows = sb_flush(NULL, 0, &errs);
    assert(rows == 0 && errs >= 1);   /* upsert не прошёл -> ошибки посчитаны */
    assert(sb_pending("10.2026", &n) == NULL || n == 0);  /* буфер очищен после flush */

    printf("OK   supabase buffer: дедуп, экранирование, учёт ошибок flush\n");

    /* ---------- ISO-время PostgREST: пояс должен учитываться ----------
       PostgREST отдаёт timestamptz в поясе сервера ("+03:00"). Без разбора
       смещения время читалось как UTC, и age_sec в /diag/supabase был
       отрицательным (-10129 вместо ~750). */
    time_t iso_utc = sb_parse_iso("2026-10-28T17:57:17Z");
    assert(iso_utc > 0);
    assert(sb_parse_iso("2026-10-28T20:57:17+03:00") == iso_utc);
    assert(sb_parse_iso("2026-10-28T20:57:17.850839+03:00") == iso_utc);
    assert(sb_parse_iso("2026-10-28T20:57:17+0300") == iso_utc);
    assert(sb_parse_iso("2026-10-28T17:57:17+00:00") == iso_utc);
    assert(sb_parse_iso("2026-10-28T10:57:17-07:00") == iso_utc);
    assert(sb_parse_iso("2026-10-28T17:57:17.5") == iso_utc);   /* без пояса = UTC */
    assert(sb_parse_iso("мусор") == 0);
    assert(sb_parse_iso(NULL) == 0);
    printf("OK   sb_parse_iso: Z, +03:00, +0300, -07:00, без пояса\n");

    /* ---------- Этап 2: разбор месяца из строк PostgREST ---------- */
    const char *mrows =
        "[{\"day\":1,\"shift_time\":\"15:10-21:00\",\"cabinet\":\"K2.2\"},"
        "{\"day\":1,\"shift_time\":\"7:30-14:40\",\"cabinet\":\"K2.1\"},"
        "{\"day\":2,\"shift_time\":\"7:30-14:40\",\"cabinet\":\"Ц\"},"
        "{\"day\":3,\"shift_time\":\"7:30-14:40\",\"cabinet\":\"K2.1\"}]";
    char a1[4096], a2[4096];
    assert(schedule_month_arrays(mrows, "Бинкевич Татьяна Эдуардовна", 0, NULL,
                                  a1, sizeof a1, a2, sizeof a2) == 0);
    /* утренняя смена = s1, хотя «15:10…» лексикографически меньше */
    assert(!strcmp(a1, "[\"Бинкевич Татьяна Эдуардовна\",\"7:30-14:40\","
                       "\"K2.1\",\"Ц\",\"K2.1\",\"3\"]"));
    assert(!strcmp(a2, "[\"\",\"15:10-21:00\",\"K2.2\",\"\",\"\",\"1\"]"));

    /* одна смена -> s2 пустой массив той же длины (как без строки листа) */
    const char *rows1 = "[{\"day\":2,\"shift_time\":\"9:00-18:00\",\"cabinet\":\"K7\"}]";
    assert(schedule_month_arrays(rows1, "X", 0, NULL, a1, sizeof a1, a2, sizeof a2) == 0);
    assert(!strcmp(a1, "[\"X\",\"9:00-18:00\",\"\",\"K7\",\"1\"]"));
    assert(!strcmp(a2, "[\"\",\"\",\"\",\"\",\"\"]"));

    /* размер месяца из заголовка: счётчик должен стоять после дня 31,
       иначе календарь покажет его как кабинет в ячейке последнего дня */
    assert(schedule_month_arrays(rows1, "X", 31, NULL, a1, sizeof a1, a2, sizeof a2) == 0);
    assert(ncommas(a1) == 33);              /* 34 элемента: имя+смена+31 день+счётчик */
    assert(strstr(a1, ",\"1\"]") != NULL); /* счётчик в конце массива, не в ячейке дня */
    assert(strstr(a1, "[\"X\",\"9:00-18:00\"") == a1);  /* имя и смена в начале */

    /* листовой счётчик «Всего смен»: берём значение Google Sheets, в s2 пусто */
    assert(schedule_month_arrays(mrows, "Бинкевич Татьяна Эдуардовна", 3, "34",
                                  a1, sizeof a1, a2, sizeof a2) == 0);
    assert(strstr(a1, ",\"34\"]") != NULL);
    assert(strstr(a2, ",\"\"]") != NULL);
    assert(schedule_month_arrays(rows1, "X", 31, "34", a1, sizeof a1, a2, sizeof a2) == 0);
    assert(strstr(a1, ",\"34\"]") != NULL);
    assert(ncommas(a2) == 33);

    /* нет данных / битый JSON -> -1 (фоллбэк на файлы) */
    assert(schedule_month_arrays("[]", "X", 0, NULL, a1, sizeof a1, a2, sizeof a2) == -1);
    assert(schedule_month_arrays("{nope", "X", 0, NULL, a1, sizeof a1, a2, sizeof a2) == -1);

    /* ---------- Этап 2: коллеги ---------- */
    const char *crows =
        "[{\"employee\":\"Бинкевич Татьяна Эдуардовна\",\"shift_time\":\"7:30-14:40\"},"
        "{\"employee\":\"Яновская Юлия Алексеевна\",\"shift_time\":\"8:00-14:30\"},"
        "{\"employee\":\"Яновская Юлия Алексеевна\",\"shift_time\":\"8:00-14:30\"},"
        "{\"employee\":\"Гринь Юлия Геннадьевна\",\"shift_time\":\"15:10-21:00\"}]";
    char cj[4096];
    assert(colleagues_json(crows, "Бинкевич Татьяна Эдуардовна", NULL, NULL,
                            cj, sizeof cj) > 0);
    assert(!strcmp(cj, "[{\"name\":\"Яновская Юлия Алексеевна\",\"shift\":\"8:00-14:30\"},"
                       "{\"name\":\"Гринь Юлия Геннадьевна\",\"shift\":\"15:10-21:00\"}]"));
    /* с фильтром по смене: сам сотрудник исключён, вечерняя отсеяна, дубль убран */
    assert(colleagues_json(crows, "Бинкевич Татьяна Эдуардовна", "7:30-14:40",
                            t_no_evening, cj, sizeof cj) > 0);
    assert(!strcmp(cj, "[{\"name\":\"Яновская Юлия Алексеевна\",\"shift\":\"8:00-14:30\"}]"));
    assert(colleagues_json("[broken", NULL, NULL, NULL, cj, sizeof cj) == -1);

    /* ---------- Этап 3: time_entries ---------- */
    const char *trows =
        "[{\"day\":5,\"t_in\":\"07:30\",\"t_out\":\"14:40\",\"hours\":7.17,\"amount\":3585.00},"
        "{\"day\":6,\"t_in\":\"08:00\",\"t_out\":\"\",\"hours\":null,\"amount\":null},"
        "{\"day\":9,\"t_in\":\"10:00\",\"t_out\":\"11:30\",\"hours\":1.5,\"amount\":700.0},"
        "{\"day\":40,\"t_in\":\"10:00\",\"t_out\":\"11:00\",\"hours\":1.0,\"amount\":1.0}]";
    char dj[4096];
    assert(time_entry_days_json(trows, dj, sizeof dj) == 0);
    /* формат в точности как tracking_days_build: 4 поля, числа -> "%.2f" */
    assert(!strcmp(dj,
        "{\"5\":{\"in\":\"07:30\",\"out\":\"14:40\",\"hours\":\"7.17\",\"sum\":\"3585.00\"},"
        "\"6\":{\"in\":\"08:00\",\"out\":\"\",\"hours\":\"\",\"sum\":\"\"},"
        "\"9\":{\"in\":\"10:00\",\"out\":\"11:30\",\"hours\":\"1.50\",\"sum\":\"700.00\"}}"));
    assert(time_entry_days_json("[]", dj, sizeof dj) == 0 && !strcmp(dj, "{}"));
    assert(time_entry_days_json("[broken", dj, sizeof dj) == -1);
    assert(time_entry_days_json(NULL, dj, sizeof dj) == -1);

    /* запись: выключена/некорректный день -> 0 без сетевых вызовов;
       на недоступной базе -> ошибка (0), но не падение */
    assert(sb_time_upsert("10.2026", 0, "07:30", "14:40", "7.17", "3585.00") == 0);
    assert(sb_time_upsert("", 5, "07:30", "14:40", NULL, NULL) == 0);
    assert(sb_time_upsert("10.2026", 5, "07:30", "14:40", "7.17", "3585.00") == 0);
    assert(sb_time_upsert("10.2026", 6, "08:00", "", NULL, NULL) == 0);

    /* ---------- Диагностика: недоступная база -> ok:false, без падения ---------- */
    {
        const char *ms[2] = { "Октябрь 2026", "Ноябрь 2026" };
        char djb[8192];
        assert(sb_diag(ms, 2, djb, sizeof djb) == -1);
        assert(strstr(djb, "\"ok\":false") != NULL);
        assert(strstr(djb, "\"errors\"") != NULL);
    }

    printf("OK   этап 2: разбор месяца и коллег\n");
    printf("OK   этап 3: формат time_entries (паритет с Google), ошибка записи без падения\n");
    printf("ALL TESTS PASSED\n");
    return 0;
}

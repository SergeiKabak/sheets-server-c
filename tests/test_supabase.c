/* Офлайн-тест буфера Supabase (без сети): сбор строк, дедуп, экранирование,
   отбрасывание некорректных дней. Запуск: make test-supabase */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "supabase.h"

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
    printf("ALL TESTS PASSED\n");
    return 0;
}

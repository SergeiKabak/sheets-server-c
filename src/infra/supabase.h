/* infra/supabase.h — буфер графика и учёта времени в Supabase (PostgREST).
 *
 * Идея: фоновая синхронизация читает график из Google Sheets и складывает
 * строки в таблицы schedule / time_entries, чтобы читать данные из SQL
 * мгновенно и хранить их вне сроков жизни бесплатных планов.
 *
 * Реализация — через curl-слой (infra/http_client), без libpq.
 * Управление: SUPABASE_URL + SUPABASE_SECRET_KEY включают синхронизацию,
 * SUPABASE_SYNC=0 — выключает принудительно.
 *
 * Модуль отвечает только за transport и накопление буфера. Разбор данных
 * живёт в слое domain (чистые функции).
 */
#ifndef SHEETS_INFRA_SUPABASE_H
#define SHEETS_INFRA_SUPABASE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Чтение env, вычисление базового URL. Вызывать один раз при старте. */
void sb_init(void);

/* 1, если настроена и не отключена синхронизация */
int  sb_enabled(void);

/* Фиксирует метку времени текущего цикла (ISO-8601 UTC).
   Вызывать ДО сбора строк: она попадёт в updated_at каждой строки,
   а затем — в DELETE устаревших записей. */
void sb_begin_cycle(void);

/* Одна строка графика. Накапливается в памяти до sb_flush().
   employee — с учётом наследования имени пустой строки листа. */
void sb_row(const char *month, int day, const char *employee,
            const char *shift, const char *cabinet);

/* Отправка накопленного (upsert) + удаление устаревших строк по месяцам
   из [months..months+nmonths) + очистка месяцев, пропавших из Google.
   Возвращает число отправленных строк; *errors (не NULL) — кол-во
   неудачных HTTP-вызовов. */
int  sb_flush(const char *const *months, int nmonths, int *errors);

/* Строка журнала sync_log (fire-and-forget): пульс и ошибки синхронизации */
void sb_log(const char *kind, const char *month, int rows, int ok, const char *error);

/* Содержимое буфера по месяцу — для диагностики и тестов (JSON-массив). */
const char *sb_pending(const char *month, int *count);

/* GET-запрос к PostgREST (path вида "/schedule?...").
   Возвращает тело (освобождает caller) или NULL; *status — HTTP-код. */
char *sb_get(const char *path, int *status);

/* UPSERT дня в time_entries (merge-duplicates по PK month+day).
   hours/sum — строки «7.17»/«3585.00» или NULL/"" (частичная запись).
   Возвращает HTTP-код (2xx — успех), 0 — выключено/сетевая ошибка. */
int sb_time_upsert(const char *month_tab, int day,
                   const char *in_t, const char *out_t,
                   const char *hours, const char *sum);

/* Удаление дня (для «Удалить время» в Mini App): DELETE +day.
   204 — успех (удаление отсутствующего дня тоже успех), 0 — выключено
   или сетевая ошибка, иначе HTTP-код. */
int sb_time_day_delete(const char *month_tab, int day);

/* Чтение одного дня для записи (fill/clock): GET ...&month=eq.X&day=eq.N.
   0 — день есть (строки скопированы; пустые означают незаполненный день),
   1 — дня нет, -1 — ошибка БД (вызывающая сторона уходит в фоллбэк). */
int sb_time_day_read(const char *month_tab, int day,
                     char *in, size_t incap,
                     char *out, size_t outcap,
                     char *hours, size_t hcap,
                     char *sum, size_t scap);

/* Диагностика для GET /diag/supabase (ручной мониторинг между этапами):
   точные счётчики schedule (всего/валидных/служебных), построчно по месяцам,
   time_entries, последняя запись и последний restore из sync_log с возрастом
   (пульс). months/nmonths — месяцы из файлового кэша.
   Возвращает 0, -1 если общий счёт не получен или sync выключен. */
int sb_diag(const char *const *months, int nmonths, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* SHEETS_INFRA_SUPABASE_H */

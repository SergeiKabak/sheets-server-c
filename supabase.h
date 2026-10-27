/* supabase.h — буфер графика в Supabase (PostgREST), этап 1 плана.
 *
 * Идея: фоновый синхронизатор (background_updater, раз в 60 с) читает
 * график из Google Sheets и складывает строки в таблицу schedule,
 * чтобы в будущем читать данные из SQL мгновенно (этап 2) и хранить
 * их вне сроков жизни бесплатных планов.
 *
 * Реализация — через существующий curl-слой (http.c), без libpq.
 * Управление: SUPABASE_URL + SUPABASE_SECRET_KEY включают синхронизацию,
 * SUPABASE_SYNC=0 — выключает принудительно.
 */
#ifndef SUPABASE_H
#define SUPABASE_H

#include <stddef.h>

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

#endif /* SUPABASE_H */

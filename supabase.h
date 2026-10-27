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

/* ---------------- Этап 2: чтение из SQL ---------------- */

/* GET-запрос к PostgREST (path вида "/schedule?...").
   Возвращает тело (освобождает caller) или NULL; *status — HTTP-код. */
char *sb_get(const char *path, int *status);

/* Строит JSON-массивы s1/s2 месяца Mini App из строк PostgREST:
   rows_json — ответ /schedule?select=day,shift_time,cabinet&...
   sheet_days — число столбцов-дней листа (n-3 из файлового кэша): счётчик
   должен стоять после последнего дня, а не оказываться в ячейке дня;
   total — листовое значение счётчика «Всего смен» (как в Google Sheets);
   NULL — запасной подсчёт, "" — в листе пусто.
   s1/s2: [имя, смена, кабинеты по дням, всего смен] — формат строки листа.
   Смены сортируются по времени начала (как в листе: утренняя -> вечерняя).
   Возвращает 0 при успехе, -1 если данных нет / JSON не разобран. */
int sb_month_arrays(const char *rows_json, const char *employee,
                    int sheet_days, const char *total,
                    char *s1, size_t s1cap, char *s2, size_t s2cap);

/* Предикат пересечения смен (передаёт вызывающая сторона — shifts_overlap) */
typedef int (*SBShiftFilter)(const char *row_shift, const char *query);

/* JSON-массив коллег из строк /schedule?...&select=employee,shift_time
   в формате [{"name":..,"shift":..}] — как find_cabinet_colleagues_from_index:
   сам сотрудник исключается, фильтр по смене через filter, дедуп имя+смена.
   Возвращает длину или -1 при ошибке разбора. */
int sb_colleagues_json(const char *rows_json, const char *self,
                       const char *shift, SBShiftFilter filter,
                       char *out, size_t outcap);

#endif /* SUPABASE_H */

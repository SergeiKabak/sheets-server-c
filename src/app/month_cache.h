/* app/month_cache.h — файловые кэши графика.
 *
 * Четыре независимых кэша, каждый со своим назначением:
 *   /tmp/months.dat            — месяцы-листы (структура строк сотрудника)
 *   /tmp/cabinet_index.txt     — индекс «месяц/день/кабинет -> сотрудник»
 *   /tmp/mresp_<hex>.json      — готовый ответ /api/month (s1/s2)
 *   /tmp/mcol_<hex>.json       — строки месяца целиком (для коллег)
 */
#ifndef SHEETS_APP_MONTH_CACHE_H
#define SHEETS_APP_MONTH_CACHE_H

#include "domain/schedule.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MONTHS_FILE        "/tmp/months.dat"
#define CABINET_INDEX_FILE "/tmp/cabinet_index.txt"

/* Возраст кэша готового ответа месяца (секунды). Держим равным TTL чтения
 * Google (SCHEDULE_GOOGLE_TTL): правки графика видны не позже чем через
 * 5 минут и в БД, и в ответе Mini App. */
#define MONTH_RESP_TTL 300

/* ---------- месяцы-листы ---------- */

/* Сохраняет месяцы в /tmp/months.dat. Возвращает 0 при успехе. */
int month_cache_save(const MonthSchedule *months, int count);

/* Читает месяцы из /tmp/months.dat.
 * Возвращает число прочитанных месяцев (0 — файла нет). */
int month_cache_load(MonthSchedule *months, int max);

/* ---------- индекс кабинетов ---------- */

/* Путь к файлу индекса. */
const char *cabinet_index_path(void);

/* ---------- кэш готового ответа месяца ---------- */

/* Читает кэш ответа ({"t":..,"s1":[..],"s2":[..]}).
 * Возвращает 0 при успехе, -1 если кэша нет/он устарел/повреждён. */
int month_response_read(const char *title, char *out, size_t cap);

/* Пишет кэш ответа атомарно. Возвращает 0 при успехе. */
int month_response_write(const char *title, const char *s1, const char *s2);

/* ---------- кэш строк месяца (для коллег) ---------- */

/* Читает полный массив строк месяца. -1 если нет/устарел/повреждён. */
int month_colleagues_cache_read(const char *title, char *out, size_t cap);

/* Пишет полный массив строк месяца атомарно. */
int month_colleagues_cache_write(const char *title, const char *json);

/* Из полного массива оставляет только строки нужного дня и кабинета. */
int month_colleagues_cache_filter(const char *all, int day, const char *cabinet,
                                  char *out, size_t outcap);

#ifdef __cplusplus
}
#endif

#endif /* SHEETS_APP_MONTH_CACHE_H */

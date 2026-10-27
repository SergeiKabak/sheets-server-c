/* app/schedule_service.h — сценарии работы с графиком смен.
 *
 * Здесь собирается «картина» графика: месяцы из Google Sheets, коллеги по
 * кабинету (из БД или из файлового индекса), строки месяца.
 */
#ifndef SHEETS_APP_SCHEDULE_SERVICE_H
#define SHEETS_APP_SCHEDULE_SERVICE_H

#include "app/config.h"
#include "domain/schedule.h"

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Список видимых листов таблицы (скрытые пропускаются).
 * sheets — массив строк длиной SCHEDULE_MAX_NAME.
 * Возвращает число листов. */
int schedule_visible_sheets(const AppConfig *cfg,
                            char sheets[][SCHEDULE_MAX_NAME], int max_sheets);

/* Строка листа: массив строк (ячеек). */
typedef char **SheetCells;

/* Разбор ответа values API: массив массивов строк.
 * Возвращает число строк; rows_out/lens_out — malloc-нутые массивы
 * (освобождает вызывающий). */
int schedule_parse_rows(const char *json, SheetCells **rows_out, int **row_lens_out);

/* Освобождает результат schedule_parse_rows. */
void schedule_free_rows(SheetCells *rows, int *lens, int count);

/* Собирает данные каждого видимого листа (=месяца) в массив.
 * idx — файл индекса кабинетов (может быть NULL: индекс не нужен).
 * Возвращает число собранных месяцев. */
int schedule_build_months(const AppConfig *cfg, MonthSchedule *out, int max,
                           FILE *idx);

/* Коллеги по кабинету из файлового индекса (фоллбэк, когда нет БД).
 * Возвращает длину JSON-массива либо -1. */
int schedule_colleagues_from_index(const char *month, int day, const char *cabinet,
                                   const char *shift, char *out, size_t outcap);

/* Строки месяца из БД -> s1/s2 (формат строки листа).
 * Возвращает 0 при успехе, -1 если данных нет либо БД недоступна. */
int schedule_read_month_from_db(const char *title, const char *employee,
                                 int sheet_days, const char *total,
                                 char *s1, size_t s1cap, char *s2, size_t s2cap);

/* Коллеги по кабинету из БД -> JSON-массив.
 * Возвращает 0 при успехе, -1 если данных нет либо БД недоступна. */
int schedule_read_colleagues_from_db(const char *month, int day, const char *cabinet,
                                     const char *shift, const char *self,
                                     char *out, size_t outcap);

/* Обновляет кэш строк месяца (mcol) из БД — для быстрых ответов коллег.
 * Возвращает 0 при успехе, -1 при ошибке. */
int schedule_refresh_colleagues_cache(const char *title);

#ifdef __cplusplus
}
#endif

#endif /* SHEETS_APP_SCHEDULE_SERVICE_H */

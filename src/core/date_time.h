/* core/date_time.h — даты, время и названия месяцев (чистые функции).
 *
 * Не зависит от «текущего времени»: функции, которым нужен год по умолчанию,
 * получают его аргументом. Источник «сейчас» живёт в infra/clock.
 */
#ifndef SHEETS_CORE_DATE_TIME_H
#define SHEETS_CORE_DATE_TIME_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Название месяца по номеру (1..12) либо NULL. */
const char *month_name(int mon);

/* "10.2026" -> "Октябрь 2026". Если разобрать нельзя — копия входа. */
void month_label(const char *numeric, char *out, size_t cap);

/* "Октябрь 2026" -> "10.2026". Если год не указан, берётся default_year.
 * Возвращает 0 при успехе, -1 если месяц не распознан. */
int month_numeric_from_label(const char *month, int default_year, char *out, size_t cap);

/* Разбор "10.2026" -> (mon, year). Возвращает 0 при успехе. */
int month_parse(const char *numeric, int *mon, int *year);

/* Число дней в месяце (0 при неверном номере месяца). */
int days_in_month(int mon, int year);

/* Нормализует дату к "ДД.ММ.ГГГГ" (принимает . / - и YYYY-MM-DD). */
void date_normalize(const char *in, char *out, size_t cap);

/* Разбирает дату в формате ДД.ММ.ГГГГ. Возвращает 0 при успехе. */
int date_parse(const char *s, int *day, int *month, int *year);

/* Сравнение двух дат "ДД.ММ.ГГГГ": <0, 0, >0. */
int date_compare(const char *a, const char *b);

/* Нормализует время к "ЧЧ:ММ" (Google может вернуть "8:00"). */
void time_normalize(const char *in, char *out, size_t cap);

/* "ЧЧ:ММ" -> минуты от полуночи; -1 при ошибке. */
int time_parse(const char *s);

#ifdef __cplusplus
}
#endif

#endif /* SHEETS_CORE_DATE_TIME_H */

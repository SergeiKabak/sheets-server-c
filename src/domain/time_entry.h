/* domain/time_entry.h — модель отметок рабочего времени.
 *
 * Отметка дня в БД: {month, day, t_in, t_out, hours, amount}.
 * Mini App показывает их в виде JSON-объекта:
 *   {"5":{"in":"07:30","out":"14:40","hours":"7.17","sum":"3585.00"}}
 */
#ifndef SHEETS_DOMAIN_TIME_ENTRY_H
#define SHEETS_DOMAIN_TIME_ENTRY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Строки БД ({"day":..,"t_in":..,"t_out":..,"hours":..,"amount":..})
 * -> JSON-объект дней Mini App (всегда 4 поля на день).
 * Возвращает 0 при успехе (объект может быть "{}"), -1 при ошибке разбора. */
int time_entry_days_json(const char *rows_json, char *out, size_t outcap);

/* Часы между приходом и уходом (переход через полночь учитывается). */
double time_entry_hours(const char *in_time, const char *out_time);

/* Сумма за день: округлённые часы (строкой) * ставка.
 * Часы берутся из строки — чтобы не было расхождения с тем, что видит
 * пользователь ("5.02" вместо 5.016666). */
double time_entry_sum(const char *hours, double rate);

#ifdef __cplusplus
}
#endif

#endif /* SHEETS_DOMAIN_TIME_ENTRY_H */

/* infra/clock.h — источник «текущего времени» (эффект).
 *
 * Вынесен отдельно от чистых функций core/date_time: те работают с уже
 * известной датой, эта — читает часы.
 */
#ifndef SHEETS_INFRA_CLOCK_H
#define SHEETS_INFRA_CLOCK_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Текущие дата ("ДД.ММ.ГГГГ") и время ("ЧЧ:ММ") в локальной зоне
 * (локальное время = UTC + tz_offset секунд). Любой из выходов может быть NULL. */
void clock_datetime(int tz_offset, char *date, size_t dcap,
                    char *time_s, size_t tcap);

/* Текущий месяц по локальному времени: "10.2026". */
void clock_month_tab(int tz_offset, char *out, size_t cap);

/* Текущий год по локальному времени. */
int clock_current_year(int tz_offset);

/* Монотонное время в миллисекундах (для замеров длительности). */
long clock_monotonic_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* SHEETS_INFRA_CLOCK_H */

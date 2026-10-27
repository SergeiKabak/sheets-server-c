/* app/tracking_cache.h — кэш отметок времени и флаги выгрузки.
 *
 *   /tmp/trk_days_<tab>.json — отметки месяца (обновляется фоновым циклом)
 *   /tmp/gdirty_<tab>        — флаг «месяц ждёт выгрузки в Google»
 */
#ifndef SHEETS_APP_TRACKING_CACHE_H
#define SHEETS_APP_TRACKING_CACHE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Возраст кэша отметок (секунды). 90 — запас на время фонового обновления. */
#define TRK_CACHE_TTL 90

/* Читает кэш отметок месяца. max_age < 0 — возраст не проверяется.
 * Возвращает 0 при успехе, -1 если кэша нет/он устарел. */
int trk_cache_read(const char *tab, char *out, size_t cap, int max_age);

/* Пишет кэш отметок месяца атомарно. */
int trk_cache_write(const char *tab, const char *json);

/* Сбрасывает кэш отметок месяца (после записи). */
void trk_cache_invalidate(const char *tab);

/* Ставит флаг «месяц ждёт выгрузки в Google». */
void trk_dirty_put(const char *tab);

/* Снимает флаг «месяц ждёт выгрузки в Google». */
void trk_dirty_clear(const char *tab);

#ifdef __cplusplus
}
#endif

#endif /* SHEETS_APP_TRACKING_CACHE_H */

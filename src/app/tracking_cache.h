/* app/tracking_cache.h — кэш отметок времени и флаги выгрузки.
 *
 *   /tmp/trk_days_<tab>.json — отметки месяца (обновляется фоновым циклом)
 *   /tmp/trk_tabs.txt        — список вкладок-месяцев Google (TTL — час)
 *   /tmp/gdirty_<tab>        — флаг «месяц ждёт выгрузки в Google»
 */
#ifndef SHEETS_APP_TRACKING_CACHE_H
#define SHEETS_APP_TRACKING_CACHE_H

#include "app/config.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Как часто фоновый цикл перестраивает кэш отметок (секунды).
 * Раньше это происходило на каждом тике (60 с) — по запросу в Supabase
 * на каждый месяц, то есть тысячи запросов в сутки только за счёт кэша.
 * Отметки пишутся через write-through и инвалидируют кэш сразу
 * (trk_cache_invalidate), поэтому 10 минут — достаточная свежесть. */
#define TRK_REFRESH_TTL (10 * 60)

/* Возраст кэша отметок, при котором Mini App ещё отвечает из файла.
 * Чуть больше окна обновления — чтобы кэш не «протухал» между тиками
 * и запросы не уходили в БД без нужды. */
#define TRK_CACHE_TTL (TRK_REFRESH_TTL + 120)

/* Возраст кэша списка вкладок: тот же интервал, что и у опроса Google. */
#define TRK_TABS_TTL GOOGLE_SYNC_TTL

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

/* ---------- список вкладок-месяцев Google ----------
 * Список меняется раз в месяц, а фоновый цикл спрашивает его каждую минуту.
 * Кэш (/tmp/trk_tabs.txt) убирает из тика запрос метаданных к Google. */

/* Читает список. max_age < 0 — возраст не проверяется (устаревший список
 * можно отдать ночью, когда Google не опрашивается). Возвращает 0 при
 * успехе; *out — массив malloc-нутых строк (освобождает вызывающий). */
int trk_tabs_cache_read(char ***out, int *count, int max, int max_age);

/* Пишет список вкладок в кэш. */
int trk_tabs_cache_write(char *const *tabs, int count);

/* Сбрасывает кэш (после создания новой вкладки месяца). */
void trk_tabs_cache_invalidate(void);

#ifdef __cplusplus
}
#endif

#endif /* SHEETS_APP_TRACKING_CACHE_H */

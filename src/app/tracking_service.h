/* app/tracking_service.h — сценарии учёта рабочего времени.
 *
 * Источник правды — БД (Supabase), Google Sheets — архив/отчёт для людей.
 * Каждый сценарий умеет три источника: БД -> фоллбэк на Google -> кэш.
 *
 * Модуль не знает про HTTP: он возвращает готовый JSON-ответ и код,
 * а слой web лишь записывает их в сокет.
 */
#ifndef SHEETS_APP_TRACKING_SERVICE_H
#define SHEETS_APP_TRACKING_SERVICE_H

#include "app/config.h"
#include "infra/google_sheets.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TRACKING_MAX_MONTHS 24

typedef struct {
    GoogleSheets   sheets;
    const AppConfig *cfg;      /* конфигурация (не изменяется) */
    double          rate;      /* ставка в час (HOURLY_RATE) */
    int             tz_offset; /* смещение локального времени, секунды */
    char            current_tab[64]; /* вкладка текущего месяца */
} TrackingService;

/* Готовит сервис: читает окружение, ищет (или создаёт) таблицу учёта. */
void tracking_service_init(TrackingService *svc, const AppConfig *cfg);

/* Имя месяца по цифровому виду ("10.2026" -> "Октябрь 2026"). */
void tracking_month_label(const char *numeric, char *out, size_t cap);

/* Список вкладок-месяцев таблицы учёта времени ("10.2026", ...).
 * Возвращает 0 при успехе; в *count — число месяцев, в *out — массив
 * malloc-нутых строк (освобождает вызывающий). */
int tracking_list_months(TrackingService *svc, char ***out, int *count, int max);

/* Итог за месяц (вкладка вида "10.2026"): часы, сумма, число дней. */
int tracking_month_total(TrackingService *svc, const char *month,
                          double *hours, double *sum, int *days);

/* Отметки времени за месяц графика (например "Октябрь 2026" или "10.2026").
 * Возвращает HTTP-код (200/400/500), в out пишет JSON:
 * {"ok":true,"month":"10.2026","label":"Октябрь 2026",
 *  "today":"26.10.2026","days":{"5":{"in":"07:30","out":"14:40",
 *  "hours":"7.17","sum":"3585.00"}}} */
int tracking_days_json(TrackingService *svc, const char *month, char *out, size_t cap);

/* То же, но только из локального кэша (без обращения к Google/БД).
 * Возвращает 0, если свежий кэш есть; -1 — если кэша нет/он устарел. */
int tracking_days_cached_json(TrackingService *svc, const char *month,
                               char *out, size_t cap);

/* Обновляет кэш отметок для месяцев графика. Вызывается фоновым
 * процессом. months — названия месяцев из расписания. */
int tracking_cache_refresh(TrackingService *svc, const char *const *months, int count);

/* Выгрузка БД -> Google (БД — первоисточник, Google — архив):
 * обработка /tmp/gdirty_* (после записей) и разовая выгрузка всех
 * месяцев (раз в сутки — подхватывает правки дашборда). */
void tracking_push_pending(TrackingService *svc);
int  tracking_push_all(TrackingService *svc, const char *const *months, int nmonths);

/* Заполняет отработанное время за произвольный день месяца графика.
 * month — "Октябрь 2026" или "10.2026", day — 1..31,
 * in_time / out_time — "ЧЧ:ММ" (например "07:30").
 * Возвращает HTTP-код (200/400/409/500), в out пишет JSON-ответ. */
int tracking_fill_day(TrackingService *svc, const char *month, int day,
                       const char *in_time, const char *out_time,
                       char *out, size_t cap);

/* Удаляет отметку дня. Возвращает HTTP-код (200/400/500). */
int tracking_erase_day(TrackingService *svc, const char *month, int day,
                        char *out, size_t cap);

/* Создаёт вкладку месяца (MM.YYYY) и записывает в неё заголовки.
 * Используется для диагностики/ручного создания отсутствующего месяца. */
int tracking_ensure_month(TrackingService *svc, const char *month,
                           char *out, size_t cap);

/* Выполняет отметку прихода/ухода (action: "in" или "out").
 * Возвращает HTTP-код (200/400/409/500), в out пишет JSON-ответ. */
int tracking_do_clock(TrackingService *svc, const char *action,
                       char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* SHEETS_APP_TRACKING_SERVICE_H */

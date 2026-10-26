/*
 * Модуль учёта рабочего времени (Time tracking).
 *
 * Записывает приход/уход сотрудника в Google Sheets
 * (вкладка = текущий месяц и год, например "Октябрь 2026").
 *
 * Требуется Service Account (OAuth2), т.к. запись по API key запрещена.
 * Переменные окружения:
 *   TRACKING_SHEET_ID   — ID таблицы Time tracking
 *   GOOGLE_SERVICE_ACCOUNT_JSON — JSON-ключ сервисного аккаунта
 *   HOURLY_RATE         — ставка в час
 */

#ifndef TRACKING_H
#define TRACKING_H

#include <stddef.h>

/* Инициализирует конфигурацию из переменных окружения (ключ сервисного
   аккаунта, ID таблицы, ставку). Вызывается и из HTTP-обработчика,
   и напрямую из Telegram-бота. */
void tracking_ensure_init(void);

/* Обрабатывает запросы /api/tracking/...
 * method — HTTP-метод, path — путь с query-строкой, body — тело запроса (может быть NULL) */
void tracking_handle(int fd, const char *method, const char *path, const char *body);

/* Название месяца по цифровому виду ("10.2026" -> "Октябрь 2026"). */
void tracking_month_label(const char *numeric, char *out, size_t cap);

/* Список вкладок-месяцев таблицы учёта времени ("10.2026", ...).
   Возвращает 0 при успехе; в *count — число месяцев, в *out — массив
   malloc-нутых строк (освобождает вызывающий). */
int tracking_list_months(char ***out, int *count, int max);

/* Итог за месяц (вкладка вида "10.2026"): часы, сумма, число дней. */
int tracking_month_total(const char *month, double *hours, double *sum, int *days);

/* Отметки времени за месяц графика (например "Октябрь 2026" или "10.2026").
   Возвращает HTTP-код (200/400/500), в out пишет JSON:
   {"ok":true,"month":"10.2026","label":"Октябрь 2026",
    "today":"26.10.2026","days":{"5":{"in":"07:30","out":"14:40",
    "hours":"7.17","sum":"3585.00"}}}
   Нужен Mini App, чтобы показать "+" только там, где время ещё не внесено. */
int tracking_days_json(const char *month, char *out, size_t cap);

/* Заполняет отработанное время за произвольный день месяца графика.
   month — "Октябрь 2026" или "10.2026", day — 1..31,
   in_time / out_time — "ЧЧ:ММ" (например "07:30").
   Создаёт вкладку месяца при необходимости и добавляет строку сотрудника.
   Возвращает HTTP-код (200/400/409/500), в out пишет JSON-ответ. */
int tracking_fill_day(const char *month, int day, const char *in_time,
                      const char *out_time, char *out, size_t cap);

/* Выполняет отметку прихода/ухода программно (используется Telegram-ботом).
   action: "in" или "out". Возвращает HTTP-код (200/400/409/500),
   в out пишет JSON-ответ. */
int tracking_do_clock(const char *action, char *out, size_t cap);

#endif /* TRACKING_H */

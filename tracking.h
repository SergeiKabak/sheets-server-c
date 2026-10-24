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

/* Выполняет отметку прихода/ухода программно (используется Telegram-ботом).
   action: "in" или "out". Возвращает HTTP-код (200/400/409/500),
   в out пишет JSON-ответ. */
int tracking_do_clock(const char *action, char *out, size_t cap);

#endif /* TRACKING_H */

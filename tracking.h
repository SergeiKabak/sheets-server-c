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
 *   EMPLOYEE_MAP        — {"chat_id":{"name":"ФИО","rate":"500"}, ...}
 *   HOURLY_RATE         — ставка по умолчанию (если нет в EMPLOYEE_MAP)
 */

#ifndef TRACKING_H
#define TRACKING_H

/* Обрабатывает запросы /api/tracking/...
 * method — HTTP-метод, path — путь с query-строкой, body — тело запроса (может быть NULL) */
void tracking_handle(int fd, const char *method, const char *path, const char *body);

#endif /* TRACKING_H */

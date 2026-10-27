/* app/reminder_service.h — сценарии напоминаний о смене.
 *
 * Планирование — чистые функции domain/reminder; здесь только сохранение
 * в Supabase и отправка созревших напоминаний через Telegram.
 *
 * Два режима:
 *   разовый (reminder_schedule) — «напомни завтра в 09:00 / за час до смены»;
 *   по графику (reminder_daily_set + reminders_materialize) — каждый день
 *   перед началом и в конце смены из таблицы.
 */
#ifndef SHEETS_APP_REMINDER_SERVICE_H
#define SHEETS_APP_REMINDER_SERVICE_H

#include "app/config.h"
#include "app/tracking_service.h"
#include "domain/schedule.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* За сколько минут до начала смены напоминать о приходе. */
#define REMINDER_START_LEAD_MIN 15
/* За сколько минут до конца смены напоминать об уходе (0 — в момент конца). */
#define REMINDER_END_LEAD_MIN   0

/* Сколько чатов с включённым режимом обрабатываем за тик. */
#define REMINDER_MAX_SUBS       64

/* Планирует разовое напоминание по выбору пользователя
 * ("08"/"09"/"20"/"shift"). months/count — кэш графика (нужен для текста
 * и «за час до смены»). reply заполняется ответом для чата в любом случае.
 * Возвращает 0 — сохранено, 1 — напоминание не нужно (причина в reply),
 * -1 — не удалось сохранить (БД недоступна). */
int reminder_schedule(const AppConfig *cfg, TrackingService *svc,
                      const MonthSchedule *months, int count,
                      long long chat_id, const char *choice,
                      char *reply, size_t cap);

/* Включает/выключает ежедневные напоминания о начале и конце смены.
 * reply — текст ответа в чат.
 * 0 — успех, -1 — БД недоступна. */
int reminder_daily_set(long long chat_id, int enabled,
                       char *reply, size_t cap);

/* Отменяет неотправленные напоминания чата.
 * 0 — успех, -1 — БД недоступна. */
int reminder_cancel(long long chat_id);

/* Раскладывает в БД напоминания на сегодня по графику для всех подписанных
 * чатов. Идемпотентна: дубли отсекает уникальный индекс, прошедшие моменты
 * пропускаются. Возвращает число добавленных.
 * Вызывается фоновым циклом, когда кэш графика уже загружен. */
int reminders_materialize(const AppConfig *cfg, const TrackingService *svc,
                          const MonthSchedule *months, int count);

/* Отправляет созревшие напоминания (вызывается фоновым циклом).
 * Возвращает число отправленных. */
int reminders_flush(const AppConfig *cfg);

#ifdef __cplusplus
}
#endif

#endif /* SHEETS_APP_REMINDER_SERVICE_H */

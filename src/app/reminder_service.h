/* app/reminder_service.h — сценарии напоминаний о завтрашней смене.
 *
 * Планирование — чистая функция domain/reminder; здесь только сохранение
 * в Supabase и отправка созревших напоминаний через Telegram.
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

/* Планирует напоминание по выбору пользователя ("08"/"09"/"20"/"shift").
 * months/count — кэш графика (нужен для текста и «за час до смены»).
 * reply заполняется ответом для чата в любом случае.
 * Возвращает 0 — сохранено, 1 — напоминание не нужно (причина в reply),
 * -1 — не удалось сохранить (БД недоступна). */
int reminder_schedule(const AppConfig *cfg, TrackingService *svc,
                      const MonthSchedule *months, int count,
                      long long chat_id, const char *choice,
                      char *reply, size_t cap);

/* Отменяет неотправленные напоминания чата.
 * 0 — успех, -1 — БД недоступна. */
int reminder_cancel(long long chat_id);

/* Отправляет созревшие напоминания (вызывается фоновым циклом).
 * Возвращает число отправленных. */
int reminders_flush(const AppConfig *cfg);

#ifdef __cplusplus
}
#endif

#endif /* SHEETS_APP_REMINDER_SERVICE_H */

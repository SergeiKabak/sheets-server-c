/* app/reminder_service.h — сценарии напоминаний о смене.
 *
 * Планирование — чистые функции domain/reminder; здесь только сохранение
 * в Supabase и отправка созревших напоминаний через Telegram.
 *
 * Два режима:
 *   разовый (reminder_schedule) — «напомни завтра в 09:00 / за час до смены»;
 *   по графику (reminder_settings_save + reminders_materialize) — каждый день
 *   перед началом и после конца смены из таблицы, с настройками на чат.
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

/* Значения по умолчанию: за 15 минут до начала, сразу после конца. */
#define REMINDER_START_LEAD_DEFAULT 15
#define REMINDER_END_DELAY_DEFAULT  0

/* Допустимые значения кнопок настроек. */
#define REMINDER_LEAD_MAX  120
#define REMINDER_DELAY_MAX 120

/* Сколько чатов с включённым режимом обрабатываем за тик. */
#define REMINDER_MAX_SUBS  64

/* Настройки режима «по графику» для одного чата. */
typedef struct {
    int enabled;
    int start_lead_min;   /* за сколько минут до начала смены напомнить */
    int end_delay_min;    /* через сколько минут после конца напомнить */
} ReminderSettings;

/* Планирует разовое напоминание по выбору пользователя
 * ("08"/"09"/"20"/"shift"). months/count — кэш графика (нужен для текста
 * и «за час до смены»). reply заполняется ответом для чата в любом случае.
 * Возвращает 0 — сохранено, 1 — напоминание не нужно (причина в reply),
 * -1 — не удалось сохранить (БД недоступна). */
int reminder_schedule(const AppConfig *cfg, TrackingService *svc,
                      const MonthSchedule *months, int count,
                      long long chat_id, const char *choice,
                      char *reply, size_t cap);

/* Читает настройки чата. При отсутствии записи — значения по умолчанию.
 * 0 — успех, -1 — БД недоступна. */
int reminder_settings_get(long long chat_id, ReminderSettings *out);

/* Сохраняет настройки чата (включая enabled).
 * 0 — успех, -1 — БД недоступна. */
int reminder_settings_save(long long chat_id, const ReminderSettings *s);

/* Отменяет неотправленные напоминания чата.
 * 0 — успех, -1 — БД недоступна. */
int reminder_cancel(long long chat_id);

/* Раскладывает в БД напоминания на сегодня по графику для всех подписанных
 * чатов, с их настройками. Идемпотентна: дубли отсекает уникальный индекс,
 * прошедшие моменты пропускаются. Возвращает число добавленных.
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

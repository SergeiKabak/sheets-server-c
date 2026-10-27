/* domain/reminder.h — планирование напоминаний о завтрашней смене.
 *
 * Чистые функции: на вход «сейчас», часовой пояс и кэш графика, на выход —
 * момент отправки и готовый текст сообщения. Ни БД, ни сети, ни Telegram:
 * сохранение и отправку делают app/infra-слои.
 */
#ifndef SHEETS_DOMAIN_REMINDER_H
#define SHEETS_DOMAIN_REMINDER_H

#include "domain/schedule.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define REMINDER_TEXT_MAX  512
#define REMINDER_REPLY_MAX 768

typedef struct {
    int       ok;              /* 0 — план готов, 1 — напоминать не о чем */
    long long remind_at;       /* UTC, секунды с эпохи */
    char      when[32];        /* локальное время отправки, "ЧЧ:ММ" */
    char      text[REMINDER_TEXT_MAX];   /* текст напоминания */
    char      reply[REMINDER_REPLY_MAX]; /* ответ пользователю */
} ReminderPlan;

/* Строит план напоминания.
 *   now      — текущее время (UTC, секунды с эпохи);
 *   tz_offset— смещение локального времени в секундах;
 *   months   — кэш графика (может быть NULL/count=0 — текста о смене не будет);
 *   choice   — "08" | "09" | "20" — завтра в это локальное время,
 *              "shift" — за час до начала завтрашней смены.
 * Возвращает 0 если план готов (plan->ok == 0), 1 если напоминание
 * бессмысленно (причина в plan->reply), -1 при неверных аргументах. */
int reminder_plan(long long now, int tz_offset,
                  const MonthSchedule *months, int count,
                  const char *choice, ReminderPlan *plan);

#ifdef __cplusplus
}
#endif

#endif /* SHEETS_DOMAIN_REMINDER_H */

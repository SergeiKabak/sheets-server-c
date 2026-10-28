/* domain/reminder.h — планирование напоминаний о смене.
 *
 * Чистые функции: на вход «сейчас», часовой пояс и кэш графика, на выход —
 * момент отправки и готовый текст сообщения. Ни БД, ни сети, ни Telegram:
 * сохранение и отправку делают app/infra-слои.
 *
 * Два режима:
 *   reminder_plan()      — разово на завтра (08:00 / 09:00 / 20:00 / за час);
 *   reminder_day_slots() — ежедневно по графику: перед началом и в конце
 *                          сегодняшней смены.
 */
#ifndef SHEETS_DOMAIN_REMINDER_H
#define SHEETS_DOMAIN_REMINDER_H

#include "domain/schedule.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define REMINDER_TEXT_MAX   512
#define REMINDER_REPLY_MAX  768
#define REMINDER_SLOTS_MAX  4
#define REMINDER_KIND_MAX   24

typedef struct {
    int       ok;              /* 0 — план готов, 1 — напоминать не о чем */
    long long remind_at;       /* UTC, секунды с эпохи */
    char      when[32];        /* локальное время отправки, "ЧЧ:ММ" */
    char      text[REMINDER_TEXT_MAX];   /* текст напоминания */
    char      reply[REMINDER_REPLY_MAX]; /* ответ пользователю */
} ReminderPlan;

/* Одно напоминание «по графику»: когда отправить и что сказать. */
typedef struct {
    long long at;                          /* UTC, секунды с эпохи */
    char      kind[REMINDER_KIND_MAX];     /* "shift_start" | "shift_end" */
    char      text[REMINDER_TEXT_MAX];
} ReminderSlot;

/* Строит план разового напоминания.
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

/* Напоминания на сегодняшнюю смену по графику:
 *   — за start_lead минут до начала («отметьте приход»);
 *   — через no_mark_delay минут после начала, если прихода всё ещё нет
 *     («отметьте начало — или конец, если смена уже закончилась»);
 *   — через end_delay минут после конца («отметьте уход»; 0 — сразу).
 * Уже прошедшие моменты пропускаются, поэтому функция идемпотентна:
 * её можно звать каждый тик фонового цикла.
 * Возвращает число слотов (0 — смены сегодня нет / нет графика),
 * -1 при неверных аргументах. */
int reminder_day_slots(long long now, int tz_offset,
                       const MonthSchedule *months, int count,
                       int start_lead, int end_delay, int no_mark_delay,
                       ReminderSlot *out, int max);

/* Нужно ли напоминать «нет отметки»: 1 — день не отмечен ни приходом,
 * ни уходом; 0 — отметка уже есть. Пустые/NULL строки — нет отметки. */
int reminder_needs_no_mark(const char *t_in, const char *t_out);

#ifdef __cplusplus
}
#endif

#endif /* SHEETS_DOMAIN_REMINDER_H */

#include "app/reminder_service.h"

#include "domain/reminder.h"
#include "infra/supabase.h"
#include "infra/telegram_api.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define REMINDER_BATCH 20   /* сколько напоминаний разбираем за один тик */

int reminder_schedule(const AppConfig *cfg, TrackingService *svc,
                      const MonthSchedule *months, int count,
                      long long chat_id, const char *choice,
                      char *reply, size_t cap) {
    (void)cfg;
    if (reply && cap) reply[0] = '\0';

    ReminderPlan plan;
    int rc = reminder_plan((long long)time(NULL),
                           svc ? svc->tz_offset : 0,
                           months, count, choice, &plan);
    if (rc != 0) {
        snprintf(reply, cap, "%s", plan.reply);
        return rc;
    }

    int st = sb_reminder_add(chat_id, plan.remind_at, plan.text);
    if (st < 200 || st > 299) {
        snprintf(reply, cap,
                 "Не удалось сохранить напоминание: база недоступна. "
                 "Попробуйте позже.");
        return -1;
    }

    snprintf(reply, cap, "%s", plan.reply);
    return 0;
}

int reminder_cancel(long long chat_id) {
    int st = sb_reminder_cancel(chat_id);
    return (st >= 200 && st <= 299) ? 0 : -1;
}

int reminders_flush(const AppConfig *cfg) {
    if (!cfg || !cfg->bot_token || !*cfg->bot_token || !sb_enabled()) return 0;

    SbReminder list[REMINDER_BATCH];
    int n = sb_reminder_due((long long)time(NULL), list, REMINDER_BATCH);
    int sent = 0;

    for (int i = 0; i < n; i++) {
        if (!list[i].text[0]) continue;
        telegram_send_text(cfg->bot_token, list[i].chat_id, list[i].text);
        /* Отправка без подтверждения доставки: помечаем, чтобы не зациклиться
           на одном напоминании при сбое сети. */
        sb_reminder_mark_sent(list[i].id);
        sent++;
    }
    return sent;
}

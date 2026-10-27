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
        if (reply && cap) snprintf(reply, cap, "%s", plan.reply);
        return rc;
    }

    int st = sb_reminder_add(chat_id, plan.remind_at, plan.text);
    if (st < 200 || st > 299) {
        if (reply && cap)
            snprintf(reply, cap,
                     "Не удалось сохранить напоминание: база недоступна. "
                     "Попробуйте позже.");
        return -1;
    }

    if (reply && cap) snprintf(reply, cap, "%s", plan.reply);
    return 0;
}

int reminder_daily_set(long long chat_id, int enabled, char *reply, size_t cap) {
    if (reply && cap) reply[0] = '\0';

    int st = sb_sub_set(chat_id, enabled);
    if (st < 200 || st > 299) {
        if (reply && cap)
            snprintf(reply, cap,
                     "Не удалось сохранить настройку: база недоступна. "
                     "Попробуйте позже.");
        return -1;
    }

    if (!enabled) {
        /* Снимаем уже разложенные на сегодня напоминания, чтобы не пришли. */
        sb_reminder_cancel_auto(chat_id);
        if (reply && cap)
            snprintf(reply, cap, "Выключил напоминания по графику.");
        return 0;
    }

    if (reply && cap) {
        if (REMINDER_END_LEAD_MIN > 0)
            snprintf(reply, cap,
                     "Включил. Буду напоминать за %d мин до начала смены "
                     "и за %d мин до её конца — по графику.",
                     REMINDER_START_LEAD_MIN, REMINDER_END_LEAD_MIN);
        else
            snprintf(reply, cap,
                     "Включил. Буду напоминать за %d мин до начала смены "
                     "и в момент её окончания — по графику.\n"
                     "Время беру из таблицы смен.",
                     REMINDER_START_LEAD_MIN);
    }
    return 0;
}

int reminder_cancel(long long chat_id) {
    int st = sb_reminder_cancel(chat_id);
    return (st >= 200 && st <= 299) ? 0 : -1;
}

int reminders_materialize(const AppConfig *cfg, const TrackingService *svc,
                          const MonthSchedule *months, int count) {
    (void)cfg;
    if (!months || count <= 0 || !sb_enabled()) return 0;

    long long chats[REMINDER_MAX_SUBS];
    int nc = sb_sub_enabled(chats, REMINDER_MAX_SUBS);
    if (nc <= 0) return 0;

    long long now = (long long)time(NULL);
    int tz = svc ? svc->tz_offset : 0;
    int added = 0;

    for (int i = 0; i < nc; i++) {
        ReminderSlot slots[REMINDER_SLOTS_MAX];
        int ns = reminder_day_slots(now, tz, months, count,
                                    REMINDER_START_LEAD_MIN,
                                    REMINDER_END_LEAD_MIN,
                                    slots, REMINDER_SLOTS_MAX);
        for (int j = 0; j < ns; j++) {
            int st = sb_reminder_add_kind(chats[i], slots[j].kind,
                                          slots[j].at, slots[j].text);
            if (st >= 200 && st <= 299) added++;
        }
    }
    return added;
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

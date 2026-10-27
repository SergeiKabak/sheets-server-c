#include "app/reminder_service.h"

#include "domain/reminder.h"
#include "infra/supabase.h"
#include "infra/telegram_api.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define REMINDER_BATCH 20   /* сколько напоминаний разбираем за один тик */

static int clamp_minutes(int v, int max) {
    if (v < 0) return 0;
    if (v > max) return max;
    return v;
}

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

int reminder_settings_get(long long chat_id, ReminderSettings *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    out->start_lead_min = REMINDER_START_LEAD_DEFAULT;
    out->end_delay_min = REMINDER_END_DELAY_DEFAULT;

    int enabled = 0, lead = REMINDER_START_LEAD_DEFAULT;
    int delay = REMINDER_END_DELAY_DEFAULT;
    int rc = sb_sub_get(chat_id, &enabled, &lead, &delay);
    if (rc < 0) return -1;

    out->enabled = enabled;
    out->start_lead_min = clamp_minutes(lead, REMINDER_LEAD_MAX);
    out->end_delay_min = clamp_minutes(delay, REMINDER_DELAY_MAX);
    return 0;
}

int reminder_settings_save(long long chat_id, const ReminderSettings *s) {
    if (!s) return -1;
    int st = sb_sub_set(chat_id, s->enabled ? 1 : 0,
                        clamp_minutes(s->start_lead_min, REMINDER_LEAD_MAX),
                        clamp_minutes(s->end_delay_min, REMINDER_DELAY_MAX));
    if (st < 200 || st > 299) return -1;

    /* Пересобираем сегодняшние авто-напоминания с новыми настройками
       (при выключении — просто снимаем их, чтобы не пришли). */
    sb_reminder_cancel_auto(chat_id);
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

    SbSub subs[REMINDER_MAX_SUBS];
    int nc = sb_sub_list(subs, REMINDER_MAX_SUBS);
    if (nc <= 0) return 0;

    long long now = (long long)time(NULL);
    int tz = svc ? svc->tz_offset : 0;
    int added = 0;

    for (int i = 0; i < nc; i++) {
        ReminderSlot slots[REMINDER_SLOTS_MAX];
        int ns = reminder_day_slots(now, tz, months, count,
                                    subs[i].start_lead_min,
                                    subs[i].end_delay_min,
                                    slots, REMINDER_SLOTS_MAX);
        for (int j = 0; j < ns; j++) {
            int st = sb_reminder_add_kind(subs[i].chat_id, slots[j].kind,
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

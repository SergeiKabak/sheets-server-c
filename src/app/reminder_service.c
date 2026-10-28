#include "app/reminder_service.h"

#include "app/bot_keyboards.h"
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

/* Отмечен ли день напоминания: 1 — отметки нет (напоминать), 0 — уже
   отмечено (приход и/или уход), -1 — БД недоступна (повторим на следующем
   тике, ничего не помечаем). */
static int no_mark_missing(const TrackingService *svc, long long remind_at) {
    int tz = svc ? svc->tz_offset : 0;
    long long local = remind_at + tz;
    time_t t = (time_t)(local - local % 86400);
    struct tm tmv;
    gmtime_r(&t, &tmv);

    char tab[16];
    snprintf(tab, sizeof(tab), "%02d.%04d", tmv.tm_mon + 1, tmv.tm_year + 1900);

    char t_in[16] = "", t_out[16] = "";
    int rc = sb_time_day_read(tab, tmv.tm_mday, t_in, sizeof(t_in),
                              t_out, sizeof(t_out), NULL, 0, NULL, 0);
    if (rc < 0) return -1;
    if (rc == 1) return 1;               /* строки дня нет — не отмечено */
    return reminder_needs_no_mark(t_in, t_out);
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
                                    REMINDER_NO_MARK_DELAY,
                                    slots, REMINDER_SLOTS_MAX);
        for (int j = 0; j < ns; j++) {
            int st = sb_reminder_add_kind(subs[i].chat_id, slots[j].kind,
                                          slots[j].at, slots[j].text);
            if (st >= 200 && st <= 299) added++;
        }
    }
    return added;
}

int reminders_flush(const AppConfig *cfg, const TrackingService *svc) {
    if (!cfg || !cfg->bot_token || !*cfg->bot_token || !sb_enabled()) return 0;

    SbReminder list[REMINDER_BATCH];
    int n = sb_reminder_due((long long)time(NULL), list, REMINDER_BATCH);
    int sent = 0;

    for (int i = 0; i < n; i++) {
        if (!list[i].text[0]) continue;

        /* «Смена началась, а отметки нет» — проверяем день в БД прямо перед
           отправкой: если пользователь уже отметился, сообщение не нужно. */
        if (strcmp(list[i].kind, "shift_missing") == 0) {
            int missing = no_mark_missing(svc, list[i].remind_at);
            if (missing == 0) {           /* уже отмечено — снимаем молча */
                sb_reminder_mark_sent(list[i].id);
                continue;
            }
            if (missing < 0) continue;    /* БД недоступна — повторим позже */
            telegram_send_text_markup(cfg->bot_token, list[i].chat_id,
                                      list[i].text, TRACK_INLINE_JSON);
            sb_reminder_mark_sent(list[i].id);
            sent++;
            continue;
        }

        telegram_send_text(cfg->bot_token, list[i].chat_id, list[i].text);
        /* Отправка без подтверждения доставки: помечаем, чтобы не зациклиться
           на одном напоминании при сбое сети. */
        sb_reminder_mark_sent(list[i].id);
        sent++;
    }
    return sent;
}

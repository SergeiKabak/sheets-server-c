#include "domain/reminder.h"

#include "core/date_time.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* Начало смены в минутах от полуночи: "7:30-14:40" -> 450.
   Возвращает -1, если время не разобрать. */
static int shift_start_minutes(const char *shift) {
    if (!shift || !*shift) return -1;
    char raw[16];
    size_t n = 0;
    while (shift[n] && shift[n] != '-' && n + 1 < sizeof(raw)) {
        raw[n] = shift[n];
        n++;
    }
    raw[n] = '\0';

    char norm[16];
    time_normalize(raw, norm, sizeof(norm));
    return time_parse(norm);
}

/* Конец смены в минутах от полуночи: "7:30-14:40" -> 880. */
static int shift_end_minutes(const char *shift) {
    if (!shift || !*shift) return -1;
    const char *dash = strchr(shift, '-');
    if (!dash) return -1;

    char raw[16];
    size_t n = 0;
    for (const char *p = dash + 1; *p && n + 1 < sizeof(raw); p++) {
        raw[n++] = *p;
    }
    raw[n] = '\0';

    char norm[16];
    time_normalize(raw, norm, sizeof(norm));
    return time_parse(norm);
}

static const MonthSchedule *find_month(const MonthSchedule *months, int count,
                                        const char *title) {
    if (!months) return NULL;
    for (int i = 0; i < count; i++)
        if (strcmp(months[i].title, title) == 0) return &months[i];
    return NULL;
}

/* Собирает текст о дне и заодно самое раннее начало смены.
   *has_shift — 0/1; *start_min — минуты начала (только при *has_shift). */
static void build_tomorrow_text(const MonthSchedule *m, int day,
                                const char *date_label,
                                char *out, size_t cap,
                                int *has_shift, int *start_min) {
    *has_shift = 0;
    *start_min = -1;

    if (!m) {
        snprintf(out, cap, "Завтра, %s: данные графика пока недоступны.", date_label);
        return;
    }

    char parts[2][192];
    int np = 0;
    for (int row = 0; row < 2; row++) {
        const char *cab = month_schedule_cell(m, row, day + 1);
        if (!cab || !*cab) continue;
        const char *sh = month_schedule_cell(m, row, 1);
        if (sh && *sh)
            snprintf(parts[np], sizeof(parts[0]), "%s (кабинет %s)", sh, cab);
        else
            snprintf(parts[np], sizeof(parts[0]), "кабинет %s", cab);
        np++;

        int st = shift_start_minutes(sh);
        if (st >= 0 && (*start_min < 0 || st < *start_min)) *start_min = st;
    }

    if (np == 0) {
        snprintf(out, cap, "Завтра, %s: смен в графике нет.", date_label);
        return;
    }
    *has_shift = 1;
    if (np == 1)
        snprintf(out, cap, "Завтра, %s: смена %s.", date_label, parts[0]);
    else
        snprintf(out, cap, "Завтра, %s: смены %s и %s.", date_label,
                 parts[0], parts[1]);
}

static void local_hhmm(long long local_epoch, char *out, size_t cap) {
    time_t t = (time_t)local_epoch;
    struct tm tmv;
    gmtime_r(&t, &tmv);
    snprintf(out, cap, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
}

/* Смена на конкретный день: самая ранняя по началу строка с кабинетом. */
typedef struct {
    int  has;
    int  start_min;              /* минуты от полуночи */
    int  end_min;
    char shift[32];
    char cabinet[64];
} DayShift;

static void day_shift(const MonthSchedule *m, int day, DayShift *ds) {
    memset(ds, 0, sizeof(*ds));
    ds->start_min = -1;
    ds->end_min = -1;
    if (!m) return;

    for (int row = 0; row < 2; row++) {
        const char *cab = month_schedule_cell(m, row, day + 1);
        if (!cab || !*cab) continue;
        const char *sh = month_schedule_cell(m, row, 1);
        int st = shift_start_minutes(sh);
        int en = shift_end_minutes(sh);

        if (st >= 0 && (ds->start_min < 0 || st < ds->start_min)) {
            ds->start_min = st;
            snprintf(ds->shift, sizeof(ds->shift), "%s", sh ? sh : "");
            snprintf(ds->cabinet, sizeof(ds->cabinet), "%s", cab);
        }
        if (en >= 0 && en > ds->end_min) ds->end_min = en;
        ds->has = 1;
    }
}

/* Локальные «сейчас», сегодняшняя дата и лист месяца на этот день. */
static const MonthSchedule *today_context(long long now, int tz_offset,
                                          const MonthSchedule *months, int count,
                                          long long *midnight, int *day_out) {
    long long local_now = now + tz_offset;
    long long mid = local_now - (local_now % 86400);
    if (midnight) *midnight = mid;

    time_t t = (time_t)mid;
    struct tm tmv;
    gmtime_r(&t, &tmv);
    int day = tmv.tm_mday, mon = tmv.tm_mon + 1, year = tmv.tm_year + 1900;
    if (day_out) *day_out = day;

    char title[SCHEDULE_MAX_NAME];
    const char *mname = month_name(mon);
    snprintf(title, sizeof(title), "%s %d", mname ? mname : "", year);
    return find_month(months, count, title);
}

int reminder_plan(long long now, int tz_offset,
                  const MonthSchedule *months, int count,
                  const char *choice, ReminderPlan *plan) {
    if (!plan || !choice || !*choice) return -1;
    memset(plan, 0, sizeof(*plan));

    /* Локальное «сейчас» и начало завтрашнего дня. Модель времени в проекте
       линейная (фиксированный tz_offset), поэтому сутки — ровно 86400 с. */
    long long local_now = now + tz_offset;
    long long midnight = local_now - (local_now % 86400);
    long long tomorrow_local = midnight + 86400;

    time_t tmr = (time_t)tomorrow_local;
    struct tm tmrw;
    gmtime_r(&tmr, &tmrw);

    int day = tmrw.tm_mday;
    int mon = tmrw.tm_mon + 1;
    int year = tmrw.tm_year + 1900;

    char date_label[16];
    snprintf(date_label, sizeof(date_label), "%02d.%02d.%04d", day, mon, year);

    const char *mname = month_name(mon);
    char month_title[SCHEDULE_MAX_NAME];
    snprintf(month_title, sizeof(month_title), "%s %d", mname ? mname : "", year);

    const MonthSchedule *m = find_month(months, count, month_title);

    char text[REMINDER_TEXT_MAX];
    int has_shift = 0, start_min = -1;
    build_tomorrow_text(m, day, date_label, text, sizeof(text),
                        &has_shift, &start_min);

    long long remind_local = 0;
    int by_shift = (strcmp(choice, "shift") == 0);

    if (by_shift) {
        if (!m) {
            plan->ok = 1;
            snprintf(plan->reply, sizeof(plan->reply),
                     "Данные графика на завтра ещё не загружены — "
                     "попробуйте через минуту.");
            return 1;
        }
        if (!has_shift || start_min < 0) {
            plan->ok = 1;
            snprintf(plan->reply, sizeof(plan->reply),
                     "Завтра смены нет — напоминание не нужно.\n%s", text);
            return 1;
        }
        remind_local = tomorrow_local + (long long)start_min * 60 - 3600;
    } else {
        int hh = -1;
        if (!strcmp(choice, "08") || !strcmp(choice, "8")) hh = 8;
        else if (!strcmp(choice, "09") || !strcmp(choice, "9")) hh = 9;
        else if (!strcmp(choice, "20")) hh = 20;
        if (hh < 0) {
            plan->ok = 1;
            snprintf(plan->reply, sizeof(plan->reply),
                     "Неизвестное время напоминания.");
            return 1;
        }
        remind_local = tomorrow_local + (long long)hh * 3600;
    }

    long long remind_at = remind_local - tz_offset;
    if (remind_at <= now + 30) {
        plan->ok = 1;
        snprintf(plan->reply, sizeof(plan->reply),
                 "Это время уже прошло — выберите другое.");
        return 1;
    }

    plan->ok = 0;
    plan->remind_at = remind_at;
    local_hhmm(remind_local, plan->when, sizeof(plan->when));
    snprintf(plan->text, sizeof(plan->text), "%s", text);
    if (by_shift)
        snprintf(plan->reply, sizeof(plan->reply),
                 "Ок! Напомню за час до смены — в %s.\n%s", plan->when, text);
    else
        snprintf(plan->reply, sizeof(plan->reply),
                 "Ок! Напомню завтра в %s.\n%s", plan->when, text);
    return 0;
}

int reminder_day_slots(long long now, int tz_offset,
                       const MonthSchedule *months, int count,
                       int start_lead, int end_lead,
                       ReminderSlot *out, int max) {
    if (!out || max <= 0) return -1;
    if (start_lead < 0) start_lead = 0;
    if (end_lead < 0) end_lead = 0;

    long long midnight = 0;
    int day = 0;
    const MonthSchedule *m = today_context(now, tz_offset, months, count,
                                           &midnight, &day);
    if (!m) return 0;

    DayShift ds;
    day_shift(m, day, &ds);
    if (!ds.has || ds.start_min < 0 || ds.end_min < 0) return 0;

    char hhmm_start[8], hhmm_end[8];
    local_hhmm(midnight + (long long)ds.start_min * 60, hhmm_start,
               sizeof(hhmm_start));
    local_hhmm(midnight + (long long)ds.end_min * 60, hhmm_end,
               sizeof(hhmm_end));

    int n = 0;
    long long at_start = midnight + (long long)(ds.start_min - start_lead) * 60;
    long long at_end = midnight + (long long)(ds.end_min - end_lead) * 60;

    if (at_start > now + 30 && n < max) {
        out[n].at = at_start - tz_offset;
        snprintf(out[n].kind, sizeof(out[n].kind), "shift_start");
        if (start_lead > 0)
            snprintf(out[n].text, sizeof(out[n].text),
                     "Через %d мин начало смены: %s–%s, кабинет %s.\n"
                     "Отметьте приход кнопкой «Пришла».",
                     start_lead, hhmm_start, hhmm_end, ds.cabinet);
        else
            snprintf(out[n].text, sizeof(out[n].text),
                     "Смена начинается в %s (кабинет %s).\n"
                     "Отметьте приход кнопкой «Пришла».",
                     hhmm_start, ds.cabinet);
        n++;
    }

    if (at_end > now + 30 && n < max) {
        out[n].at = at_end - tz_offset;
        snprintf(out[n].kind, sizeof(out[n].kind), "shift_end");
        if (end_lead > 0)
            snprintf(out[n].text, sizeof(out[n].text),
                     "Смена заканчивается в %s — через %d мин.\n"
                     "Отметьте уход кнопкой «Ушла».",
                     hhmm_end, end_lead);
        else
            snprintf(out[n].text, sizeof(out[n].text),
                     "Смена закончилась в %s.\n"
                     "Отметьте уход кнопкой «Ушла».", hhmm_end);
        n++;
    }

    return n;
}

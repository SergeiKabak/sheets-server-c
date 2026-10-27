/* Тесты планирования напоминаний (domain/reminder.c): без сети и БД.
   Запуск: make test-reminders */
#define _GNU_SOURCE
#include "domain/reminder.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static int fails = 0;

static void ok(int cond, const char *msg) {
    if (cond) {
        printf("OK   %s\n", msg);
    } else {
        fails++;
        printf("FAIL %s\n", msg);
    }
}

static long long epoch(int y, int mo, int d, int h, int mi) {
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year = y - 1900;
    t.tm_mon = mo - 1;
    t.tm_mday = d;
    t.tm_hour = h;
    t.tm_min = mi;
    t.tm_isdst = 0;
    return (long long)timegm(&t);
}

/* Строка листа: [0] ФИО, [1] смена, [2..] кабинеты по дням, [33] счётчик. */
static char *row1[34];
static char *row2[34];

static MonthSchedule month_with_shift(void) {
    MonthSchedule m;
    memset(&m, 0, sizeof(m));
    snprintf(m.title, sizeof(m.title), "Октябрь 2026");
    row1[0] = "Бинкевич Татьяна Эдуардовна";
    row1[1] = "7:30-14:40";
    row1[29] = "K2.1";          /* 28 октября: индекс дня = day + 1 */
    row1[33] = "6";
    row2[1] = "15:10-21:00";
    m.shift1 = row1; m.shift1_len = 34;
    m.shift2 = row2; m.shift2_len = 34;
    return m;
}

int main(void) {
    /* 27.10.2026 12:00 UTC, пояс UTC */
    long long now = epoch(2026, 10, 27, 12, 0);

    MonthSchedule m = month_with_shift();
    ReminderPlan p;

    /* --- завтра в 09:00 --- */
    memset(&p, 0, sizeof(p));
    int rc = reminder_plan(now, 0, &m, 1, "09", &p);
    ok(rc == 0 && p.ok == 0, "09: план построен");
    ok(p.remind_at == epoch(2026, 10, 28, 9, 0), "09: момент — завтра 09:00 UTC");
    ok(strcmp(p.when, "09:00") == 0, "09: время отправки 09:00");
    ok(strstr(p.text, "28.10.2026") != NULL, "09: в тексте дата завтра");
    ok(strstr(p.text, "7:30-14:40") != NULL, "09: в тексте смена");
    ok(strstr(p.text, "K2.1") != NULL, "09: в тексте кабинет");
    ok(strstr(p.reply, "09:00") != NULL, "09: подтверждение с временем");

    /* --- завтра в 20:00 --- */
    memset(&p, 0, sizeof(p));
    rc = reminder_plan(now, 0, &m, 1, "20", &p);
    ok(rc == 0 && p.remind_at == epoch(2026, 10, 28, 20, 0),
       "20: момент — завтра 20:00");

    /* --- за час до смены 7:30 -> 06:30 --- */
    memset(&p, 0, sizeof(p));
    rc = reminder_plan(now, 0, &m, 1, "shift", &p);
    ok(rc == 0 && p.ok == 0, "shift: план построен");
    ok(p.remind_at == epoch(2026, 10, 28, 6, 30), "shift: за час до смены (06:30)");
    ok(strstr(p.reply, "06:30") != NULL, "shift: подтверждение с 06:30");

    /* --- смена, но не завтра --- */
    MonthSchedule empty = month_with_shift();
    empty.shift1 = row2; empty.shift2 = row2;   /* нет кабинета на 28-е */
    memset(&p, 0, sizeof(p));
    rc = reminder_plan(now, 0, &empty, 1, "shift", &p);
    ok(rc == 1 && p.ok == 1, "shift: без смены завтра — напоминание не ставится");
    ok(strstr(p.reply, "смены нет") != NULL, "shift: причина — смены нет");

    /* фиксированное время всё равно доступно */
    memset(&p, 0, sizeof(p));
    rc = reminder_plan(now, 0, &empty, 1, "09", &p);
    ok(rc == 0 && strstr(p.text, "смен в графике нет") != NULL,
       "09: без смены текст «смен в графике нет»");

    /* --- график ещё не загружен --- */
    memset(&p, 0, sizeof(p));
    rc = reminder_plan(now, 0, NULL, 0, "shift", &p);
    ok(rc == 1 && strstr(p.reply, "не загружены") != NULL,
       "shift: нет кэша графика — просим попробовать позже");
    memset(&p, 0, sizeof(p));
    rc = reminder_plan(now, 0, NULL, 0, "09", &p);
    ok(rc == 0 && strstr(p.text, "недоступны") != NULL,
       "09: нет кэша — текст «данные недоступны»");

    /* --- часовой пояс UTC+3: локальное «сейчас» уже 28-е --- */
    long long now_tz = epoch(2026, 10, 27, 21, 30);   /* 28.10 00:30 по UTC+3 */
    memset(&p, 0, sizeof(p));
    rc = reminder_plan(now_tz, 3 * 3600, &m, 1, "09", &p);
    ok(rc == 0 && p.remind_at == epoch(2026, 10, 29, 6, 0),
       "tz+3: завтра = 29.10, 09:00 локально = 06:00 UTC");
    ok(strstr(p.text, "29.10.2026") != NULL, "tz+3: дата в тексте 29.10");

    /* --- неизвестный выбор --- */
    memset(&p, 0, sizeof(p));
    rc = reminder_plan(now, 0, &m, 1, "xx", &p);
    ok(rc == 1 && strstr(p.reply, "Неизвестное") != NULL,
       "неизвестный выбор отклонён");

    if (fails) printf("\nFAILURES: %d\n", fails);
    else printf("\nALL TESTS PASSED\n");
    return fails ? 1 : 0;
}

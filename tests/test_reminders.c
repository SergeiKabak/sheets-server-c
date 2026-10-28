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
    row1[28] = "K2.1";          /* 27 октября (сегодня): индекс = day + 1 */
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

    /* ================= напоминания по графику (сегодня) ================= */

    ReminderSlot slots[REMINDER_SLOTS_MAX];

    /* 27.10 06:00 — смена 7:30-14:40:
       7:15 начало, 8:00..14:00 приход каждые 30 мин,
       14:10 «скоро конец», 14:40..17:10 уход каждые 30 мин. */
    long long morning = epoch(2026, 10, 27, 6, 0);
    int ns = reminder_day_slots(morning, 0, &m, 1, 15, 0, 30, slots,
                                REMINDER_SLOTS_MAX);
    ok(ns == 21, "day: 21 напоминание (начало, 13 приход, 7 уход)");
    ok(ns == 21 && slots[0].at == epoch(2026, 10, 27, 7, 15),
       "day: начало — за 15 мин (07:15)");
    ok(ns == 21 && strcmp(slots[0].kind, "shift_start") == 0,
       "day: вид первого — shift_start");
    ok(ns == 21 && strstr(slots[0].text, "Пришла") != NULL,
       "day: текст просит нажать «Пришла»");
    ok(ns == 21 && strstr(slots[0].text, "15 мин") != NULL,
       "day: в тексте «через 15 мин»");

    ok(ns == 21 && slots[1].at == epoch(2026, 10, 27, 8, 0),
       "day: первый контроль прихода — 08:00 (через 30 мин)");
    ok(ns == 21 && strcmp(slots[1].kind, "shift_missing") == 0,
       "day: вид второго — shift_missing");
    ok(ns == 21 && strstr(slots[1].text, "отметки прихода нет") != NULL,
       "day: текст про отсутствие отметки");
    ok(ns == 21 && strstr(slots[1].text, "Пришла") != NULL &&
       strstr(slots[1].text, "Ушла") != NULL,
       "day: текст предлагает и начало, и конец");
    ok(ns == 21 && strstr(slots[1].text, "ещё раз") == NULL,
       "day: первое напоминание — без «ещё раз»");

    ok(ns == 21 && slots[2].at == epoch(2026, 10, 27, 8, 30) &&
       strstr(slots[2].text, "ещё раз") != NULL,
       "day: повтор прихода — 08:30 с «напоминаю ещё раз»");
    ok(ns == 21 && slots[13].at == epoch(2026, 10, 27, 14, 0),
       "day: последний приход — 14:00 (за 30 мин до конца)");

    ok(ns == 21 && slots[14].at == epoch(2026, 10, 27, 14, 10) &&
       strcmp(slots[14].kind, "shift_end") == 0 &&
       strstr(slots[14].text, "Не забудьте") != NULL,
       "day: за 30 мин до конца — «не забудьте про уход»");
    ok(ns == 21 && slots[15].at == epoch(2026, 10, 27, 14, 40),
       "day: конец смены — 14:40");
    ok(ns == 21 && strstr(slots[15].text, "Ушла") != NULL,
       "day: текст просит нажать «Ушла»");
    ok(ns == 21 && slots[20].at == epoch(2026, 10, 27, 17, 10) &&
       strstr(slots[20].text, "ещё раз") != NULL,
       "day: последний уход — 17:10 (3 часа после конца)");

    /* 27.10 07:20 — начало прошло, первый контроль прихода ещё впереди */
    ns = reminder_day_slots(epoch(2026, 10, 27, 7, 20), 0, &m, 1, 15, 0, 30,
                            slots, REMINDER_SLOTS_MAX);
    ok(ns == 20 && slots[0].at == epoch(2026, 10, 27, 8, 0),
       "day: прошедшее начало пропущено, приход остался");
    ok(ns == 20 && strcmp(slots[0].kind, "shift_missing") == 0,
       "day: первым идёт shift_missing");

    /* 27.10 14:30 — приход уже не напоминаем, уход ещё впереди */
    ns = reminder_day_slots(epoch(2026, 10, 27, 14, 30), 0, &m, 1, 15, 0, 30,
                            slots, REMINDER_SLOTS_MAX);
    ok(ns == 6 && slots[0].at == epoch(2026, 10, 27, 14, 40),
       "day: после 14:10 остаются только напоминания об уходе");

    /* 27.10 18:00 — всё позади */
    ns = reminder_day_slots(epoch(2026, 10, 27, 18, 0), 0, &m, 1, 15, 0, 30,
                            slots, REMINDER_SLOTS_MAX);
    ok(ns == 0, "day: после смены — пусто");

    /* задержка после конца смены: первый уход — через 10 минут (14:50) */
    ns = reminder_day_slots(morning, 0, &m, 1, 15, 10, 30, slots,
                            REMINDER_SLOTS_MAX);
    ok(ns == 21 && slots[15].at == epoch(2026, 10, 27, 14, 50),
       "day: уход — через 10 мин после конца (14:50)");
    ok(ns == 21 && strstr(slots[15].text, "10 мин назад") != NULL,
       "day: в тексте «10 мин назад»");

    /* шаг меньше 30 минут не принимаем (защита от потока сообщений) */
    ns = reminder_day_slots(morning, 0, &m, 1, 15, 0, 5, slots,
                            REMINDER_SLOTS_MAX);
    ok(ns == 21 && slots[1].at == epoch(2026, 10, 27, 8, 0),
       "day: repeat_min < 30 поднимается до 30");

    /* пояс UTC+3: 06:30 локально = 03:30 UTC */
    ns = reminder_day_slots(epoch(2026, 10, 27, 3, 30), 3 * 3600, &m, 1, 15, 0, 30,
                            slots, REMINDER_SLOTS_MAX);
    ok(ns == 21 && slots[0].at == epoch(2026, 10, 27, 4, 15),
       "day tz+3: 07:15 локально = 04:15 UTC");
    ok(ns == 21 && slots[1].at == epoch(2026, 10, 27, 5, 0),
       "day tz+3: 08:00 локально = 05:00 UTC");

    /* пояс UTC+3: 12:00 локально — прошедшие слоты не создаются
       (иначе на UTC+3 в «прошлом» оказывались бы ещё 3 часа цепочки) */
    ns = reminder_day_slots(epoch(2026, 10, 27, 9, 0), 3 * 3600, &m, 1, 15, 0, 30,
                            slots, REMINDER_SLOTS_MAX);
    ok(ns == 11 && slots[0].at == epoch(2026, 10, 27, 9, 30) &&
       strcmp(slots[0].kind, "shift_missing") == 0,
       "day tz+3: в 12:00 локально первый слот — 12:30 (09:30 UTC)");

    /* без графика — тихо ничего не планируем */
    ns = reminder_day_slots(morning, 0, NULL, 0, 15, 0, 30, slots,
                            REMINDER_SLOTS_MAX);
    ok(ns == 0, "day: нет кэша графика — пусто");

    /* смена есть, но кабинета на сегодня нет */
    MonthSchedule off = month_with_shift();
    off.shift1 = row2; off.shift2 = row2;
    ns = reminder_day_slots(morning, 0, &off, 1, 15, 0, 30, slots,
                            REMINDER_SLOTS_MAX);
    ok(ns == 0, "day: сегодня смены нет — пусто");

    /* раньше 08:00 не будим: смена 6:00-12:00 — первый контроль в 08:00 */
    char *saved_shift = row1[1];
    row1[1] = "6:00-12:00";
    ns = reminder_day_slots(epoch(2026, 10, 27, 5, 0), 0, &m, 1, 0, 0, 30,
                            slots, REMINDER_SLOTS_MAX);
    ok(ns == 15 && slots[0].at == epoch(2026, 10, 27, 6, 0) &&
       strcmp(slots[0].kind, "shift_start") == 0,
       "day: смена с 6:00 — начало в 06:00");
    ok(ns == 15 && slots[1].at == epoch(2026, 10, 27, 8, 0),
       "day: первый контроль прихода — не раньше 08:00");
    row1[1] = saved_shift;

    /* две смены в один день: 7:30-14:40 и 15:10-21:00 — 33 напоминания,
       все влезают в REMINDER_SLOTS_MAX */
    row2[28] = "K2.1";
    ns = reminder_day_slots(morning, 0, &m, 1, 15, 0, 30, slots,
                            REMINDER_SLOTS_MAX);
    ok(ns == 33, "day: две смены — 33 напоминания (приход до 20:00)");
    ok(ns == 33 && slots[ns - 1].at == epoch(2026, 10, 27, 23, 30),
       "day: последнее напоминание — 23:30 (не за полночь)");
    row2[28] = NULL;

    /* решение «напоминать или нет» по отметкам дня */
    ok(reminder_needs_no_mark("", "") == 1, "no-mark: пустой день — напоминать");
    ok(reminder_needs_no_mark(NULL, NULL) == 1, "no-mark: NULL — напоминать");
    ok(reminder_needs_no_mark("07:30", "") == 0, "no-mark: приход есть — молчим");
    ok(reminder_needs_no_mark("", "14:40") == 0, "no-mark: уход есть — молчим");
    ok(reminder_needs_no_mark("07:30", "14:40") == 0,
       "no-mark: полный день — молчим");

    ok(reminder_mark_present("07:30", "", REMINDER_MARK_IN) == 1,
       "mark: приход есть — для прихода хватит");
    ok(reminder_mark_present("07:30", "", REMINDER_MARK_OUT) == 0,
       "mark: приход есть, но для ухода этого мало");
    ok(reminder_mark_present("", "14:40", REMINDER_MARK_OUT) == 1,
       "mark: уход есть — для ухода хватит");
    ok(reminder_mark_present("", "14:40", REMINDER_MARK_IN) == 0,
       "mark: уход есть, но прихода нет");
    ok(reminder_mark_present(NULL, NULL,
                             REMINDER_MARK_IN | REMINDER_MARK_OUT) == 0,
       "mark: пустой день — ничего нет");
    ok(reminder_mark_present("07:30", "",
                             REMINDER_MARK_IN | REMINDER_MARK_OUT) == 1,
       "mark: любая отметка закрывает напоминание о приходе");

    if (fails) printf("\nFAILURES: %d\n", fails);
    else printf("\nALL TESTS PASSED\n");
    return fails ? 1 : 0;
}

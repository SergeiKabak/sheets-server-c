#include "app/background.h"

#include "app/month_cache.h"
#include "app/reminder_service.h"
#include "app/schedule_service.h"
#include "app/tracking_cache.h"
#include "domain/schedule.h"
#include "infra/clock.h"
#include "infra/heartbeat.h"
#include "infra/supabase.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Листы графика читаются раз в GOOGLE_SYNC_TTL (30 минут, см. app/config.h)
 * и только в рабочее окно GOOGLE_SYNC_HOUR_FROM..TO (07:00–21:00 локально):
 * люди правят график редко и ночью, а каждый обход — это запрос метаданных
 * + запрос на каждый лист. Между обходами живём на /tmp/months.dat и таблице
 * schedule. Проверка идёт на тике цикла (~60 с). */
/* ================================================================
 *  Этап 1 плана Supabase: строки самого сотрудника в буфер
 * ================================================================ */
/* Строка листа: [0]=имя, [1]=смена, [2..]=кабинеты по дням,
   позиция n-1 (n = max длины строк) — ячейка-счётчик «Всего смен».
   Ровно по этой логике Mini App считает дни: days = n - 3.
   limit — индекс счётчика (не включительно). */
static void sb_add_row_cells(const char *month, const char *fallback_name,
                             char **cells, int len, int limit) {
    if (!cells || len < 3) return;
    const char *name = (cells[0] && cells[0][0]) ? cells[0] : fallback_name;
    if (!name || !name[0]) return;
    const char *shift = cells[1] ? cells[1] : "";
    int last = len < limit ? len : limit;
    for (int d = 2; d < last; d++)
        if (cells[d] && cells[d][0])
            sb_row(month, d - 1, name, shift, cells[d]);
}

static void sb_add_month_rows(const MonthSchedule *m, const char *fallback_name) {
    /* счётчик — по позиции max(длин строк): хвостовые пустые ячейки
       одной строки не должны «съедать» последний день другой */
    int n = m->shift1_len > m->shift2_len ? m->shift1_len : m->shift2_len;
    sb_add_row_cells(m->title, fallback_name, m->shift1, m->shift1_len, n - 1);
    sb_add_row_cells(m->title, fallback_name, m->shift2, m->shift2_len, n - 1);
}

/* ================================================================
 *  Фоновое обновление кэшей
 * ================================================================ */
void background_run(AppContext *app) {
    if (!app) return;
    MonthSchedule months[SCHEDULE_MAX_MONTHS];
    time_t last_google = 0;   /* когда последний раз читали листы Google */

    /* Расписания «дорогих» дел. Тик остаётся раз в минуту (нужен, чтобы
     * вовремя поймать окно Google и срок напоминания), но каждый тик больше
     * не ходит в сеть: у каждого дела — свой интервал, см. app/background.h. */
    time_t last_tracking = 0;             /* кэш отметок: TRK_REFRESH_TTL */
    time_t last_plan = 0;                 /* раскладка напоминаний */
    time_t last_menu = time(NULL);        /* кнопку меню ставит main.c при старте */

    for (;;) {
        int sync_rows = -1;   /* строк буфера в текущем обходе (-1 — нет данных) */
        int sync_errs = 0;
        memset(months, 0, sizeof(months));
        int c = 0;

        /* График из Google читаем не каждый тик, а раз в GOOGLE_SYNC_TTL
         * (час) и только в рабочее окно: ночью (21:00–07:00) листы
         * никто не правит — опрос выключен, трафик и квота не тратятся.
         * Между чтениями живём на кэше. */
        int google_hours = clock_in_hour_window(app->tracking.tz_offset,
                                                GOOGLE_SYNC_HOUR_FROM,
                                                GOOGLE_SYNC_HOUR_TO);
        if (google_hours && time(NULL) - last_google >= GOOGLE_SYNC_TTL) {
            last_google = time(NULL);
            sb_begin_cycle();   /* updated_at текущего цикла — ДО сбора строк */

            FILE *idx = fopen(cabinet_index_path(), "w");
            c = schedule_build_months(&app->cfg, months, SCHEDULE_MAX_MONTHS, idx);
            if (idx) { fclose(idx); fprintf(stderr, "[cabinet] index saved\n"); }
            month_cache_save(months, c);
            fprintf(stderr, "[updater] months from google: %d\n", c);

            /* «Форма» месяцев (ширина строки + счётчик) — в БД: без неё
               календарь не построить, когда /tmp пуст, а Google молчит. */
            if (sb_enabled() && c > 0)
                schedule_month_meta_flush(months, c);

            /* Буфер графика в Supabase: upsert строк месяца + уборка устаревших */
            if (sb_enabled()) {
                const char *titles[SCHEDULE_MAX_MONTHS];
                int nt = 0;
                for (int i = 0; i < c && nt < SCHEDULE_MAX_MONTHS; i++) {
                    titles[nt++] = months[i].title;
                    sb_add_month_rows(&months[i], app->cfg.search_name);
                }
                int errs = 0;
                int rows = sb_flush(titles, nt, &errs);
                fprintf(stderr, "[sync] supabase schedule: rows=%d errors=%d\n", rows, errs);
                sync_rows = rows;
                sync_errs = errs;
                /* В sync_log пишем ТОЛЬКО ошибки: успешный обход виден по
                   метке /tmp/last_cycle (её показывает /diag/supabase), а
                   строка в БД на каждый обход — это запрос в сеть ради
                   одного числа (см. docs/OPTIMIZE_BANDWIDTH.md). */
                if (errs)
                    sb_log("schedule", "", rows, 0,
                           "HTTP error при синхронизации графика");
            }

            /* Пульс «фон жив»: локальный файл, без сети и без БД. */
            heartbeat_write((long long)time(NULL), sync_rows, sync_errs);
        } else {
            c = month_cache_load(months, SCHEDULE_MAX_MONTHS);
        }

        /* Ни файлового кэша, ни ответа Google (свежий контейнер / сбой
           квоты) — структура месяцев из БД: календарь, напоминания и
           отметки продолжают работать. */
        if (c <= 0) {
            c = schedule_months_from_db(months, SCHEDULE_MAX_MONTHS);
            if (c > 0) fprintf(stderr, "[updater] months from db: %d\n", c);
        }

        /* Тот же цикл обновляет и кэш отметок Time tracking, чтобы Mini App
           не ждал БД/Google при открытии месяца. Обновление — раз в
           TRK_REFRESH_TTL, а не на каждом тике: отметки месяца в БД меняются
           только при записи, и такая запись сама инвалидирует кэш. */
        if (c > 0 && time(NULL) - last_tracking >= TRK_REFRESH_TTL) {
            last_tracking = time(NULL);
            const char *labels[SCHEDULE_MAX_MONTHS];
            int lc = 0;
            for (int i = 0; i < c && i < SCHEDULE_MAX_MONTHS; i++)
                labels[lc++] = months[i].title;
            int tn = tracking_cache_refresh(&app->tracking, labels, lc);
            if (tn >= 0) fprintf(stderr, "[updater] tracking cache months: %d\n", tn);
        }

        /* Выгрузка БД -> Google (Google = архив для людей):
           - каждый цикл: месяцы, помеченные dirty_put() после записей в БД;
           - раз в сутки: ВСЕ месяцы (подхватывает ручные правки дашборда
             в Supabase) + освежение кэша коллег.
           Ночной restore time_entries из Google УДАЛЁН: БД — первоисточник,
           зеркалирование из Google затёрло бы правки, сделанные в БД. */
        if (sb_enabled() && c > 0) {
            tracking_push_pending(&app->tracking);

            static time_t last_daily = 0;
            if (time(NULL) - last_daily >= DAY_SYNC_TTL) {
                last_daily = time(NULL);

                int col_n = 0;
                for (int i2 = 0; i2 < c; i2++)
                    if (schedule_refresh_colleagues_cache(months[i2].title) == 0) col_n++;
                fprintf(stderr, "[updater] month colleagues cached: %d\n", col_n);

                const char *titles[SCHEDULE_MAX_MONTHS];
                int nt = 0;
                for (int i2 = 0; i2 < c && nt < SCHEDULE_MAX_MONTHS; i2++)
                    titles[nt++] = months[i2].title;
                int pushed = tracking_push_all(&app->tracking, titles, nt);
                fprintf(stderr, "[sync] google push months: %d\n", pushed);

                /* Журнал напоминаний: отправленные строки старше 3 дней
                   больше не нужны — иначе таблица растёт без предела. */
                sb_reminder_purge_sent(time(NULL) - 3 * 86400);

                /* sync_log: теперь только ошибки, но они могут копиться
                   месяцами — строки старше недели удаляем раз в сутки. */
                sb_sync_log_purge(time(NULL) - 7 * 86400);
            }
        }

        /* Напоминания по графику: раскладываем сегодняшние (начало/конец
           смены) для подписанных чатов, пока кэш месяцев ещё в памяти.
           Раз в REMINDER_PLAN_TTL: слоты строятся на весь день вперёд, а не
           только на ближайшую минуту, поэтому пропусков нет. */
        if (c > 0 && time(NULL) - last_plan >= REMINDER_PLAN_TTL) {
            last_plan = time(NULL);
            int mat = reminders_materialize(&app->cfg, &app->tracking, months, c);
            if (mat > 0) {
                fprintf(stderr, "[remind] planned: %d\n", mat);
                reminders_wake();   /* есть новое — не ждём паузы отправки */
            }
        }

        month_schedules_free(months, c);

        /* Напоминания бота: отправляем созревшие (раз в минуту). */
        int reminded = reminders_flush(&app->cfg, &app->tracking);
        if (reminded > 0) fprintf(stderr, "[remind] sent: %d\n", reminded);

        /* Кнопка меню чата (доступ к графику-таблице) ставится раз в сутки:
           если запрос не прошёл, доступ к таблице восстановится следующим
           разом. Каждый тик здесь был лишним POST в Telegram. */
        if (time(NULL) - last_menu >= MENU_BUTTON_TTL) {
            last_menu = time(NULL);
            bot_set_menu_button(&app->bot);
        }
        sleep(CACHE_TTL);
    }
}

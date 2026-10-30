/* infra/supabase.h — буфер графика и учёта времени в Supabase (PostgREST).
 *
 * Идея: фоновая синхронизация читает график из Google Sheets и складывает
 * строки в таблицы schedule / time_entries, чтобы читать данные из SQL
 * мгновенно и хранить их вне сроков жизни бесплатных планов.
 *
 * Реализация — через curl-слой (infra/http_client), без libpq.
 * Управление: SUPABASE_URL + SUPABASE_SECRET_KEY включают синхронизацию,
 * SUPABASE_SYNC=0 — выключает принудительно.
 *
 * Модуль отвечает только за transport и накопление буфера. Разбор данных
 * живёт в слое domain (чистые функции).
 */
#ifndef SHEETS_INFRA_SUPABASE_H
#define SHEETS_INFRA_SUPABASE_H

#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Чтение env, вычисление базового URL. Вызывать один раз при старте. */
void sb_init(void);

/* 1, если настроена и не отключена синхронизация */
int  sb_enabled(void);

/* Фиксирует метку времени текущего цикла (ISO-8601 UTC).
   Вызывать ДО сбора строк: она попадёт в updated_at каждой строки,
   а затем — в DELETE устаревших записей. */
void sb_begin_cycle(void);

/* Одна строка графика. Накапливается в памяти до sb_flush().
   employee — с учётом наследования имени пустой строки листа. */
void sb_row(const char *month, int day, const char *employee,
            const char *shift, const char *cabinet);

/* Отправка накопленного (upsert) + удаление устаревших строк по месяцам
   из [months..months+nmonths) + очистка месяцев, пропавших из Google.
   Возвращает число отправленных строк; *errors (не NULL) — кол-во
   неудачных HTTP-вызовов. */
int  sb_flush(const char *const *months, int nmonths, int *errors);

/* Строка журнала sync_log (fire-and-forget): пульс и ошибки синхронизации */
void sb_log(const char *kind, const char *month, int rows, int ok, const char *error);

/* Содержимое буфера по месяцу — для диагностики и тестов (JSON-массив). */
const char *sb_pending(const char *month, int *count);

/* GET-запрос к PostgREST (path вида "/schedule?...").
   Возвращает тело (освобождает caller) или NULL; *status — HTTP-код. */
char *sb_get(const char *path, int *status);

/* UPSERT дня в time_entries (merge-duplicates по PK month+day).
   hours/sum — строки «7.17»/«3585.00» или NULL/"" (частичная запись).
   Возвращает HTTP-код (2xx — успех), 0 — выключено/сетевая ошибка. */
int sb_time_upsert(const char *month_tab, int day,
                   const char *in_t, const char *out_t,
                   const char *hours, const char *sum);

/* Удаление дня (для «Удалить время» в Mini App): DELETE +day.
   204 — успех (удаление отсутствующего дня тоже успех), 0 — выключено
   или сетевая ошибка, иначе HTTP-код. */
int sb_time_day_delete(const char *month_tab, int day);

/* Чтение одного дня для записи (fill/clock): GET ...&month=eq.X&day=eq.N.
   0 — день есть (строки скопированы; пустые означают незаполненный день),
   1 — дня нет, -1 — ошибка БД (вызывающая сторона уходит в фоллбэк). */
int sb_time_day_read(const char *month_tab, int day,
                     char *in, size_t incap,
                     char *out, size_t outcap,
                     char *hours, size_t hcap,
                     char *sum, size_t scap);

/* Ставка за час на месяц (таблица month_rates).
   0 — ставка есть (в *rate), 1 — для месяца ставка не задана,
   -1 — БД недоступна/ошибка. */
int sb_month_rate_get(const char *month_tab, double *rate);

/* Сохраняет ставку месяца (upsert по month). Возвращает HTTP-код
   (2xx — успех), 0 — выключено/сетевая ошибка/неверные аргументы. */
int sb_month_rate_set(const char *month_tab, const char *rate);

/* Пересчёт суммы одного дня при смене ставки месяца:
   PATCH amount для строки (month, day). HTTP-код, 0 — ошибка. */
int sb_time_amount_set(const char *month_tab, int day, const char *amount);

/* ---------- напоминания бота ---------- */

#define SB_REMINDER_TEXT 512

typedef struct {
    long long id;
    long long chat_id;
    long long remind_at;                   /* UTC, секунды с эпохи */
    char      kind[24];                    /* "once" | "shift_start" | ... */
    char      text[SB_REMINDER_TEXT];
} SbReminder;

/* Добавляет напоминание на момент remind_at (UTC, секунды с эпохи).
   kind: "once" | "shift_start" | "shift_missing" | "shift_end"
   (для авто-напоминаний).
   Возвращает HTTP-код (2xx — успех), 0 — выключено/сетевая ошибка. */
int sb_reminder_add(long long chat_id, long long remind_at, const char *text);
int sb_reminder_add_kind(long long chat_id, const char *kind, long long remind_at,
                         const char *text);

/* Забирает созревшие (remind_at <= now, sent=false) напоминания.
   Возвращает число записей (0 — нет или ошибка). */
int sb_reminder_due(long long now, SbReminder *out, int max);

/* Моменты уже запланированных напоминаний чата с remind_at >= from
   (отправленные тоже — чтобы не вставлять их заново).
   Возвращает число записей (0 — нет или ошибка). */
int sb_reminder_times(long long chat_id, long long from, long long *out, int max);

/* Помечает напоминание отправленным. HTTP-код, 0 — выключено/ошибка. */
int sb_reminder_mark_sent(long long id);

/* Удаляет неотправленные напоминания чата (кнопка «Отменить»).
   HTTP-код, 0 — выключено/ошибка. */
int sb_reminder_cancel(long long chat_id);

/* Удаляет неотправленные авто-напоминания (начало/конец смены). */
int sb_reminder_cancel_auto(long long chat_id);

/* ---------- подписка «напоминать по графику» ---------- */

typedef struct {
    long long chat_id;
    int       start_lead_min;   /* за сколько минут до начала смены */
    int       end_delay_min;    /* через сколько минут после конца */
} SbSub;

/* Сохраняет настройки чата (upsert по chat_id). HTTP-код, 0 — выключено. */
int sb_sub_set(long long chat_id, int enabled, int start_lead_min,
               int end_delay_min);

/* Читает настройки чата. 0 — найдены, 1 — записи нет (значения по
   умолчанию: выключено, 15 и 0), -1 — ошибка/БД недоступна. */
int sb_sub_get(long long chat_id, int *enabled, int *start_lead_min,
               int *end_delay_min);

/* Заполняет out подписками с включённым режимом.
   Возвращает число чатов (0 — нет или ошибка). */
int sb_sub_list(SbSub *out, int max);

/* ---------- структура листа месяца (month_meta) ---------- */

/* Ширина строки листа (дни + 3 служебные колонки) и счётчик «Всего смен».
   Нужна, чтобы построить месяц без Google: на холодном старте /tmp пуст,
   а календарю всё равно нужны длина строки и значение счётчика. */
typedef struct {
    char month[32];
    int  cols;
    char total_shifts[32];
} SbMonthMeta;

/* Upsert структуры месяцев одним запросом (пишет фоновый цикл).
   HTTP-код, 0 — выключено. */
int sb_month_meta_flush(const SbMonthMeta *list, int n);

/* Структура одного месяца. 0 — найдена, 1 — записи нет, -1 — ошибка. */
int sb_month_meta_get(const char *month, SbMonthMeta *out);

/* Все месяцы структуры, по возрастанию названия. Возвращает число (0 — нет). */
int sb_month_meta_list(SbMonthMeta *out, int max);

/* Диагностика для GET /diag/supabase (ручной мониторинг между этапами):
   точные счётчики schedule (всего/валидных/служебных), построчно по месяцам,
   time_entries, последняя запись и последний restore из sync_log с возрастом
   (пульс). months/nmonths — месяцы из файлового кэша.
   Возвращает 0, -1 если общий счёт не получен или sync выключен. */
int sb_diag(const char *const *months, int nmonths, char *out, size_t cap);

/* Разбирает ISO-8601 из PostgREST ("2026-10-28T20:57:17.85+03:00") в
   unix-время с учётом пояса; 0 — строка не разобрана.
   Экспортируется ради офлайн-теста (tests/test_supabase.c): без учёта
   смещения age_sec в /diag/supabase уходил в минус. */
time_t sb_parse_iso(const char *s);

#ifdef __cplusplus
}
#endif

#endif /* SHEETS_INFRA_SUPABASE_H */

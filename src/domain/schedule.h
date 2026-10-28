/* domain/schedule.h — модель графика смен (расписание).
 *
 * График — это таблица Google Sheets, где каждый лист = месяц, а строки
 * описывают сотрудников и их кабинеты по дням:
 *   A: ФИО | B: смена | C..: кабинет по дню | последняя ячейка: «Всего смен»
 *
 * Здесь — чистые преобразования: разбор строк из БД в формат строки листа
 * и список коллег по кабинету.
 */
#ifndef SHEETS_DOMAIN_SCHEDULE_H
#define SHEETS_DOMAIN_SCHEDULE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCHEDULE_MAX_NAME  128
#define SCHEDULE_MAX_MONTHS 50
#define COLLEAGUE_MAX      64

/* ---------- месяц-лист ---------- */

typedef struct {
    char   title[SCHEDULE_MAX_NAME];   /* название листа, например "Октябрь 2026" */
    char **shift1;                     /* утренняя строка сотрудника */
    int    shift1_len;
    char **shift2;                     /* вечерняя строка сотрудника */
    int    shift2_len;
} MonthSchedule;

void  month_schedule_free(MonthSchedule *m);
void  month_schedules_free(MonthSchedule *months, int count);
char **month_schedule_copy_row(char **src, int len);

/* Длина самой длинной строки месяца (число столбцов листа). */
int   month_schedule_width(const MonthSchedule *m);

/* Ячейка строки листа: row 0 — утро, row 1 — вечер. */
const char *month_schedule_cell(const MonthSchedule *m, int row, int col);

/* Месяц-заглушка по структуре листа (ширина строки cols, счётчик «Всего
 * смен» — в последней ячейке утренней строки). Нужен, когда самого листа
 * нет под рукой: на холодном старте календарь строится из БД, а длину
 * строки и счётчик даёт таблица month_meta. Возвращает 0 или -1. */
int   month_schedule_init_stub(MonthSchedule *m, const char *title, int cols,
                               const char *total);

/* ---------- строки месяца из БД ---------- */

/* Строит JSON-массивы s1/s2 месяца Mini App из строк БД (PostgREST).
 * rows_json     — ответ /schedule?select=day,shift_time,cabinet&...
 * employee      — ФИО сотрудника
 * sheet_days    — число столбцов-дней листа: счётчик «Всего смен» должен
 *                 стоять после последнего дня, а не в ячейке дня
 * total         — значение счётчика из листа; NULL — запасной подсчёт,
 *                 "" — в листе пусто
 * Возвращает 0 при успехе, -1 если данных нет либо JSON не разобран. */
int schedule_month_arrays(const char *rows_json, const char *employee,
                          int sheet_days, const char *total,
                          char *s1, size_t s1cap, char *s2, size_t s2cap);

/* ---------- коллеги ---------- */

typedef struct {
    char name[256];
    char shift[128];
} Colleague;

typedef struct {
    Colleague items[COLLEAGUE_MAX];
    int        count;
} ColleagueList;

/* Предикат пересечения смен (передаётся вызывающей стороной). */
typedef int (*ShiftFilter)(const char *row_shift, const char *query);

void colleague_list_init(ColleagueList *list);

/* Добавляет коллегу, если такой пары (имя+смена) ещё нет.
 * Возвращает 0 если добавлен, 1 если был дубль, -1 при переполнении. */
int  colleague_list_add(ColleagueList *list, const char *name, const char *shift);

/* Печатает список в JSON: [{"name":"..","shift":".."}].
 * Возвращает длину JSON либо -1 при переполнении. */
int  colleague_list_render_json(const ColleagueList *list, char *out, size_t outcap);

/* Собирает коллег из строк БД ({"employee":..,"shift_time":..}).
 * self   — ФИО сотрудника, которого исключаем из списка
 * shift  — смена-фильтр (NULL — без фильтра)
 * filter — предикат пересечения смен (NULL — без фильтра)
 * Возвращает 0 при успехе, -1 если JSON не разобран. */
int  colleagues_from_rows_json(const char *rows_json, const char *self,
                               const char *shift, ShiftFilter filter,
                               ColleagueList *out);

#ifdef __cplusplus
}
#endif

#endif /* SHEETS_DOMAIN_SCHEDULE_H */

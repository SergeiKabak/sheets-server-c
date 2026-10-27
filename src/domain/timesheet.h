/* domain/timesheet.h — модель листа учёта времени ("Time tracking").
 *
 * Лист состоит из строк по 6 ячеек (A..F):
 *   A: Дата | B: Время прихода | C: Время ухода
 *   D: Время в часах | E: Ставка в час | F: Сумма за день
 * Блоки разных сотрудников разделены пустой строкой.
 *
 * Все функции этого модуля — чистые: работают с текстом JSON и структурой
 * данных, сетевых вызовов не делают.
 */
#ifndef SHEETS_DOMAIN_TIMESHEET_H
#define SHEETS_DOMAIN_TIMESHEET_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHEET_COLS 6

/* Заголовки колонок листа (используются при создании вкладки). */
extern const char *const SHEET_HEADERS[SHEET_COLS];

typedef struct {
    char *cell[SHEET_COLS];   /* NULL == пустая ячейка */
} SheetRow;

typedef struct {
    SheetRow *row;
    int       count;
    int       cap;
} SheetRows;

void sheet_rows_init(SheetRows *rows);
void sheet_rows_free(SheetRows *rows);

/* Разбирает ответ Google Sheets ("values":[[...],[...]]) в строки листа.
 * Возвращает число разобранных строк. */
int sheet_rows_parse_values_json(SheetRows *rows, const char *json);

/* Готовит строки к записи: строки с датами внутри блоков сортируются
 * по дате (строки-разделители и заголовки остаются на месте). */
void sheet_rows_sort_dates(SheetRows *rows);

/* Тело запроса values.update: {"values":[[...],[...]]}.
 * Возвращает malloc-ную строку либо NULL. */
char *sheet_rows_to_body_json(const SheetRows *rows);

/* Значения одной строки в виде JSON-массива: ["a","b",...].
 * Возвращает malloc-ную строку либо NULL. */
char *sheet_row_to_values_json(const SheetRow *row);

/* Вставляет пустую строку в позицию idx (строки сдвигаются вместе с
 * указателями на ячейки). */
void sheet_rows_insert(SheetRows *rows, int idx);

/* Строка пустая (все ячейки пустые)? */
int sheet_row_is_empty(const SheetRow *row);

/* Ячейка A содержит дату? */
int sheet_row_is_date(const SheetRow *row);

/* Индекс строки-заголовка блока сотрудника (-1, если не найден). */
int sheet_find_employee(const SheetRows *rows, const char *name);

/* Индекс строки с указанной датой внутри блока (-1, если нет). */
int sheet_find_date_row(const SheetRows *rows, int block_idx, const char *date);

/* Индекс последней строки блока сотрудника. */
int sheet_find_block_end(const SheetRows *rows, int block_idx);

/* Безопасно заменяет содержимое ячейки (старое значение освобождается). */
void sheet_row_set(SheetRow *row, int col, const char *value);

/* Значение ячейки ("" для NULL). */
const char *sheet_row_get(const SheetRow *row, int col);

/* Итоги по блоку сотрудника: часы, сумма, число отработанных дней. */
void sheet_block_totals(const SheetRows *rows, int block_idx,
                        double *hours, double *sum, int *days);

#ifdef __cplusplus
}
#endif

#endif /* SHEETS_DOMAIN_TIMESHEET_H */

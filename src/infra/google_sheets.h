/* infra/google_sheets.h — Google Sheets API (эффект: сеть).
 *
 * Адаптер скрывает детали HTTP: вызывающий код работает с вкладками
 * и значениями, а не с URL, заголовками и кодами ответа.
 */
#ifndef SHEETS_INFRA_GOOGLE_SHEETS_H
#define SHEETS_INFRA_GOOGLE_SHEETS_H

#include "infra/google_auth.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    GoogleAuth auth;
    char *sheet_id;              /* ID таблицы "Time tracking" */
    char  api_base[256];
    char  drive_base[256];
    char  last_error[512];       /* последняя ошибка (для диагностики) */
    int   last_get_status;       /* последний ответ чтения вкладки */
    char  last_get_resp[512];
    int   last_create_status;    /* последний ответ создания вкладки */
    char  last_create_resp[512];
} GoogleSheets;

void google_sheets_init(GoogleSheets *sheets);

/* Находит таблицу учёта времени (или создаёт её) и запоминает её ID.
 * Возвращает 0 при успехе, -1 при ошибке (текст — в sheets->last_error). */
int google_sheets_ensure(GoogleSheets *sheets);

/* Значения вкладки (JSON-массив строк). Возвращает malloc-ную строку
 * (освобождает caller) либо NULL при ошибке. */
char *google_sheets_get_values(GoogleSheets *sheets, const char *tab);

/* Дописывает строку в конец вкладки. values_json — массив из 6 строк (A..F).
 * Возвращает 0 при успехе, -1 при ошибке. */
int google_sheets_append_row(GoogleSheets *sheets, const char *tab,
                              const char *values_json);

/* Записывает готовый объект {"values":[...]} в диапазон A1-нотации. */
int google_sheets_update_values(GoogleSheets *sheets, const char *tab,
                                 const char *a1_suffix, const char *values_object);

/* Записывает значения в диапазон: принимает массив строк (без обёртки). */
int google_sheets_update_range(GoogleSheets *sheets, const char *tab,
                                const char *a1_suffix, const char *values_json);

/* Создаёт вкладку (если уже есть — считается успехом). */
int google_sheets_create_sheet(GoogleSheets *sheets, const char *tab);

/* Список названий вкладок таблицы.
 * Возвращает 0 при успехе; *out — массив malloc-нутых строк (освобождает
 * вызывающий), *count — их количество. */
int google_sheets_list_tabs(GoogleSheets *sheets, char ***out, int *count, int max);

#ifdef __cplusplus
}
#endif

#endif /* SHEETS_INFRA_GOOGLE_SHEETS_H */

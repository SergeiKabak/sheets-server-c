/* infra/http_client.h — HTTP-клиент (эффект: ходит в сеть через curl).
 *
 * Единственный модуль проекта, который знает про curl. Все остальные
 * обращаются к внешнему миру только через такие адаптеры.
 */
#ifndef SHEETS_INFRA_HTTP_CLIENT_H
#define SHEETS_INFRA_HTTP_CLIENT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GET-запрос. Возвращает тело ответа (освобождает caller) либо NULL.
 * *status — HTTP-код (0 при сетевой ошибке). */
char *http_get(const char *url, int *status);

/* POST с application/x-www-form-urlencoded. Возвращает HTTP-код. */
int http_post_form(const char *url, const char *post_fields);

/* POST с application/json. Возвращает HTTP-код. */
int http_post_json(const char *url, const char *json);

/* Универсальный запрос. Возвращает HTTP-код, *status — код ответа. */
int http_request(const char *method, const char *url,
                 const char *content_type, const char *body,
                 const char *bearer, int *status);

/* Универсальный запрос с возвратом тела ответа и HTTP-кода.
 * caller отвечает за free(out). */
char *http_request_full(const char *method, const char *url,
                        const char *content_type, const char *body,
                        const char *bearer, int *status);

/* Запрос с дополнительными заголовками (например apikey/Prefer для Supabase).
 * extra — NULL-terminated массив строк вида "Name: value".
 * hdr_out/hdr_cap (можно NULL/0) — сюда копируются заголовки ответа
 * (нужен, например, Content-Range для точных счётчиков PostgREST).
 * caller отвечает за free(out). */
char *http_request_h(const char *method, const char *url,
                     const char *content_type, const char *body,
                     const char *const *extra, int *status,
                     char *hdr_out, size_t hdr_cap);

#ifdef __cplusplus
}
#endif

#endif /* SHEETS_INFRA_HTTP_CLIENT_H */

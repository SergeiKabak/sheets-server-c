#ifndef HTTP_H
#define HTTP_H

char *http_get(const char *url, int *status);
char *url_encode(const char *s);
int  http_post_form(const char *url, const char *post_fields);
int  http_post_json(const char *url, const char *json);
int  http_request(const char *method, const char *url,
                  const char *content_type, const char *body,
                  const char *bearer, int *status);

/* Универсальный запрос с возвратом тела ответа и HTTP-кода.
   caller отвечает за free(out). */
char *http_request_full(const char *method, const char *url,
                 const char *content_type, const char *body,
                 const char *bearer, int *status);

/* Запрос с дополнительными заголовками (например apikey/Prefer для Supabase).
   extra — NULL-terminated массив строк вида "Name: value".
   hdr_out/hdr_cap (можно NULL/0) — сюда копируются заголовки ответа
   (нужен, например, Content-Range для точных счётчиков PostgREST).
   caller отвечает за free(out). */
char *http_request_h(const char *method, const char *url,
                 const char *content_type, const char *body,
                 const char *const *extra, int *status,
                 char *hdr_out, size_t hdr_cap);

#endif /* HTTP_H */

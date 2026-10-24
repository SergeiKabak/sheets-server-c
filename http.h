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

#endif /* HTTP_H */

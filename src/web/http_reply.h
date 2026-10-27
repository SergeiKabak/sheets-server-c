/* web/http_reply.h — формирование HTTP-ответов. */
#ifndef SHEETS_WEB_HTTP_REPLY_H
#define SHEETS_WEB_HTTP_REPLY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Ответ с произвольным телом (Content-Type не задаётся). */
void http_reply(int fd, const char *status, const char *body);

/* Ответ с JSON-телом (Content-Type: application/json). */
void http_reply_json(int fd, const char *status, const char *json);

/* write() может записать меньше — досылаем целиком. */
void http_write_all(int fd, const void *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* SHEETS_WEB_HTTP_REPLY_H */

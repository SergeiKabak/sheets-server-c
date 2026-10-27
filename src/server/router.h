/* server/router.h — маршрутизация HTTP-запросов. */
#ifndef SHEETS_SERVER_ROUTER_H
#define SHEETS_SERVER_ROUTER_H

#include "app/context.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Обрабатывает запрос и пишет ответ в fd.
 * path — путь с query-строкой, body — тело запроса (может быть NULL),
 * raw — исходный текст запроса (нужен для If-None-Match).
 * Возвращает 1, если маршрут найден, 0 — иначе (ответ уже отправлен). */
int router_dispatch(AppContext *app, int fd, const char *method,
                    const char *path, const char *body, const char *raw);

#ifdef __cplusplus
}
#endif

#endif /* SHEETS_SERVER_ROUTER_H */

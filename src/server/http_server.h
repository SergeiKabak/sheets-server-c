/* server/http_server.h — приём и разбор HTTP-соединений. */
#ifndef SHEETS_SERVER_HTTP_SERVER_H
#define SHEETS_SERVER_HTTP_SERVER_H

#include "app/context.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Максимальный размер запроса (буфер приёма). */
#define BUFFER_SIZE (512 * 1024)

/* Запускает сервер: слушает порт, на каждое соединение форкает процесс.
 * Возвращает код завершения main. */
int http_server_run(AppContext *app, int port);

#ifdef __cplusplus
}
#endif

#endif /* SHEETS_SERVER_HTTP_SERVER_H */

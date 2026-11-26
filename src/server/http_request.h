/* server/http_request.h — чтение HTTP-запроса из сокета клиента.
 *
 * Вынесено из http_server.c отдельным модулем, чтобы это можно было
 * проверить тестом (`make test-http`). Раньше разбор жил прямо в обработчике
 * соединения и имел три проблемы:
 *   — на сокет не ставился таймаут: клиент, открывший соединение и молчащий,
 *     держал форк-процесс сколько угодно (на 512 МБ Render это исчерпание);
 *   — Content-Length искался через `strstr` по всему буферу (регистрозависимо
 *     и вместе с телом): при другом регистре заголовка или при слове
 *     «Content-Length:» внутри тела тело запроса могло быть обрезано —
 *     Telegram-апдейт терялся молча;
 *   — длина тела не проверялась на размер буфера.
 */
#ifndef SHEETS_SERVER_HTTP_REQUEST_H
#define SHEETS_SERVER_HTTP_REQUEST_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Сколько секунд ждём данные от клиента (и отдаём ответ). */
#define HTTP_REQ_TIMEOUT_SEC 10

/* Ставит SO_RCVTIMEO/SO_SNDTIMEO на сокет клиента. seconds <= 0 — снять. */
void http_request_set_timeouts(int fd, int seconds);

/* Читает запрос целиком: заголовки + тело по Content-Length.
 * Возвращает число байт в буфере или -1 (пусто/ошибка/таймаут до заголовков).
 * Если тело не помещается в буфер, возвращает то, что влезло: ответ всё равно
 * будет 4xx, но обработчик не зависнет и не переполнит буфер. */
long http_request_read(int fd, char *buf, size_t cap);

/* Content-Length из блока заголовков (до пустой строки), без учёта регистра.
 * -1 — заголовка нет или он битый. */
long http_request_content_length(const char *buf);

/* Начало тела запроса (сразу после пустой строки) или NULL. */
char *http_request_body(char *buf);

#ifdef __cplusplus
}
#endif

#endif /* SHEETS_SERVER_HTTP_REQUEST_H */

#include "server/http_request.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

void http_request_set_timeouts(int fd, int seconds) {
    struct timeval tv;
    tv.tv_sec = seconds > 0 ? seconds : 0;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

/* Заголовки кончаются пустой строкой. Всё, что дальше, — тело, и искать
 * там Content-Length нельзя: Telegram-текст может сам содержать такую строку. */
static size_t headers_len(const char *buf) {
    const char *end = strstr(buf, "\r\n\r\n");
    return end ? (size_t)(end - buf) : strlen(buf);
}

long http_request_content_length(const char *buf) {
    if (!buf) return -1;
    const char *limit = buf + headers_len(buf);

    /* первая строка — request-line, разбор начинаем со следующей */
    const char *p = strstr(buf, "\r\n");
    if (!p || p >= limit) return -1;
    p += 2;

    while (p < limit) {
        const char *eol = strstr(p, "\r\n");
        if (!eol || eol > limit) eol = limit;
        const char *colon = memchr(p, ':', (size_t)(eol - p));
        if (colon) {
            static const char name[] = "content-length";
            size_t nlen = (size_t)(colon - p);
            if (nlen == sizeof(name) - 1 && strncasecmp(p, name, nlen) == 0) {
                char *num_end = NULL;
                long v = strtol(colon + 1, &num_end, 10);
                if (num_end == colon + 1 || v < 0) return -1;   /* битое значение */
                return v;
            }
        }
        p = eol + 2;
    }
    return -1;
}

char *http_request_body(char *buf) {
    if (!buf) return NULL;
    char *end = strstr(buf, "\r\n\r\n");
    return end ? end + 4 : NULL;
}

long http_request_read(int fd, char *buf, size_t cap) {
    if (fd < 0 || !buf || cap < 2) return -1;
    size_t total = 0;
    buf[0] = '\0';

    for (;;) {
        if (total >= cap - 1) {
            /* буфер полон: отдаём что есть, дальше ответ 4xx */
            buf[cap - 1] = '\0';
            return (long)(cap - 1);
        }

        ssize_t n = read(fd, buf + total, cap - 1 - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            /* таймаут (SO_RCVTIMEO) или обрыв до конца заголовков */
            return (long)total > 0 ? (long)total : -1;
        }
        if (n == 0) break;                 /* клиент закрыл соединение */
        total += (size_t)n;
        buf[total] = '\0';

        const char *hdr_end = strstr(buf, "\r\n\r\n");
        if (!hdr_end) continue;            /* заголовки ещё не пришли */

        long clen = http_request_content_length(buf);
        if (clen <= 0) break;              /* тела нет */

        size_t hlen = (size_t)(hdr_end + 4 - buf);
        if (clen > (long)(cap - 1 - hlen)) {
            /* тело больше буфера — не ждём его целиком */
            return (long)total;
        }
        if (total >= hlen + (size_t)clen) break;
    }

    return total ? (long)total : -1;
}

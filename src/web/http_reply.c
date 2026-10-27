#include "web/http_reply.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

void http_write_all(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) { if (errno == EINTR) continue; return; }
        p += n;
        len -= (size_t)n;
    }
}

void http_reply(int fd, const char *status, const char *body) {
    char header[256];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
        status, strlen(body ? body : ""));
    http_write_all(fd, header, (size_t)hlen);
    http_write_all(fd, body ? body : "", body ? strlen(body) : 0);
}

void http_reply_json(int fd, const char *status, const char *json) {
    char header[256];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %s\r\nContent-Type: application/json\r\n"
        "Content-Length: %zu\r\nConnection: close\r\n\r\n",
        status, strlen(json ? json : ""));
    http_write_all(fd, header, (size_t)hlen);
    http_write_all(fd, json ? json : "", json ? strlen(json) : 0);
}

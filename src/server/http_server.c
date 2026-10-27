#include "server/http_server.h"

#include "server/router.h"
#include "web/http_reply.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* Читает запрос целиком (заголовки + тело по Content-Length).
 * Возвращает длину прочитанного или -1 при ошибке. */
static long read_request(int client_fd, char *buf, size_t cap) {
    size_t total = 0;
    while (total < cap - 1) {
        ssize_t n = read(client_fd, buf + total, cap - 1 - total);
        if (n <= 0) break;
        total += n;
        buf[total] = '\0';
        char *hdr = strstr(buf, "\r\n\r\n");
        if (hdr) {
            long clen = 0;
            char *cl = strstr(buf, "Content-Length:");
            if (cl) { clen = strtol(cl + 15, NULL, 10); if (clen < 0) clen = 0; }
            size_t hl = (size_t)(hdr + 4 - buf);
            if (total >= hl + (size_t)clen) break;
        }
    }
    if (total == 0) return -1;
    return (long)total;
}

/* Обрабатывает одно соединение (выполняется в отдельном процессе). */
static void handle_conn(AppContext *app, int client_fd) {
    char buf[BUFFER_SIZE];
    if (read_request(client_fd, buf, sizeof(buf)) < 0) return;

    char method[16] = { 0 }, path[512] = { 0 };
    sscanf(buf, "%15s %511s", method, path);

    char *body = NULL;
    char *hdr = strstr(buf, "\r\n\r\n");
    if (hdr) body = hdr + 4;

    if (!router_dispatch(app, client_fd, method, path, body, buf))
        http_reply(client_fd, "404 Not Found", "Not Found");
}

int http_server_run(AppContext *app, int port) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons((unsigned short)port)
    };
    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(server_fd, 10) < 0) { perror("listen"); return 1; }

    printf("[INFO] Bot webhook listening on 0.0.0.0:%d (path /webhook)\n", port);

    for (;;) {
        /* рейпим завершившихся детей-обработчиков webhook, не блокируясь */
        while (waitpid(-1, NULL, WNOHANG) > 0) {}

        struct sockaddr_in ca;
        socklen_t clen = sizeof(ca);
        int client_fd = accept(server_fd, (struct sockaddr *)&ca, &clen);
        if (client_fd < 0) continue;
        pid_t c = fork();
        if (c == 0) { handle_conn(app, client_fd); close(client_fd); _exit(0); }
        close(client_fd);
    }

    close(server_fd);
    return 0;
}

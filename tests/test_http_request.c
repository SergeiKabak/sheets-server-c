/* tests/test_http_request.c — разбор HTTP-запроса (server/http_request.c).
 *
 * Почему это важно: webhook Telegram приходит POST-ом с JSON-телом. Если тело
 * обрезать, бот молча ничего не отвечает. Три случая, которые раньше ломались:
 *   — заголовок «content-length» в нижнем регистре (искали регистрозависимо);
 *   — в теле встречается текст «Content-Length:» (искали по всему буферу);
 *   — молчащий клиент без таймаута держал форк-процесс (проверяем, что
 *     SO_RCVTIMEO реально останавливает чтение).
 *
 * Сеть не нужна: пара сокетов поднимается на loopback, тест работает мгновенно.
 */
#include "server/http_request.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures;
static int checks;

#define CHECK(cond, what) do {                                             \
        checks++;                                                          \
        if (cond) printf("OK   %s\n", what);                               \
        else { printf("FAIL %s  (%s:%d)\n", what, __FILE__, __LINE__);     \
               failures++; }                                               \
    } while (0)

static long ms_since(const struct timespec *t0) {
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (t1.tv_sec - t0->tv_sec) * 1000 + (t1.tv_nsec - t0->tv_nsec) / 1000000;
}

/* Пара соединённых сокетов через loopback — как настоящий клиент. */
static int pair_open(int *srv, int *cli) {
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) return -1;
    int opt = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(ls, (struct sockaddr *)&a, sizeof(a)) != 0) { close(ls); return -1; }
    if (listen(ls, 1) != 0) { close(ls); return -1; }
    socklen_t alen = sizeof(a);
    if (getsockname(ls, (struct sockaddr *)&a, &alen) != 0) { close(ls); return -1; }

    *cli = socket(AF_INET, SOCK_STREAM, 0);
    if (*cli < 0) { close(ls); return -1; }
    if (connect(*cli, (struct sockaddr *)&a, sizeof(a)) != 0) {
        close(*cli); close(ls); return -1;
    }
    *srv = accept(ls, NULL, NULL);
    close(ls);
    if (*srv < 0) { close(*cli); return -1; }
    return 0;
}

static void pair_close(int srv, int cli) {
    if (srv >= 0) close(srv);
    if (cli >= 0) close(cli);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);   /* строки видны сразу, даже если тест упадёт */
    char buf[8192];

    /* ---------- 1. Content-Length: разбор заголовка ---------- */
    const char *get_req = "GET /health HTTP/1.1\r\nHost: x\r\n\r\n";
    CHECK(http_request_content_length(get_req) == -1, "нет Content-Length — -1");

    const char *post_req =
        "POST /webhook HTTP/1.1\r\nHost: x\r\nContent-Length: 27\r\n\r\n"
        "{\"update_id\":123456789012}";
    CHECK(http_request_content_length(post_req) == 27, "Content-Length распознан");
    CHECK(http_request_body((char *)post_req) != NULL &&
          strcmp(http_request_body((char *)post_req), "{\"update_id\":123456789012}") == 0,
          "тело начинается после пустой строки");

    const char *lower =
        "POST /webhook HTTP/1.1\r\nhost: x\r\ncontent-length: 5\r\n\r\nhello";
    CHECK(http_request_content_length(lower) == 5, "content-length в нижнем регистре");

    /* тело само содержит текст «Content-Length:» — его нельзя принять за заголовок */
    const char *body_trap =
        "POST /webhook HTTP/1.1\r\nContent-Length: 26\r\n\r\n"
        "{\"text\":\"Content-Length: 9\"}";
    CHECK(http_request_content_length(body_trap) == 26,
          "Content-Length из тела не подменяет заголовок");

    const char *broken =
        "POST /webhook HTTP/1.1\r\nContent-Length: abc\r\n\r\n";
    CHECK(http_request_content_length(broken) == -1, "битый Content-Length — -1");

    /* ---------- 2. Таймаут сокета ---------- */
    {
        int srv = -1, cli = -1;
        if (pair_open(&srv, &cli) == 0) {
            http_request_set_timeouts(srv, 3);
            struct timeval tv;
            socklen_t len = sizeof(tv);
            int ok = (getsockopt(srv, SOL_SOCKET, SO_RCVTIMEO, &tv, &len) == 0) &&
                     (tv.tv_sec == 3);
            CHECK(ok, "http_request_set_timeouts ставит SO_RCVTIMEO");
            pair_close(srv, cli);
        } else {
            CHECK(0, "loopback-пара для проверки таймаута");
        }
    }

    /* ---------- 3. Чтение: целый запрос ---------- */
    {
        int srv = -1, cli = -1;
        if (pair_open(&srv, &cli) == 0) {
            http_request_set_timeouts(srv, 5);
            const char *req =
                "POST /webhook HTTP/1.1\r\nHost: x\r\nContent-Length: 27\r\n\r\n"
                "{\"update_id\":123456789012}";
            write(cli, req, strlen(req));
            long n = http_request_read(srv, buf, sizeof(buf));
            CHECK(n == (long)strlen(req), "целый запрос прочитан полностью");
            CHECK(strcmp(http_request_body(buf), "{\"update_id\":123456789012}") == 0,
                  "тело целого запроса не обрезано");
            close(cli);
            CHECK(http_request_read(srv, buf, sizeof(buf)) == -1, "пустой сокет — -1");
            pair_close(srv, cli);
        } else {
            CHECK(0, "loopback-пара для целого запроса");
        }
    }

    /* ---------- 4. Чтение: заголовки и тело разными сегментами ---------- */
    {
        int srv = -1, cli = -1;
        if (pair_open(&srv, &cli) == 0) {
            http_request_set_timeouts(srv, 5);
            const char *hdr = "POST /webhook HTTP/1.1\r\ncontent-length: 27\r\n\r\n";
            const char *body = "{\"update_id\":123456789012}";
            write(cli, hdr, strlen(hdr));
            struct timespec t0;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            /* тело приходит отдельным TCP-сегментом с задержкой */
            if (fork() == 0) {
                usleep(150 * 1000);
                write(cli, body, strlen(body));
                _exit(0);
            }
            long n = http_request_read(srv, buf, sizeof(buf));
            CHECK(n == (long)(strlen(hdr) + strlen(body)),
                  "тело, пришедшее вторым сегментом, дочитано (нижний регистр заголовка)");
            CHECK(ms_since(&t0) >= 100, "чтение действительно ждало второй сегмент");
            wait(NULL);
            pair_close(srv, cli);
        } else {
            CHECK(0, "loopback-пара для двух сегментов");
        }
    }

    /* ---------- 5. Чтение: GET без тела ---------- */
    {
        int srv = -1, cli = -1;
        if (pair_open(&srv, &cli) == 0) {
            http_request_set_timeouts(srv, 5);
            write(cli, get_req, strlen(get_req));
            long n = http_request_read(srv, buf, sizeof(buf));
            CHECK(n == (long)strlen(get_req), "GET без тела читается по заголовкам");
            pair_close(srv, cli);
        } else {
            CHECK(0, "loopback-пара для GET");
        }
    }

    /* ---------- 6. Тело больше буфера: не ждём и не переполняем ---------- */
    {
        int srv = -1, cli = -1;
        if (pair_open(&srv, &cli) == 0) {
            http_request_set_timeouts(srv, 5);
            const char *hdr = "POST /webhook HTTP/1.1\r\nContent-Length: 1000000\r\n\r\n";
            write(cli, hdr, strlen(hdr));
            write(cli, "0123456789", 10);
            char small[256];
            long n = http_request_read(srv, small, sizeof(small));
            CHECK(n > 0 && n < (long)sizeof(small),
                  "тело больше буфера — читаем сколько влезло, без зависания");
            CHECK(strstr(small, "\r\n\r\n") != NULL, "заголовки в буфере целиком");
            pair_close(srv, cli);
        } else {
            CHECK(0, "loopback-пара для большого тела");
        }
    }

    /* ---------- 7. Молчащий клиент не держит процесс ---------- */
    {
        int srv = -1, cli = -1;
        if (pair_open(&srv, &cli) == 0) {
            http_request_set_timeouts(srv, 1);
            const char *part = "POST /webhook HTTP/1.1\r\nHost: x\r\n";  /* без пустой строки */
            write(cli, part, strlen(part));
            struct timespec t0;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            long n = http_request_read(srv, buf, sizeof(buf));
            long spent = ms_since(&t0);
            CHECK(n > 0, "частичные заголовки возвращаются как прочитанное");
            CHECK(spent < 3000, "чтение прервано таймаутом сокета (<3 с), а не висит");
            pair_close(srv, cli);
        } else {
            CHECK(0, "loopback-пара для таймаута");
        }
    }

    printf("\n%s (%d проверок, ошибок %d)\n",
           failures ? "TESTS FAILED" : "ALL TESTS PASSED", checks, failures);
    return failures ? 1 : 0;
}

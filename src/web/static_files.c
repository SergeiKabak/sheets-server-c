#include "web/static_files.h"

#include "web/http_reply.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *web_dir(void) {
    const char *d = getenv("WEB_DIR");
    return (d && *d) ? d : "web";
}

/* Только [A-Za-z0-9._-/], без ".." и ведущего "/": защита от path traversal. */
static int web_path_safe(const char *p) {
    if (!p || !*p || *p == '/') return 0;
    for (const char *q = p; *q; q++) {
        unsigned char c = (unsigned char)*q;
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                 c == '-' || c == '/';
        if (!ok) return 0;
        if (q[0] == '.' && q[1] == '.') return 0;
    }
    return 1;
}

static const char *web_mime(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return "application/octet-stream";
    if (!strcmp(dot, ".html")) return "text/html; charset=utf-8";
    if (!strcmp(dot, ".css"))  return "text/css; charset=utf-8";
    if (!strcmp(dot, ".js"))   return "text/javascript; charset=utf-8";
    if (!strcmp(dot, ".json")) return "application/json; charset=utf-8";
    if (!strcmp(dot, ".svg"))  return "image/svg+xml";
    if (!strcmp(dot, ".ico"))  return "image/x-icon";
    if (!strcmp(dot, ".png"))  return "image/png";
    return "application/octet-stream";
}

void static_serve(int fd, const char *rel, const char *cache, const char *req) {
    if (!web_path_safe(rel)) { http_reply(fd, "400 Bad Request", "Bad Request"); return; }

    char full[1024];
    int n = snprintf(full, sizeof(full), "%s/%s", web_dir(), rel);
    if (n <= 0 || n >= (int)sizeof(full)) {
        http_reply(fd, "414 URI Too Long", "Too Long");
        return;
    }

    FILE *fp = fopen(full, "rb");
    if (!fp) { http_reply(fd, "404 Not Found", "Not Found"); return; }
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); http_reply(fd, "500 Internal Server Error", "read"); return; }
    long sz = ftell(fp);
    if (sz < 0 || sz > 4L * 1024 * 1024) {
        fclose(fp);
        http_reply(fd, "500 Internal Server Error", "read");
        return;
    }
    rewind(fp);

    char etag[64];
    struct stat st;
    if (fstat(fileno(fp), &st) == 0)
        snprintf(etag, sizeof(etag), "\"%llx-%llx\"",
                 (unsigned long long)sz, (unsigned long long)st.st_mtime);
    else
        snprintf(etag, sizeof(etag), "\"%llx\"", (unsigned long long)sz);

    if (req) {
        const char *inm = strstr(req, "If-None-Match:");
        if (inm) {
            const char *eol = strstr(inm, "\r\n");
            size_t vlen = eol ? (size_t)(eol - inm) : strlen(inm);
            if (vlen < 512) {
                char val[512];
                memcpy(val, inm, vlen);
                val[vlen] = '\0';
                if (strstr(val, etag)) {
                    char h304[256];
                    int n304 = snprintf(h304, sizeof(h304),
                        "HTTP/1.1 304 Not Modified\r\nETag: %s\r\n"
                        "Cache-Control: %s\r\nConnection: close\r\n\r\n",
                        etag, cache);
                    fclose(fp);
                    http_write_all(fd, h304, (size_t)n304);
                    return;
                }
            }
        }
    }

    char *data = malloc((size_t)sz + 1);
    if (!data) { fclose(fp); http_reply(fd, "500 Internal Server Error", "oom"); return; }
    size_t got = fread(data, 1, (size_t)sz, fp);
    fclose(fp);

    char hdr[512];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "ETag: %s\r\n"
        "Cache-Control: %s\r\n"
        "Connection: close\r\n\r\n",
        web_mime(rel), got, etag, cache);
    http_write_all(fd, hdr, (size_t)hlen);
    http_write_all(fd, data, got);
    free(data);
}

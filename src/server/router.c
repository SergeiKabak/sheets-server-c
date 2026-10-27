#include "server/router.h"

#include "core/json_text.h"
#include "core/strbuf.h"
#include "web/api_schedule.h"
#include "web/api_tracking.h"
#include "web/diag.h"
#include "web/http_reply.h"
#include "web/static_files.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Статика Mini App: /app.css, файлы из js/, /favicon.ico —
   отдаются из web/ с Cache-Control (браузер не перетаскивает их каждый раз). */
static int serve_static_path(AppContext *app, int fd, const char *path,
                              const char *raw) {
    (void)app;
    const char *q = strchr(path, '?');
    size_t plen = q ? (size_t)(q - path) : strlen(path);
    if (plen == 8 && strncmp(path, "/app.css", 8) == 0) {
        static_serve(fd, "app.css", WEB_CACHE, raw);
        return 1;
    }
    if (plen > 4 && plen < 240 && strncmp(path, "/js/", 4) == 0) {
        char rel[256];
        memcpy(rel, path + 1, plen - 1);
        rel[plen - 1] = '\0';
        static_serve(fd, rel, WEB_CACHE, raw);
        return 1;
    }
    if (plen == 12 && strncmp(path, "/favicon.ico", 12) == 0) {
        static_serve(fd, "favicon.ico", WEB_CACHE, raw);
        return 1;
    }
    return 0;
}

/* /version — какой коммит реально задеплоен (Render прокидывает
   RENDER_GIT_COMMIT / RENDER_GIT_BRANCH в окружение). */
static void serve_version(AppContext *app, int fd) {
    const char *sha    = getenv("RENDER_GIT_COMMIT");
    const char *branch = getenv("RENDER_GIT_BRANCH");
    int has_url = app->cfg.miniapp_url && *app->cfg.miniapp_url;

    StrBuf sb;
    sbuf_init(&sb);
    sbuf_append(&sb, "{\"commit\":");
    sbuf_append_json_string(&sb, sha ? sha : "неизвестно");
    sbuf_append(&sb, ",\"branch\":");
    sbuf_append_json_string(&sb, branch ? branch : "неизвестно");
    sbuf_append(&sb, ",\"track_buttons\":\"Пришла/Ушла\"");
    sbuf_append(&sb, ",\"miniapp_url\":");
    sbuf_append_json_string(&sb, has_url ? "задан" : "не задан");
    sbuf_append(&sb, ",\"menu_button\":");
    sbuf_append_json_string(&sb, has_url ? "web_app График" : "нет");
    sbuf_append(&sb, "}");
    char *json = sbuf_take(&sb);
    http_reply_json(fd, "200 OK", json ? json : "{}");
    free(json);
}

int router_dispatch(AppContext *app, int fd, const char *method,
                    const char *path, const char *body, const char *raw) {
    if (!app || !path) return 0;

    if (strcmp(path, "/health") == 0) {
        http_reply(fd, "200 OK", "OK");
        return 1;
    }
    if (strcmp(path, "/version") == 0) {
        serve_version(app, fd);
        return 1;
    }
    if (strcmp(path, "/") == 0) {
        static_serve(fd, "index.html", WEB_CACHE_HTML, raw);
        return 1;
    }
    if (serve_static_path(app, fd, path, raw)) return 1;

    if (strcmp(path, "/api/months") == 0) {
        api_months(app, fd);
        return 1;
    }
    if (strncmp(path, "/api/month", 10) == 0) {
        api_month(app, fd, path);
        return 1;
    }
    if (strncmp(path, "/api/cabinet", 12) == 0) {
        api_cabinet(app, fd, path);
        return 1;
    }
    if (strncmp(path, "/api/tracking/", 14) == 0 ||
        strcmp(path, "/diag/tracking") == 0 ||
        strncmp(path, "/diag/tracking/", 15) == 0) {
        api_tracking_handle(app, fd, method, path, body);
        return 1;
    }
    if (strncmp(path, "/diag/", 6) == 0 || strcmp(path, "/diag") == 0) {
        diag_handle(app, fd, path);
        return 1;
    }
    if (strcmp(path, "/webhook") == 0) {
        /* сразу отвечаем 200, чтобы Telegram не ретраил, потом обрабатываем */
        http_reply(fd, "200 OK", "OK");
        if (body && *body) bot_process_update(&app->bot, body);
        return 1;
    }
    return 0;
}

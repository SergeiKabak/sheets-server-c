#include "web/api_schedule.h"

#include "app/month_api.h"
#include "web/http_reply.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void api_months(AppContext *app, int fd) {
    char json[65536];
    if (month_api_months_json(&app->cfg, json, sizeof(json)) != 0) {
        http_reply_json(fd, "500 Internal Server Error", "{}");
        return;
    }
    http_reply_json(fd, "200 OK", json);
}

void api_month(AppContext *app, int fd, const char *path) {
    char json[65536];
    int code = month_api_month_json(&app->tracking, path, json, sizeof(json));
    switch (code) {
        case 200: http_reply_json(fd, "200 OK", json); break;
        case 400: http_reply_json(fd, "400 Bad Request", json); break;
        case 404: http_reply_json(fd, "404 Not Found", json); break;
        default:  http_reply_json(fd, "500 Internal Server Error", json); break;
    }
}

void api_cabinet(AppContext *app, int fd, const char *path) {
    char json[131072];
    int code = month_api_cabinet_json(&app->tracking, path, json, sizeof(json));
    switch (code) {
        case 200: http_reply_json(fd, "200 OK", json); break;
        case 400: http_reply_json(fd, "400 Bad Request", json); break;
        default:  http_reply_json(fd, "500 Internal Server Error", json); break;
    }
}

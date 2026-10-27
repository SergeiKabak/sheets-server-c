/*
 * main.c — composition root.
 *
 * Здесь и только здесь собираются вместе конфигурация, сервисы и сервер.
 * Все остальные модули получают зависимости явными аргументами, поэтому
 * порядок инициализации виден сразу, а не спрятан в глобальных переменных.
 *
 * Telegram-бот с webhook + Mini App (график смен) на чистом C.
 * Деплой: Docker на Render (Web Service).
 */

#include "app/background.h"
#include "app/context.h"
#include "infra/supabase.h"
#include "server/http_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(void) {
    int port = 8080;
    const char *port_env = getenv("PORT");
    if (port_env) port = atoi(port_env);

    /* Контекст приложения: конфигурация + сервисы. */
    AppContext app;
    app_context_init(&app);

    /* Буфер графика в Supabase (включается, если заданы переменные). */
    sb_init();

    fprintf(stderr, "[build] cabinet-index-v2 (all rows, name inheritance, shift overlap)\n");
    fprintf(stderr, "[config] SHEET=%s FROM=%s TO=%s SEARCH_NAME='%s'\n",
            app.cfg.spreadsheet_id ? app.cfg.spreadsheet_id : "",
            app.cfg.from_col ? app.cfg.from_col : "",
            app.cfg.to_col ? app.cfg.to_col : "",
            app.cfg.search_name ? app.cfg.search_name : "");

    if (config_validate(&app.cfg) != 0) {
        fprintf(stderr, "[ERROR] Required env vars missing:\n");
        return 1;
    }

    /* Устанавливаем кнопку меню чата, которая открывает Mini App одним кликом */
    bot_set_menu_button(&app.bot);

    /* ВАЖНО: НЕ игнорируем SIGCHLD — на Linux это ломает waitpid() внутри
       pclose() для popen/curl: pclose вернёт -1, и все fetch_url будут
       ошибочно считаться проваленными. Детей рейпим в accept-цикле. */

    pid_t upd = fork();
    if (upd == 0) { background_run(&app); _exit(0); }

    return http_server_run(&app, port);
}

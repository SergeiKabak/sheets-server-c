#include "app/context.h"

void app_context_init(AppContext *app) {
    if (!app) return;
    config_load(&app->cfg);
    tracking_service_init(&app->tracking, &app->cfg);
    bot_service_init(&app->bot, &app->cfg, &app->tracking);
}

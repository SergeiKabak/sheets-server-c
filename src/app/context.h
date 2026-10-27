/* app/context.h — контекст приложения.
 *
 * Единственный объект, который связывает слои: конфигурация, сервис учёта
 * времени и бот. Создаётся в composition root (main.c) и передаётся
 * дальше явно — вместо разбросанных по модулям глобальных переменных.
 */
#ifndef SHEETS_APP_CONTEXT_H
#define SHEETS_APP_CONTEXT_H

#include "app/bot_service.h"
#include "app/config.h"
#include "app/tracking_service.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    AppConfig       cfg;
    TrackingService tracking;
    BotService      bot;
} AppContext;

/* Создаёт контекст: читает окружение, готовит сервисы. */
void app_context_init(AppContext *app);

#ifdef __cplusplus
}
#endif

#endif /* SHEETS_APP_CONTEXT_H */

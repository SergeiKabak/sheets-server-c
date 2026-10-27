/* app/bot_service.h — сценарии Telegram-бота.
 *
 * Разбор входящего update и ответы в чат. Никакой работы с сетью:
 * отправка — через infra/telegram_api.
 */
#ifndef SHEETS_APP_BOT_SERVICE_H
#define SHEETS_APP_BOT_SERVICE_H

#include "app/config.h"
#include "app/tracking_service.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BOT_MAX_CHATS 128

typedef struct {
    const AppConfig   *cfg;
    TrackingService   *tracking;
    /* Состояние чатов: индекс показанного месяца графика. */
    long long chat_id[BOT_MAX_CHATS];
    int      chat_idx[BOT_MAX_CHATS];
    int      chat_count;
} BotService;

void bot_service_init(BotService *bot, const AppConfig *cfg,
                       TrackingService *tracking);

/* Обрабатывает входящий Telegram update (JSON тела webhook). */
void bot_process_update(BotService *bot, const char *body);

/* Устанавливает кнопку меню чата (Mini App). */
void bot_set_menu_button(BotService *bot);

#ifdef __cplusplus
}
#endif

#endif /* SHEETS_APP_BOT_SERVICE_H */

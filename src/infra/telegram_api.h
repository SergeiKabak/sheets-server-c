/* infra/telegram_api.h — Telegram Bot API (эффект: сеть).
 *
 * Адаптер ничего не решает: он только отправляет сообщения и отвечает на
 * callback-запросы. Содержимое клавиатур собирает прикладной слой.
 */
#ifndef SHEETS_INFRA_TELEGRAM_API_H
#define SHEETS_INFRA_TELEGRAM_API_H

#ifdef __cplusplus
extern "C" {
#endif

/* Отправляет текст сообщения. */
void telegram_send_text(const char *bot_token, long long chat_id, const char *text);

/* Отправляет текст вместе с клавиатурой (markup_json — готовый JSON
 * разметки: reply_markup или inline_keyboard). */
void telegram_send_text_markup(const char *bot_token, long long chat_id,
                               const char *text, const char *markup_json);

/* Подтверждает нажатие inline-кнопки (id — строка!). */
void telegram_answer_callback(const char *bot_token, const char *cq_id,
                               const char *text);

/* Устанавливает кнопку меню чата (открывает Mini App одним кликом). */
int telegram_set_menu_button(const char *bot_token, const char *miniapp_url);

#ifdef __cplusplus
}
#endif

#endif /* SHEETS_INFRA_TELEGRAM_API_H */

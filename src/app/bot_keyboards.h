/* app/bot_keyboards.h — клавиатуры бота, общие для нескольких сценариев.
 *
 * Вынесены из bot_service.c, потому что кнопки «Пришла/Ушла» нужны не только
 * меню учёта времени, но и напоминаниям («смена началась, а отметки нет»).
 */
#ifndef SHEETS_APP_BOT_KEYBOARDS_H
#define SHEETS_APP_BOT_KEYBOARDS_H

/* Инлайн-кнопки отметки: сразу под сообщением, работают и в напоминании. */
#define TRACK_INLINE_JSON "{\"inline_keyboard\":[[{\"text\":\"Пришла\",\"callback_data\":\"trk_in\"},{\"text\":\"Ушла\",\"callback_data\":\"trk_out\"}]]}"

#endif /* SHEETS_APP_BOT_KEYBOARDS_H */

#include "app/bot_service.h"

#include "app/month_cache.h"
#include "app/reminder_service.h"
#include "vendor/cJSON.h"
#include "domain/schedule.h"
#include "infra/telegram_api.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Клавиатура учёта времени (Пришла / Ушла / Напомнить) в JSON-виде. */
#define TRACK_KEYBOARD_JSON "{\"keyboard\":[[{\"text\":\"Пришла\"},{\"text\":\"Ушла\"}],[{\"text\":\"Напомнить\"}]],\"resize_keyboard\":true,\"is_persistent\":true}"

/* Инлайн-клавиатура учёта времени. */
#define TRACK_INLINE_JSON "{\"inline_keyboard\":[[{\"text\":\"Пришла\",\"callback_data\":\"trk_in\"},{\"text\":\"Ушла\",\"callback_data\":\"trk_out\"}]]}"

/* Выбор времени напоминания о завтрашней смене. */
#define REMIND_INLINE_JSON "{\"inline_keyboard\":[" \
    "[{\"text\":\"08:00\",\"callback_data\":\"rm:08\"}," \
    "{\"text\":\"09:00\",\"callback_data\":\"rm:09\"}," \
    "{\"text\":\"20:00\",\"callback_data\":\"rm:20\"}]," \
    "[{\"text\":\"За час до смены\",\"callback_data\":\"rm:shift\"}]," \
    "[{\"text\":\"Отменить напоминания\",\"callback_data\":\"rm:off\"}]]}"

static const char HELP_MSG[] =
    "Привет! Я бот-график смен.\n\n"
    "График — кнопка меню чата\n"
    "Отметить время — кнопки внизу\n"
    "Напомнить — сообщение о смене завтра";

void bot_service_init(BotService *bot, const AppConfig *cfg,
                       TrackingService *tracking) {
    if (!bot) return;
    memset(bot, 0, sizeof(*bot));
    bot->cfg = cfg;
    bot->tracking = tracking;
}

void bot_set_menu_button(BotService *bot) {
    if (!bot || !bot->cfg) return;
    if (!bot->cfg->miniapp_url || !*bot->cfg->miniapp_url) return;
    telegram_set_menu_button(bot->cfg->bot_token, bot->cfg->miniapp_url);
}

/* ---------- вспомогательные действия ---------- */

static void send_text(BotService *bot, long long chat_id, const char *text) {
    telegram_send_text(bot->cfg->bot_token, chat_id, text);
}

/* Отправляет текст вместе с клавиатурой — так клавиатура всегда остаётся
   на экране, даже если пользователь её раньше скрыл. */
static void send_text_kb(BotService *bot, long long chat_id, const char *text) {
    telegram_send_text_markup(bot->cfg->bot_token, chat_id, text,
                               TRACK_KEYBOARD_JSON);
}

static void send_track_menu(BotService *bot, long long chat_id) {
    telegram_send_text_markup(bot->cfg->bot_token, chat_id,
                               "Отметьте рабочее время:", TRACK_KEYBOARD_JSON);
}

static void send_track_inline(BotService *bot, long long chat_id) {
    telegram_send_text_markup(bot->cfg->bot_token, chat_id,
                               "Отметьте рабочее время:", TRACK_INLINE_JSON);
}

/* Меню выбора времени напоминания о завтрашней смене. */
static void send_remind_menu(BotService *bot, long long chat_id) {
    telegram_send_text_markup(bot->cfg->bot_token, chat_id,
                               "Когда напомнить о завтрашней смене?",
                               REMIND_INLINE_JSON);
}

/* Выполняет отметку и отправляет результат в чат. */
static void bot_track(BotService *bot, long long chat_id, const char *action) {
    char json[1024];
    int code = tracking_do_clock(bot->tracking, action, json, sizeof(json));

    cJSON *root = cJSON_Parse(json);
    const char *err = root ? cJSON_GetStringValue(
                                 cJSON_GetObjectItemCaseSensitive(root, "error")) : NULL;
    char msg[768];

    if (code == 200) {
        const char *t = root ? cJSON_GetStringValue(
                                 cJSON_GetObjectItemCaseSensitive(root, "time")) : NULL;
        if (strcmp(action, "in") == 0) {
            snprintf(msg, sizeof(msg), "Записано! Приход в %s", t ? t : "");
        } else {
            const char *h = root ? cJSON_GetStringValue(
                                     cJSON_GetObjectItemCaseSensitive(root, "hours")) : NULL;
            const char *s = root ? cJSON_GetStringValue(
                                     cJSON_GetObjectItemCaseSensitive(root, "sum")) : NULL;
            snprintf(msg, sizeof(msg), "Записано! Уход в %s", t ? t : "");
            if (h && *h) {
                size_t l = strlen(msg);
                snprintf(msg + l, sizeof(msg) - l, "\nОтработано: %s ч.", h);
            }
            if (s && *s) {
                size_t l = strlen(msg);
                snprintf(msg + l, sizeof(msg) - l, "\nСумма: %s", s);
            }
        }
    } else {
        snprintf(msg, sizeof(msg), "Ошибка: %s",
                 (err && *err) ? err : "Не удалось записать");
    }
    cJSON_Delete(root);

    /* Ответ идёт вместе с клавиатурой: она всегда остаётся на экране */
    send_text_kb(bot, chat_id, msg);
}

/* ---------- состояние чатов ---------- */

static int bot_get_chat(BotService *bot, long long id) {
    for (int i = 0; i < bot->chat_count; i++)
        if (bot->chat_id[i] == id) return i;
    if (bot->chat_count < BOT_MAX_CHATS) {
        bot->chat_id[bot->chat_count] = id;
        bot->chat_idx[bot->chat_count] = -1;
        return bot->chat_count++;
    }
    return -1;
}

static int pick_current_index(int count) {
    if (count <= 0) return -1;
    return count - 1; /* последний лист по умолчанию */
}

static void show_index(BotService *bot, long long cid, int idx,
                        const MonthSchedule *months, int count) {
    if (idx < 0 || idx >= count) {
        send_text_kb(bot, cid, "Нет данных по графикам.");
        return;
    }
    char label[64];
    tracking_month_label(months[idx].title, label, sizeof(label));
    char msg[256];
    snprintf(msg, sizeof(msg), "График: %s", label);
    send_text_kb(bot, cid, msg);
}

/* ---------- обработка update ---------- */

void bot_process_update(BotService *bot, const char *body) {
    if (!bot || !body) return;
    cJSON *root = cJSON_Parse(body);
    if (!root) return;

    cJSON *message = cJSON_GetObjectItemCaseSensitive(root, "message");
    cJSON *chat = message ? cJSON_GetObjectItemCaseSensitive(message, "chat") : NULL;
    cJSON *chat_id = chat ? cJSON_GetObjectItemCaseSensitive(chat, "id") : NULL;
    if (message && chat_id && cJSON_IsNumber(chat_id)) {
        long long cid = (long long)chat_id->valuedouble;
        cJSON *text_item = cJSON_GetObjectItemCaseSensitive(message, "text");
        const char *text = cJSON_IsString(text_item) ? text_item->valuestring : NULL;
        if (text && *text) {
            char low[1024];
            int j = 0;
            for (int i = 0; text[i] && j < 1022; i++)
                low[j++] = (char)tolower((unsigned char)text[i]);
            low[j] = '\0';

            /* Кнопки учёта времени (reply-клавиатура) */
            if (strcmp(low, "/start") == 0 || strcmp(low, "/help") == 0 ||
                strcmp(low, "/start@") == 0) {
                /* /start и /help — принудительно обновляют клавиатуру */
                send_text_kb(bot, cid, HELP_MSG);
                send_track_menu(bot, cid);
            } else if (strstr(text, "Пришла") || strcmp(low, "/in") == 0) {
                bot_track(bot, cid, "in");
            } else if (strstr(text, "Ушла") || strcmp(low, "/out") == 0) {
                bot_track(bot, cid, "out");
            } else if (strstr(low, "/remind") || strstr(text, "Напомин")) {
                send_remind_menu(bot, cid);
            } else if (strstr(low, "/track") || strstr(low, "/time") ||
                       strstr(text, "Отметить") || strstr(text, "время")) {
                send_track_inline(bot, cid);
                send_track_menu(bot, cid);
            } else {
                send_text(bot, cid, HELP_MSG);
                send_track_menu(bot, cid);
            }
        }
        cJSON_Delete(root);
        return;
    }

    cJSON *cq = cJSON_GetObjectItemCaseSensitive(root, "callback_query");
    if (cq) {
        /* id в callback_query — строка, не число */
        cJSON *id_item = cJSON_GetObjectItemCaseSensitive(cq, "id");
        const char *cqid = cJSON_IsString(id_item) ? id_item->valuestring : NULL;
        cJSON *cq_chat = cJSON_GetObjectItemCaseSensitive(cq, "message");
        cJSON *cq_chat_obj = cq_chat ? cJSON_GetObjectItemCaseSensitive(cq_chat, "chat") : NULL;
        cJSON *cq_chat_id = cq_chat_obj ? cJSON_GetObjectItemCaseSensitive(cq_chat_obj, "id") : NULL;
        long long cid = cJSON_IsNumber(cq_chat_id) ? (long long)cq_chat_id->valuedouble : 0;
        cJSON *data_item = cJSON_GetObjectItemCaseSensitive(cq, "data");
        const char *data = cJSON_IsString(data_item) ? data_item->valuestring : NULL;

        MonthSchedule months[SCHEDULE_MAX_MONTHS];
        memset(months, 0, sizeof(months));
        int count = month_cache_load(months, SCHEDULE_MAX_MONTHS);

        int gi = bot_get_chat(bot, cid);
        int idx = (gi >= 0) ? bot->chat_idx[gi] : -1;
        if (idx < 0) idx = pick_current_index(count);

        if (data) {
            /* Кнопки учёта времени */
            if (strcmp(data, "trk_in") == 0) {
                bot_track(bot, cid, "in");
                if (cqid) telegram_answer_callback(bot->cfg->bot_token, cqid, "Записано");
                month_schedules_free(months, count);
                cJSON_Delete(root);
                return;
            }
            if (strcmp(data, "trk_out") == 0) {
                bot_track(bot, cid, "out");
                if (cqid) telegram_answer_callback(bot->cfg->bot_token, cqid, "Записано");
                month_schedules_free(months, count);
                cJSON_Delete(root);
                return;
            }
            /* Напоминание о завтрашней смене */
            if (strncmp(data, "rm:", 3) == 0) {
                const char *choice = data + 3;
                if (strcmp(choice, "off") == 0) {
                    int rc = reminder_cancel(cid);
                    if (cqid)
                        telegram_answer_callback(bot->cfg->bot_token, cqid,
                                                 rc == 0 ? "Отменено" : "Ошибка");
                    send_text_kb(bot, cid,
                                 rc == 0 ? "Напоминания отменены."
                                         : "Не удалось отменить: база недоступна.");
                } else {
                    char reply[768];
                    int rc = reminder_schedule(bot->cfg, bot->tracking,
                                               months, count, cid, choice,
                                               reply, sizeof(reply));
                    if (cqid)
                        telegram_answer_callback(bot->cfg->bot_token, cqid,
                                                 rc == 0 ? "Готово" : "Не получилось");
                    send_text_kb(bot, cid, reply);
                }
                month_schedules_free(months, count);
                cJSON_Delete(root);
                return;
            }
            if (strcmp(data, "prev") == 0) { if (idx > 0) idx--; }
            else if (strcmp(data, "next") == 0) { if (idx < count - 1) idx++; }
        }
        if (gi >= 0) bot->chat_idx[gi] = idx;
        show_index(bot, cid, idx, months, count);
        if (cqid) telegram_answer_callback(bot->cfg->bot_token, cqid, NULL);
        month_schedules_free(months, count);
        cJSON_Delete(root);
        return;
    }

    cJSON_Delete(root);
}

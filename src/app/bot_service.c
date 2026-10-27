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

/* Выбор напоминания: разово на завтра или каждый день по графику. */
#define REMIND_INLINE_JSON "{\"inline_keyboard\":[" \
    "[{\"text\":\"08:00\",\"callback_data\":\"rm:08\"}," \
    "{\"text\":\"09:00\",\"callback_data\":\"rm:09\"}," \
    "{\"text\":\"20:00\",\"callback_data\":\"rm:20\"}]," \
    "[{\"text\":\"За час до смены\",\"callback_data\":\"rm:shift\"}]," \
    "[{\"text\":\"Включить по графику\",\"callback_data\":\"rm:daily_on\"}," \
    "{\"text\":\"Настройки\",\"callback_data\":\"rm:cfg\"}]," \
    "[{\"text\":\"Выключить по графику\",\"callback_data\":\"rm:daily_off\"}," \
    "{\"text\":\"Отменить всё\",\"callback_data\":\"rm:off\"}]]}"

#define REMIND_MENU_TEXT \
    "Напоминания о смене.\n\n" \
    "Разово: 08:00 / 09:00 / 20:00 или за час до смены.\n" \
    "По графику: каждый день перед началом и после конца смены — " \
    "с настройками в кнопке «Настройки»."

static const char HELP_MSG[] =
    "Привет! Я бот-график смен.\n\n"
    "График — кнопка меню чата\n"
    "Отметить время — кнопки внизу\n"
    "Напомнить — смена завтра или каждый день по графику";

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
                               REMIND_MENU_TEXT, REMIND_INLINE_JSON);
}

/* Кнопка выбора значения: текущее помечается галочкой. */
static void settings_btn(char *out, size_t cap, size_t *len, int first,
                         const char *prefix, int value, int current) {
    int n = snprintf(out + *len, cap - *len,
                     "%s{\"text\":\"%s%d\",\"callback_data\":\"%s%d\"}",
                     first ? "" : ",",
                     value == current ? "✓ " : "", value, prefix, value);
    if (n > 0 && (size_t)n < cap - *len) *len += (size_t)n;
}

/* Экран настроек режима «по графику» с текущими значениями. */
static void send_remind_settings(BotService *bot, long long chat_id,
                                 const ReminderSettings *s) {
    static const int lead_vals[]  = { 5, 10, 15, 30, 60 };
    static const int delay_vals[] = { 0, 5, 10, 15, 30 };

    char json[1200];
    size_t len = 0;
    int n = snprintf(json, sizeof(json), "{\"inline_keyboard\":[[");
    if (n > 0) len = (size_t)n;

    for (size_t i = 0; i < sizeof(lead_vals) / sizeof(lead_vals[0]); i++)
        settings_btn(json, sizeof(json), &len, i == 0, "rm:lead:",
                     lead_vals[i], s->start_lead_min);

    n = snprintf(json + len, sizeof(json) - len, "],[");
    if (n > 0) len += (size_t)n;

    for (size_t i = 0; i < sizeof(delay_vals) / sizeof(delay_vals[0]); i++)
        settings_btn(json, sizeof(json), &len, i == 0, "rm:after:",
                     delay_vals[i], s->end_delay_min);

    n = snprintf(json + len, sizeof(json) - len,
                 "],[{\"text\":\"Назад\",\"callback_data\":\"rm:menu\"}]]}");
    if (n > 0) len += (size_t)n;

    char text[512];
    snprintf(text, sizeof(text),
             "Настройки напоминаний по графику\n\n"
             "До начала смены: %d мин\n"
             "После конца смены: %d мин\n"
             "Режим: %s\n\n"
             "Верхний ряд — за сколько минут до начала смены напомнить "
             "отметить приход.\n"
             "Нижний ряд — через сколько минут после конца смены напомнить "
             "отметить уход.",
             s->start_lead_min, s->end_delay_min,
             s->enabled ? "включён" : "выключен");

    telegram_send_text_markup(bot->cfg->bot_token, chat_id, text, json);
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

/* Обрабатывает нажатия в меню напоминаний. Всегда возвращает 1:
   ветка rm:* полностью обслуживается здесь. */
static int remind_callback(BotService *bot, long long cid, const char *cqid,
                           const char *choice,
                           const MonthSchedule *months, int count) {
    char reply[768];
    ReminderSettings s;
    int rc = 0;

    /* Нажатие в настройках подтверждаем молча, разовые — с тостом «Готово». */
    int one_off = (strcmp(choice, "shift") == 0) ||
                  (choice[0] >= '0' && choice[0] <= '9');
    if (cqid && !one_off)
        telegram_answer_callback(bot->cfg->bot_token, cqid, NULL);

    if (strcmp(choice, "menu") == 0) {
        send_remind_menu(bot, cid);
        return 1;
    }

    if (strcmp(choice, "cfg") == 0) {
        if (reminder_settings_get(cid, &s) != 0)
            send_text_kb(bot, cid, "Настройки недоступны: база не отвечает.");
        else
            send_remind_settings(bot, cid, &s);
        return 1;
    }

    if (strncmp(choice, "lead:", 5) == 0 || strncmp(choice, "after:", 6) == 0) {
        int is_lead = (choice[0] == 'l');
        int val = atoi(choice + (is_lead ? 5 : 6));
        if (reminder_settings_get(cid, &s) != 0) {
            send_text_kb(bot, cid, "Настройки недоступны: база не отвечает.");
            return 1;
        }
        if (is_lead) s.start_lead_min = val;
        else         s.end_delay_min = val;
        if (reminder_settings_save(cid, &s) != 0)
            send_text_kb(bot, cid, "Не удалось сохранить: база недоступна.");
        else
            send_remind_settings(bot, cid, &s);
        return 1;
    }

    if (strcmp(choice, "daily_on") == 0 || strcmp(choice, "daily_off") == 0) {
        int on = (strcmp(choice, "daily_on") == 0);
        if (reminder_settings_get(cid, &s) != 0) {
            send_text_kb(bot, cid, "Не удалось включить: база недоступна.");
            return 1;
        }
        s.enabled = on;
        if (reminder_settings_save(cid, &s) != 0) {
            send_text_kb(bot, cid, "Не удалось сохранить: база недоступна.");
            return 1;
        }
        if (!on) {
            send_text_kb(bot, cid, "Выключил напоминания по графику.");
        } else if (s.end_delay_min > 0) {
            snprintf(reply, sizeof(reply),
                     "Включил. Напомню за %d мин до начала смены и через "
                     "%d мин после её конца — по графику.\n"
                     "Изменить время — кнопка «Настройки».",
                     s.start_lead_min, s.end_delay_min);
            send_text_kb(bot, cid, reply);
        } else {
            snprintf(reply, sizeof(reply),
                     "Включил. Напомню за %d мин до начала смены и сразу "
                     "после её конца — по графику.\n"
                     "Изменить время — кнопка «Настройки».",
                     s.start_lead_min);
            send_text_kb(bot, cid, reply);
        }
        return 1;
    }

    if (strcmp(choice, "off") == 0) {
        rc = reminder_cancel(cid);
        send_text_kb(bot, cid, rc == 0 ? "Напоминания отменены."
                                       : "Не удалось отменить: база недоступна.");
        return 1;
    }

    /* 08 / 09 / 20 / shift — разово на завтра */
    rc = reminder_schedule(bot->cfg, bot->tracking, months, count, cid, choice,
                           reply, sizeof(reply));
    if (cqid)
        telegram_answer_callback(bot->cfg->bot_token, cqid,
                                 rc == 0 ? "Готово" : "Не получилось");
    send_text_kb(bot, cid, reply);
    return 1;
}

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
            /* Напоминания: разовые и режим «по графику» */
            if (strncmp(data, "rm:", 3) == 0) {
                remind_callback(bot, cid, cqid, data + 3, months, count);
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

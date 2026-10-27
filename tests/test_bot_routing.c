/* Тесты маршрутизации сообщений бота (app/bot_service.c).
 *
 * Telegram-клиент подменён заглушками: тест проверяет, какую клавиатуру
 * бот выбирает для текста сообщения. Ловит опечатки в ключевых словах
 * кнопок (например, «Напомн» вместо «Напомин»).
 *
 * Запуск: make test-bot
 */
#include "app/bot_service.h"
#include "app/config.h"
#include "app/tracking_service.h"

#include <stdio.h>
#include <string.h>

static int fails = 0;

static char last_text[1024];
static char last_markup[1024];
static int  sent_count = 0;
static int  saw_help = 0;

static void ok(int cond, const char *msg) {
    if (cond) {
        printf("OK   %s\n", msg);
    } else {
        fails++;
        printf("FAIL %s\n", msg);
    }
}

/* ---------- заглушки Telegram ---------- */

void telegram_send_text(const char *t, long long c, const char *text) {
    (void)t; (void)c;
    sent_count++;
    snprintf(last_text, sizeof(last_text), "%s", text ? text : "");
    last_markup[0] = '\0';
    if (text && strstr(text, "бот-график")) saw_help = 1;
}

void telegram_send_text_markup(const char *t, long long c, const char *text,
                               const char *markup) {
    (void)t; (void)c;
    sent_count++;
    snprintf(last_text, sizeof(last_text), "%s", text ? text : "");
    snprintf(last_markup, sizeof(last_markup), "%s", markup ? markup : "");
    if (text && strstr(text, "бот-график")) saw_help = 1;
}

void telegram_answer_callback(const char *t, const char *id, const char *text) {
    (void)t; (void)id; (void)text;
}

int telegram_set_menu_button(const char *t, const char *url) {
    (void)t; (void)url;
    return 200;
}

/* ---------- хелперы ---------- */

static void feed(BotService *bot, const char *text) {
    char upd[1024];
    snprintf(upd, sizeof(upd),
             "{\"update_id\":1,\"message\":{\"message_id\":1,\"date\":1,"
             "\"chat\":{\"id\":1,\"type\":\"private\"},\"text\":\"%s\"}}",
             text);
    sent_count = 0;
    saw_help = 0;
    last_text[0] = '\0';
    last_markup[0] = '\0';
    bot_process_update(bot, upd);
}

static int is_remind_menu(void) {
    return strstr(last_markup, "rm:daily_on") != NULL;
}

static int is_help(void) {
    return saw_help;
}

int main(void) {
    AppConfig cfg;
    config_load(&cfg);

    TrackingService ts;
    memset(&ts, 0, sizeof(ts));
    ts.cfg = &cfg;
    ts.tz_offset = 3 * 3600;

    BotService bot;
    bot_service_init(&bot, &cfg, &ts);

    /* Кнопка reply-клавиатуры приходит именно так — с заглавной «Н». */
    feed(&bot, "Напомнить");
    ok(is_remind_menu(), "«Напомнить» открывает меню напоминаний");
    ok(!is_help(), "«Напомнить» не падает в справку");
    ok(strstr(last_markup, "rm:cfg") != NULL, "в меню есть кнопка «Настройки»");
    ok(strstr(last_markup, "rm:off") != NULL, "в меню есть кнопка отмены");

    feed(&bot, "напомнить");
    ok(is_remind_menu(), "«напомнить» строчными тоже открывает меню");

    feed(&bot, "/remind");
    ok(is_remind_menu(), "/remind открывает меню");

    /* Обычный текст — справка и клавиатура учёта времени. */
    feed(&bot, "привет");
    ok(is_help(), "неизвестный текст — справка");
    ok(strstr(last_markup, "\"keyboard\"") != NULL,
       "справка идёт вместе с reply-клавиатурой");

    if (fails) printf("\nFAILURES: %d\n", fails);
    else printf("\nALL TESTS PASSED\n");
    return fails ? 1 : 0;
}

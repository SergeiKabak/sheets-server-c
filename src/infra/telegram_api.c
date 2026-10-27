#include "infra/telegram_api.h"

#include "core/json_text.h"
#include "core/url_codec.h"
#include "infra/http_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void telegram_send_text(const char *bot_token, long long chat_id, const char *text) {
    if (!bot_token || !*bot_token) return;
    char url[512];
    char *enc_text = url_encode(text ? text : "");
    char post[4096];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", bot_token);
    snprintf(post, sizeof(post), "chat_id=%lld&text=%s", chat_id,
             enc_text ? enc_text : "");
    free(enc_text);
    http_post_form(url, post);
}

void telegram_send_text_markup(const char *bot_token, long long chat_id,
                               const char *text, const char *markup_json) {
    if (!bot_token || !*bot_token) return;
    char url[512];
    char *enc_text = url_encode(text ? text : "");
    char *enc_markup = url_encode(markup_json ? markup_json : "");
    char post[8192];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", bot_token);
    snprintf(post, sizeof(post), "chat_id=%lld&text=%s&reply_markup=%s",
             chat_id, enc_text ? enc_text : "", enc_markup ? enc_markup : "");
    free(enc_text);
    free(enc_markup);
    http_post_form(url, post);
}

void telegram_answer_callback(const char *bot_token, const char *cq_id,
                               const char *text) {
    if (!bot_token || !*bot_token) return;
    if (!cq_id || !*cq_id) return;
    char url[512];
    char *enc_cq = url_encode(cq_id);
    char *enc_text = text ? url_encode(text) : NULL;
    char post[2048];
    snprintf(url, sizeof(url),
             "https://api.telegram.org/bot%s/answerCallbackQuery", bot_token);
    if (enc_text) {
        snprintf(post, sizeof(post), "callback_query_id=%s&text=%s",
                 enc_cq ? enc_cq : "", enc_text);
        free(enc_text);
    } else {
        snprintf(post, sizeof(post), "callback_query_id=%s", enc_cq ? enc_cq : "");
    }
    free(enc_cq);
    http_post_form(url, post);
}

int telegram_set_menu_button(const char *bot_token, const char *miniapp_url) {
    if (!bot_token || !*bot_token) return -1;
    if (!miniapp_url || !*miniapp_url) return -1;

    char *eu = json_escape(miniapp_url);
    char json[8192];
    char url[512];
    snprintf(json, sizeof(json),
        "{\"menu_button\":{\"type\":\"web_app\",\"text\":\"График\","
        "\"web_app\":{\"url\":\"%s\"}}}", eu ? eu : "");
    free(eu);
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/setChatMenuButton",
             bot_token);
    return http_post_json(url, json);
}

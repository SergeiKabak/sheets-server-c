#!/bin/bash
# Установка webhook для Telegram-бота.
#
# Использование:
#   ./set_webhook.sh https://ИМЯ-ПРИЛОЖЕНИЯ.onrender.com
#
# Токен берётся из переменной окружения BOT_TOKEN, либо из строки BOT_TOKEN= в .env
set -e

URL="${1:?Использование: $0 https://sheets-server-c.onrender.com}"

TOKEN="${BOT_TOKEN:-$(grep -E '^BOT_TOKEN=' .env 2>/dev/null | head -1 | cut -d= -f2-)}"
if [ -z "$TOKEN" ]; then
    echo "Ошибка: BOT_TOKEN не найден. Задайте переменную окружения BOT_TOKEN или строку BOT_TOKEN= в .env"
    exit 1
fi

echo "Устанавливаю webhook: ${URL}/webhook"
resp=$(curl -sS -X POST "https://api.telegram.org/bot${TOKEN}/setWebhook" \
          --data-urlencode "url=${URL}/webhook")
echo "$resp"
echo "$resp" | grep -q '"ok":true' && echo ">> Webhook успешно установлен."

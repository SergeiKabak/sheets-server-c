# --- ЭТАП 1: Сборка (Компиляция) ---
FROM gcc:14 AS builder

WORKDIR /build

COPY . .

RUN gcc -O2 -Wall -Wextra -o server_app main.c tracking.c

# --- ЭТАП 2: Финальный запуск ---
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl openssl && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY --from=builder /build/server_app .

# Переменные окружения (секреты) задаются на Render:
#   BOT_TOKEN, SPREADSHEET_ID, GOOGLE_API_KEY, FROM_COL, TO_COL, SEARCH_NAME, MINIAPP_URL, PORT
# Для учёта рабочего времени дополнительно:
#   TRACKING_SHEET_ID — ID таблицы "Time tracking"
#   GOOGLE_SERVICE_ACCOUNT_JSON — JSON-ключ Service Account (для записи в таблицу)
#   EMPLOYEE_MAP — {"chat_id":"ФИО"}
#   EMPLOYEE_RATES — {"ФИО":"500"}
#   HOURLY_RATE — ставка по умолчанию

EXPOSE 8080

CMD ["./server_app"]

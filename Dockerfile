# --- ЭТАП 1: Сборка (Компиляция) ---
FROM gcc:14 AS builder

WORKDIR /build

COPY . .

RUN gcc -O2 -Wall -Wextra -o server_app main.c

# --- ЭТАП 2: Финальный запуск ---
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl imagemagick librsvg2-bin fonts-dejavu-core && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY --from=builder /build/server_app .

# Переменные окружения (секреты) задаются на Render:
#   BOT_TOKEN, SPREADSHEET_ID, GOOGLE_API_KEY, FROM_COL, TO_COL, SEARCH_NAME, PORT

EXPOSE 8080

CMD ["./server_app"]

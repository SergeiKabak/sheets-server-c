# --- ЭТАП 1: Сборка (Компиляция) ---
FROM gcc:14 AS builder

WORKDIR /build

COPY . .

RUN gcc -O2 -o server_app main.c

# --- ЭТАП 2: Финальный запуск ---
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates curl && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY --from=builder /build/server_app .

EXPOSE 8080

CMD ["./server_app"]

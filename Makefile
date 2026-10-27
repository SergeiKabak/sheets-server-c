# Сборка сервера. Исходники лежат в src/ и разбиты по слоям:
#   src/core    — чистые функции (нет I/O, нет состояния)
#   src/domain  — модель предметной области
#   src/infra   — адаптеры внешнего мира (HTTP, Google, Supabase, Telegram, ФС)
#   src/app     — сценарии использования
#   src/web     — HTTP-обработчики
#   src/server  — приём соединений и маршрутизация
#   src/vendor  — сторонние библиотеки (cJSON, мини-шим curl)
CC_NATIVE = gcc
CFLAGS_NATIVE = -O2 -Wall -Wextra

# libcurl: на Linux докера лежит как libcurl.so.4 (GNU-имя), на macOS — libcurl.dylib
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
CURL_LIB = -lcurl
else
CURL_LIB = -l:libcurl.so.4
endif

# Кросс-компиляция для ARM (Render использует x86_64 — не подойдёт)
# Оставлено для истории, если понадобится ARM-сборка
CC_ARM = arm-linux-gnueabihf-gcc
CFLAGS_ARM = -Os -Wall -Wextra -ffunction-sections -fdata-sections
LDFLAGS_ARM = -Wl,--gc-sections -s

TARGET_ARM = sheet
TARGET_NATIVE = server_app

SRC = $(shell find src -name '*.c' | sort)
INCLUDES = -Isrc

.PHONY: all build run clean test test-js test-supabase test-core test-reminders

all: build

# Сборка нативного бинарника
build:
	$(CC_NATIVE) $(CFLAGS_NATIVE) $(INCLUDES) -o $(TARGET_NATIVE) $(SRC) $(CURL_LIB)
	@echo "Size:"
	@ls -lh $(TARGET_NATIVE)
	@file $(TARGET_NATIVE)

# Сборка ARM-бинарника (для справки)
arm:
	$(CC_ARM) $(CFLAGS_ARM) $(INCLUDES) -o $(TARGET_ARM) $(SRC) $(LDFLAGS_ARM) $(CURL_LIB)
	@echo "Size:"
	@ls -lh $(TARGET_ARM)
	@file $(TARGET_ARM)

# Запуск с подгрузкой .env
run: build
	@if [ -f .env ]; then \
		echo "[INFO] Загружаю .env"; \
		while IFS='=' read -r key val; do \
			[ -n "$$key" ] && export "$$key=$$val"; \
		done < .env && ./$(TARGET_NATIVE); \
	else \
		./$(TARGET_NATIVE); \
	fi

# Тесты (C-часть: нужен libcurl с GNU-именем -l:libcurl.so.4, т.е. Linux)
test: test-reminders
	bash tests/run.sh

# Тесты JS-части Mini App (нужны node и jsdom: npm i --no-save jsdom)
test-js:
	node tests/test_miniapp.js

# Юнит-тест буфера Supabase (офлайн, без сети)
test-supabase:
	$(CC_NATIVE) -O0 -g -Wall -Wextra $(INCLUDES) tests/test_supabase.c \
		src/infra/supabase.c src/infra/http_client.c src/core/json_text.c \
		src/core/strbuf.c src/core/num.c src/core/url_codec.c \
		src/core/shift.c src/core/date_time.c \
		src/domain/schedule.c src/domain/time_entry.c \
		src/vendor/cJSON.c -o /tmp/test_supabase $(CURL_LIB) && /tmp/test_supabase

# Юнит-тест планирования напоминаний (чистая логика, без сети/БД)
test-reminders:
	$(CC_NATIVE) -O0 -g -Wall -Wextra $(INCLUDES) tests/test_reminders.c \
		src/domain/reminder.c src/domain/schedule.c \
		src/core/json_text.c src/core/strbuf.c src/core/num.c \
		src/core/url_codec.c src/core/date_time.c src/core/shift.c \
		src/vendor/cJSON.c -o /tmp/test_reminders && /tmp/test_reminders

# Юнит-тесты чистого ядра (core + domain): без сети и без файлов
test-core:
	$(CC_NATIVE) -O0 -g -Wall -Wextra $(INCLUDES) tests/test_core.c \
		src/core/json_text.c src/core/strbuf.c src/core/num.c \
		src/core/url_codec.c src/core/date_time.c src/core/shift.c \
		src/domain/timesheet.c src/domain/schedule.c src/domain/time_entry.c \
		src/vendor/cJSON.c -o /tmp/test_core && /tmp/test_core

clean:
	rm -f $(TARGET_ARM) $(TARGET_NATIVE)

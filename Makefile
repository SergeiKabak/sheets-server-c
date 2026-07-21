# Кросс-компиляция для ARM (Render использует x86_64 — не подойдёт)
# Оставлено для истории, если понадобится ARM-сборка
CC_ARM = arm-linux-gnueabihf-gcc
CFLAGS_ARM = -Os -Wall -Wextra -ffunction-sections -fdata-sections
LDFLAGS_ARM = -Wl,--gc-sections -s

# Нативная компиляция для локального запуска
CC_NATIVE = gcc
CFLAGS_NATIVE = -O2 -Wall -Wextra

TARGET_ARM = server
TARGET_NATIVE = server_app
SRC = main.c

.PHONY: all build run clean

all: build

# Сборка нативного бинарника
build:
	$(CC_NATIVE) $(CFLAGS_NATIVE) -o $(TARGET_NATIVE) $(SRC)
	@echo "Size:"
	@ls -lh $(TARGET_NATIVE)
	@file $(TARGET_NATIVE)

# Сборка ARM-бинарника (для справки)
arm:
	$(CC_ARM) $(CFLAGS_ARM) -o $(TARGET_ARM) $(SRC) $(LDFLAGS_ARM)
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

clean:
	rm -f $(TARGET_ARM) $(TARGET_NATIVE)
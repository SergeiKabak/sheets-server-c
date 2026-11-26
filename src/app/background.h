/* app/background.h — фоновый цикл обновления кэшей. */
#ifndef SHEETS_APP_BACKGROUND_H
#define SHEETS_APP_BACKGROUND_H

#include "app/context.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Период обновления кэшей (секунды). */
#define CACHE_TTL 60
/* Раз в сутки: выгрузка БД -> Google (архив, включая правки дашборда
   в Supabase) и освежение кэша коллег. */
#define DAY_SYNC_TTL (24 * 3600)

/* Как часто фон раскладывает сегодняшние напоминания (секунды).
 * Раскладка — это несколько запросов в Supabase на каждого подписчика,
 * а слоты строятся сразу на весь день вперёд (domain/reminder.c), так что
 * 15 минут не теряют ни одного напоминания. */
#define REMINDER_PLAN_TTL (15 * 60)

/* Как часто переустанавливается кнопка меню чата (секунды).
 * setChatMenuButton нужен только чтобы восстановить доступ к Mini App
 * после сбоя: раз в сутки более чем достаточно (раньше был каждый тик —
 * ~43 000 POST в Telegram в месяц). */
#define MENU_BUTTON_TTL (24 * 3600)

/* Бесконечный цикл: обновляет график, буфер Supabase, кэш отметок и
 * выгружает накопленные изменения в Google. */
void background_run(AppContext *app);

#ifdef __cplusplus
}
#endif

#endif /* SHEETS_APP_BACKGROUND_H */

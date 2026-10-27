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

/* Бесконечный цикл: обновляет график, буфер Supabase, кэш отметок и
 * выгружает накопленные изменения в Google. */
void background_run(AppContext *app);

#ifdef __cplusplus
}
#endif

#endif /* SHEETS_APP_BACKGROUND_H */

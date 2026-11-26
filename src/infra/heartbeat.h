/* infra/heartbeat.h — метка последнего обхода Google (локальный файл).
 *
 * Раньше «фон жив и когда был последний обход» узнавали по строке в
 * sync_log каждые ~10 минут: это запись в БД и сетевой запрос ради одного
 * числа. Теперь пульс пишется в /tmp, а в sync_log попадают только ошибки
 * (см. docs/OPTIMIZE_BANDWIDTH.md, вариант B).
 *
 * Файл эфемерный: после рестарта контейнера метки нет, /diag/supabase честно
 * покажет «unavailable» до первого обхода. Это ожидаемо — журнал ошибок в БД
 * при этом сохраняется.
 */
#ifndef SHEETS_INFRA_HEARTBEAT_H
#define SHEETS_INFRA_HEARTBEAT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Путь метки (можно переопределить в тестах через HEARTBEAT_PATH). */
#define HEARTBEAT_FILE "/tmp/last_cycle"

typedef struct {
    long long at;   /* когда закончился обход (UTC, секунды с эпохи) */
    int rows;       /* строк графика в буфере (0 — ещё не выгружали) */
    int errs;       /* неудачных HTTP-вызовов в этом обходе */
} Heartbeat;

/* Пишет метку атомарно. rows < 0 — «неизвестно» (Supabase выключен). */
void heartbeat_write(long long at, int rows, int errs);

/* Читает метку. 0 — успех, -1 — метки нет/битая. */
int heartbeat_read(Heartbeat *out);

#ifdef __cplusplus
}
#endif

#endif /* SHEETS_INFRA_HEARTBEAT_H */

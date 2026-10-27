/* infra/file_store.h — работа с файлами кэша (эффект).
 *
 * Все кэши проекта пишутся атомарно (временный файл + rename): читатели
 * никогда не видят половину записи.
 */
#ifndef SHEETS_INFRA_FILE_STORE_H
#define SHEETS_INFRA_FILE_STORE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Атомарная запись файла. Возвращает 0 при успехе, -1 при ошибке. */
int file_write_atomic(const char *path, const char *data, size_t len);

/* Чтение файла целиком в буфер вызывающего.
 * Возвращает число прочитанных байт либо -1. */
long file_read_all(const char *path, char *out, size_t cap);

/* Свежесть файла: 1 — свежий, 0 — устаревший, -1 — файла нет.
 * max_age < 0 — возраст не проверяется (файл просто есть). */
int file_is_fresh(const char *path, int max_age);

/* Удаление файла (ошибки игнорируются). */
void file_remove(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* SHEETS_INFRA_FILE_STORE_H */

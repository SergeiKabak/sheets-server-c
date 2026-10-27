/* core/url_codec.h — кодирование/декодирование URL и разбор query-строки.
 *
 * Чистые функции: не ходят в сеть, не трогают глобальное состояние.
 * Раньше кодирование URL было продублировано в http.c и tracking.c,
 * а разбор query-строки жил внутри tracking.c.
 */
#ifndef SHEETS_CORE_URL_CODEC_H
#define SHEETS_CORE_URL_CODEC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Кодирует строку для application/x-www-form-urlencoded.
 * Возвращает malloc-ную строку (освобождает вызывающий) либо NULL. */
char *url_encode(const char *s);

/* Декодирует percent-escape и '+' -> ' ' в буфер вызывающего. */
void url_decode_into(char *dst, size_t dstcap, const char *src);

/* То же, но возвращает malloc-ную строку либо NULL. */
char *url_decode(const char *src);

/* Значение параметра query-строки ("a=1&b=2" -> "1" для имени "a").
 * Ищется ТОЛЬКО целое имя параметра: "in=" не находится внутри "min=".
 * Возвращает malloc-ную декодированную строку либо NULL, если параметра нет. */
char *url_query_param(const char *query, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* SHEETS_CORE_URL_CODEC_H */

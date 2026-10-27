/* core/strbuf.h — наращиваемый буфер строки.
 *
 * Заменяет ручную арифметику «offset + snprintf», из-за которой в коде
 * раньше появлялись переполнения буферов. Ошибки не копятся: любая
 * операция возвращает код, а буфер всегда остаётся корректной строкой.
 */
#ifndef SHEETS_CORE_STRBUF_H
#define SHEETS_CORE_STRBUF_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char  *data;     /* malloc-буфер, всегда '\0'-закрыт */
    size_t len;      /* длина строки без '\0' */
    size_t cap;      /* выделенная ёмкость (включая '\0') */
    int    failed;   /* 1, если произошла ошибка выделения/переполнения */
} StrBuf;

void sbuf_init(StrBuf *b);
void sbuf_free(StrBuf *b);

/* Добавляет строку (без форматирования). 0 — успех, -1 — ошибка. */
int sbuf_append(StrBuf *b, const char *s);

/* Добавляет первые len байт строки. 0 — успех, -1 — ошибка. */
int sbuf_append_len(StrBuf *b, const char *s, size_t len);

/* Добавляет фрагмент форматированной строки. 0 — успех, -1 — ошибка. */
int sbuf_appendf(StrBuf *b, const char *fmt, ...);

/* Добавляет строку как JSON-значение (с кавычками и экранированием). */
int sbuf_append_json_string(StrBuf *b, const char *s);

/* Отдаёт накопленную строку вызывающему (буфер освобождается).
 * Возвращает NULL, если в буфере была ошибка. */
char *sbuf_take(StrBuf *b);

#ifdef __cplusplus
}
#endif

#endif /* SHEETS_CORE_STRBUF_H */

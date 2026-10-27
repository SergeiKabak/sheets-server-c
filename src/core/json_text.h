/* core/json_text.h — работа с текстом JSON (чистые функции).
 *
 * Слой core не выполняет ввода-вывода и не хранит состояния:
 * все функции — «данные на входе -> данные на выходе».
 *
 * Здесь живёт ЕДИНСТВЕННОЕ экранирование строк для JSON на весь проект
 * (раньше его копировали трижды: main.c, tracking.c, supabase.c — с
 * разным поведением, из-за чего ответы API отличались друг от друга).
 */
#ifndef SHEETS_CORE_JSON_TEXT_H
#define SHEETS_CORE_JSON_TEXT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Экранирует строку для вставки внутрь JSON (без обрамляющих кавычек).
 * UTF-8 сохраняется как есть; управляющие символы -> \u00XX.
 * Возвращает malloc-ную строку (освобождает вызывающий) либо NULL. */
char *json_escape(const char *s);

/* То же, но пишет в буфер вызывающего.
 * Возвращает 0 при успехе, -1 если строка не поместилась (буфер при этом
 * обрезается и закрывается нулём). */
int json_escape_into(char *dst, size_t cap, const char *s);

/* JSON-строка целиком, вместе с кавычками: "текст".
 * Возвращает 0 при успехе, -1 при переполнении. */
int json_write_string(char *dst, size_t cap, const char *s);

/* Строковое поле JSON-документа (escape-последовательности раскодируются).
 * Возвращает malloc-ную строку либо NULL, если поля нет либо оно не строка. */
char *json_field_string(const char *json, const char *key);

/* Поле JSON, которое может быть числом или строкой (например "id" в Telegram).
 * Возвращает malloc-ную строку «как есть» либо NULL. */
char *json_field_raw(const char *json, const char *key);

#ifdef __cplusplus
}
#endif

#endif /* SHEETS_CORE_JSON_TEXT_H */

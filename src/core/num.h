/* core/num.h — разбор и форматирование чисел (чистые функции). */
#ifndef SHEETS_CORE_NUM_H
#define SHEETS_CORE_NUM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Число из строки; запятая трактуется как точка ("7,17" -> 7.17). */
double num_parse(const char *s);

/* Строка — корректное число? ("7.17", "-1", "12" — да; "", "abc" — нет). */
int num_is_valid(const char *s);

/* Печать числа с двумя знаками после запятой ("7.17"). */
void num_format2(double value, char *out, size_t cap);

/* Округление десятичной строки до двух знаков (половина вверх):
   "14.805" -> "14.81", "600" -> "600.00", "0,5" -> "0.50".
   0 — успех; -1 — не число или не хватает буфера. */
int num_round2_text(const char *s, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* SHEETS_CORE_NUM_H */

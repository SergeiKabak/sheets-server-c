/* infra/google_auth.h — OAuth2 Service Account -> access_token (эффект).
 *
 * Токен кэшируется дважды: в памяти процесса и в файле (чтобы пережить
 * fork-процессы сервера — каждый запрос обрабатывается отдельным процессом).
 */
#ifndef SHEETS_INFRA_GOOGLE_AUTH_H
#define SHEETS_INFRA_GOOGLE_AUTH_H

#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GOOGLE_TOKEN_CACHE_MAX 256

typedef struct {
    char  *sa_json;                        /* JSON ключа сервисного аккаунта */
    char  *token;                          /* текущий access_token */
    time_t token_exp;                      /* время истечения токена */
    char   token_cache_path[GOOGLE_TOKEN_CACHE_MAX];
} GoogleAuth;

/* Подготавливает авторизацию (читает ключ из окружения, если передан NULL). */
void google_auth_init(GoogleAuth *auth, const char *sa_json);

/* Access token. Возвращает NULL, если получить не удалось.
 * Кэш: память -> файл -> новый запрос к OAuth2. */
const char *google_auth_token(GoogleAuth *auth);

/* Сбрасывает кэш (и в памяти, и в файле). */
void google_auth_reset(GoogleAuth *auth);

/* Путь файла кэша токена (может быть переопределён переменной окружения). */
const char *google_auth_cache_path(void);

#ifdef __cplusplus
}
#endif

#endif /* SHEETS_INFRA_GOOGLE_AUTH_H */

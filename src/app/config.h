/* app/config.h — конфигурация приложения из переменных окружения.
 *
 * Конфигурация читается один раз при старте и больше не меняется:
 * это единственный источник правды о том, с какой таблицей и ботом
 * работает сервер. Раньше эти значения лежали в глобальных переменных
 * main.c, к которым тянулись все модули.
 */
#ifndef SHEETS_APP_CONFIG_H
#define SHEETS_APP_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *bot_token;        /* BOT_TOKEN */
    const char *spreadsheet_id;   /* SPREADSHEET_ID */
    const char *google_api_key;   /* GOOGLE_API_KEY */
    const char *from_col;         /* FROM_COL */
    const char *to_col;           /* TO_COL */
    const char *search_name;      /* SEARCH_NAME — сотрудник */
    const char *miniapp_url;      /* MINIAPP_URL */
} AppConfig;

/* Читает конфигурацию из окружения. Значения остаются валидными
 * (указатели на getenv-строки живут всё время работы процесса). */
void config_load(AppConfig *cfg);

/* Проверяет обязательные переменные. Возвращает 0 если всё на месте,
 * иначе печатает, чего не хватает, и возвращает -1. */
int config_validate(const AppConfig *cfg);

#ifdef __cplusplus
}
#endif

#endif /* SHEETS_APP_CONFIG_H */

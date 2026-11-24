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

/* Единый интервал обращения к Google Sheets по таймеру (секунды).
 * Полный обход графика — это запрос метаданных плюс запрос на каждый лист,
 * поэтому частый опрос превращается в гигабайты трафика в месяц и жжёт
 * квоту Google. Листы правят люди, а Mini App отвечает из кэшей и БД,
 * так что 30 минут — достаточная свежесть. */
#define GOOGLE_SYNC_TTL (30 * 60)

/* Рабочее окно локального времени (часы, [from, to)), когда фон ходит
 * в Google. Ночью (21:00–07:00) листы никто не правит — опрос выключен,
 * трафик и квота не тратятся. Пояс — тот же, что у TrackingService
 * (tz_offset, по умолчанию UTC+3). */
#define GOOGLE_SYNC_HOUR_FROM 7
#define GOOGLE_SYNC_HOUR_TO   21

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

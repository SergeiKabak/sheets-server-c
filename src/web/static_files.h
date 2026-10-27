/* web/static_files.h — раздача статики Mini App (web/). */
#ifndef SHEETS_WEB_STATIC_FILES_H
#define SHEETS_WEB_STATIC_FILES_H

#ifdef __cplusplus
extern "C" {
#endif

/* Всё без кэша: при деплое сразу новые HTML/CSS/JS. Повторный запрос дешёвый —
 * ETag даёт 304 без тела. Раньше css/js жили 5 минут, и после обновления
 * браузер мог смешать новый HTML со старыми стилями/скриптами. */
#define WEB_CACHE_HTML "no-cache"
#define WEB_CACHE      "no-cache"

/* Отдаёт файл из web/: Content-Type + ETag + Cache-Control.
 * req — исходный запрос: при совпадении If-None-Match с ETag отвечаем
 * 304 без тела. */
void static_serve(int fd, const char *rel, const char *cache, const char *req);

#ifdef __cplusplus
}
#endif

#endif /* SHEETS_WEB_STATIC_FILES_H */

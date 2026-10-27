/* web/static_files.h — раздача статики Mini App (web/). */
#ifndef SHEETS_WEB_STATIC_FILES_H
#define SHEETS_WEB_STATIC_FILES_H

#ifdef __cplusplus
extern "C" {
#endif

/* HTML — без кэша (при деплое сразу новый), css/js — с кэшем на 5 минут. */
#define WEB_CACHE_HTML "no-cache"
#define WEB_CACHE      "public, max-age=300"

/* Отдаёт файл из web/: Content-Type + ETag + Cache-Control.
 * req — исходный запрос: при совпадении If-None-Match с ETag отвечаем
 * 304 без тела. */
void static_serve(int fd, const char *rel, const char *cache, const char *req);

#ifdef __cplusplus
}
#endif

#endif /* SHEETS_WEB_STATIC_FILES_H */

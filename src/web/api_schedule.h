/* web/api_schedule.h — HTTP-обработчики графика смен (Mini App). */
#ifndef SHEETS_WEB_API_SCHEDULE_H
#define SHEETS_WEB_API_SCHEDULE_H

#include "app/context.h"

#ifdef __cplusplus
extern "C" {
#endif

void api_months(AppContext *app, int fd);
void api_month(AppContext *app, int fd, const char *path);
void api_cabinet(AppContext *app, int fd, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* SHEETS_WEB_API_SCHEDULE_H */

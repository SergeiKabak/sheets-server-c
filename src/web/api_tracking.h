/* web/api_tracking.h — HTTP-обработчики учёта рабочего времени. */
#ifndef SHEETS_WEB_API_TRACKING_H
#define SHEETS_WEB_API_TRACKING_H

#include "app/context.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Обрабатывает /api/tracking/... и /diag/tracking... */
void api_tracking_handle(AppContext *app, int fd, const char *method,
                          const char *path, const char *body);

#ifdef __cplusplus
}
#endif

#endif /* SHEETS_WEB_API_TRACKING_H */

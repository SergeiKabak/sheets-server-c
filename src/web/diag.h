/* web/diag.h — диагностические endpoints. */
#ifndef SHEETS_WEB_DIAG_H
#define SHEETS_WEB_DIAG_H

#include "app/context.h"

#ifdef __cplusplus
extern "C" {
#endif

/* /diag, /diag/cabinet, /diag/supabase */
void diag_handle(AppContext *app, int fd, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* SHEETS_WEB_DIAG_H */

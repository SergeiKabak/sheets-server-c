/* app/month_api.h — сценарии Mini App (график смен).
 *
 * Каждый сценарий возвращает готовый JSON-текст и код ответа.
 * Слой web только записывает их в сокет — никакой логики там.
 */
#ifndef SHEETS_APP_MONTH_API_H
#define SHEETS_APP_MONTH_API_H

#include "app/config.h"
#include "app/tracking_service.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GET /api/months -> {"months":["Октябрь 2026", ...]} */
int month_api_months_json(const AppConfig *cfg, char *out, size_t cap);

/* GET /api/month?name=... -> {"t":..,"s1":[..],"s2":[..],"trk":..} */
int month_api_month_json(TrackingService *svc, const char *query,
                          char *out, size_t cap);

/* GET /api/cabinet?month=..&cabinet=..&day=..&shift=..
 * -> {"cabinet":..,"month":..,"day":..,"shift":..,"colleagues":[..]} */
int month_api_cabinet_json(TrackingService *svc, const char *query,
                            char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* SHEETS_APP_MONTH_API_H */

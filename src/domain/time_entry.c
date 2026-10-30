#include "domain/time_entry.h"

#include "vendor/cJSON.h"
#include "core/date_time.h"
#include "core/json_text.h"
#include "core/num.h"
#include "core/strbuf.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int time_entry_days_json(const char *rows_json, char *out, size_t outcap) {
    if (!out || outcap < 8) return -1;
    out[0] = '\0';

    cJSON *arr = rows_json ? cJSON_Parse(rows_json) : NULL;
    if (!arr || !cJSON_IsArray(arr)) { cJSON_Delete(arr); return -1; }

    StrBuf sb;
    sbuf_init(&sb);
    sbuf_append(&sb, "{");

    int size = cJSON_GetArraySize(arr), n = 0;
    for (int i = 0; i < size; i++) {
        cJSON *it = cJSON_GetArrayItem(arr, i);
        cJSON *jd = it ? cJSON_GetObjectItemCaseSensitive(it, "day") : NULL;
        cJSON *ji = it ? cJSON_GetObjectItemCaseSensitive(it, "t_in") : NULL;
        cJSON *jo = it ? cJSON_GetObjectItemCaseSensitive(it, "t_out") : NULL;
        cJSON *jh = it ? cJSON_GetObjectItemCaseSensitive(it, "hours") : NULL;
        cJSON *ja = it ? cJSON_GetObjectItemCaseSensitive(it, "amount") : NULL;
        if (!cJSON_IsNumber(jd)) continue;
        int day = (int)jd->valuedouble;
        if (day < 1 || day > 31) continue;

        const char *in  = (cJSON_IsString(ji) && ji->valuestring) ? ji->valuestring : "";
        const char *out = (cJSON_IsString(jo) && jo->valuestring) ? jo->valuestring : "";
        /* формат Mini App: часы/сумма — строки, у пустых — "" */
        char hbuf[32] = "", sbuf[32] = "";
        if (cJSON_IsNumber(jh)) num_format2(jh->valuedouble, hbuf, sizeof(hbuf));
        if (cJSON_IsNumber(ja)) num_format2(ja->valuedouble, sbuf, sizeof(sbuf));

        sbuf_appendf(&sb, "%s\"%d\":{", n ? "," : "", day);
        sbuf_append(&sb, "\"in\":");
        sbuf_append_json_string(&sb, in);
        sbuf_append(&sb, ",\"out\":");
        sbuf_append_json_string(&sb, out);
        sbuf_append(&sb, ",\"hours\":");
        sbuf_append_json_string(&sb, hbuf);
        sbuf_append(&sb, ",\"sum\":");
        sbuf_append_json_string(&sb, sbuf);
        sbuf_append(&sb, "}");
        n++;
    }
    cJSON_Delete(arr);

    sbuf_append(&sb, "}");
    char *json = sbuf_take(&sb);
    if (!json) { out[0] = '\0'; return -1; }
    if (strlen(json) >= outcap) { free(json); out[0] = '\0'; return -1; }
    memcpy(out, json, strlen(json) + 1);
    free(json);
    return 0;
}

double time_entry_hours(const char *in_time, const char *out_time) {
    int in_min  = time_parse(in_time);
    int out_min = time_parse(out_time);
    if (in_min < 0 || out_min < 0) return 0.0;
    double hours = (out_min - in_min) / 60.0;
    if (hours < 0) hours += 24.0;
    return hours;
}

double time_entry_sum(const char *hours, double rate) {
    /* Деньги считаем в копейках, чтобы результат всегда был ровно
       с двумя знаками (округление половины вверх):
       часы(2 знака) × ставка(2 знака) = 4 знака -> копейки. */
    long long h2 = llround(num_parse(hours) * 100.0);
    long long r2 = llround(rate * 100.0);
    if (h2 < 0) h2 = 0;
    if (r2 < 0) r2 = 0;
    long long cents = (h2 * r2 + 50) / 100;
    return (double)cents / 100.0;
}

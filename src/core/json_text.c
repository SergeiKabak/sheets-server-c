#include "core/json_text.h"

#include "vendor/cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Печатает один байт в hex-виде: \u00XX */
static void put_u_escape(char *dst, size_t *o, unsigned char c) {
    dst[(*o)++] = '\\';
    dst[(*o)++] = 'u';
    dst[(*o)++] = '0';
    dst[(*o)++] = '0';
    static const char HEX[] = "0123456789abcdef";
    dst[(*o)++] = HEX[(c >> 4) & 0x0F];
    dst[(*o)++] = HEX[c & 0x0F];
}

int json_escape_into(char *dst, size_t cap, const char *s) {
    if (!dst || cap == 0) return -1;
    if (!s) s = "";

    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        char esc[8];
        size_t n;

        switch (c) {
            case '"':  esc[0] = '\\'; esc[1] = '"';  n = 2; break;
            case '\\': esc[0] = '\\'; esc[1] = '\\'; n = 2; break;
            case '\n': esc[0] = '\\'; esc[1] = 'n';  n = 2; break;
            case '\r': esc[0] = '\\'; esc[1] = 'r';  n = 2; break;
            case '\t': esc[0] = '\\'; esc[1] = 't';  n = 2; break;
            default:
                if (c < 0x20) {
                    if (o + 6 > cap) goto overflow;
                    put_u_escape(dst, &o, c);
                    continue;
                }
                esc[0] = (char)c;
                n = 1;
                break;
        }
        if (o + n > cap) goto overflow;
        memcpy(dst + o, esc, n);
        o += n;
    }
    dst[o] = '\0';
    return 0;

overflow:
    dst[o] = '\0';
    return -1;
}

char *json_escape(const char *s) {
    if (!s) s = "";
    size_t len = strlen(s);
    /* худший случай: каждый байт -> 6 символов (\u00XX) */
    char *out = malloc(len * 6 + 16);
    if (!out) return NULL;
    if (json_escape_into(out, len * 6 + 16, s) != 0) {
        free(out);
        return NULL;
    }
    return out;
}

int json_write_string(char *dst, size_t cap, const char *s) {
    if (!dst || cap < 3) return -1;
    dst[0] = '"';
    if (json_escape_into(dst + 1, cap - 2, s) != 0) {
        dst[0] = '\0';
        return -1;
    }
    size_t len = strlen(dst);
    if (len + 1 >= cap) {
        dst[0] = '\0';
        return -1;
    }
    dst[len] = '"';
    dst[len + 1] = '\0';
    return 0;
}

char *json_field_string(const char *json, const char *key) {
    if (!json || !key) return NULL;
    cJSON *root = cJSON_Parse(json);
    if (!root) return NULL;
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    char *out = NULL;
    if (cJSON_IsString(item) && item->valuestring)
        out = strdup(item->valuestring);
    cJSON_Delete(root);
    return out;
}

char *json_field_raw(const char *json, const char *key) {
    if (!json || !key) return NULL;
    cJSON *root = cJSON_Parse(json);
    if (!root) return NULL;
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    char *out = NULL;
    if (cJSON_IsString(item) && item->valuestring) {
        out = strdup(item->valuestring);
    } else if (cJSON_IsNumber(item)) {
        char buf[64];
        double v = item->valuedouble;
        /* целые значения (например Telegram user.id) печатаем без дробной
           части — иначе теряются «хвостовые» разряды double */
        if (v == (double)(long long)v && v < 1e15 && v > -1e15)
            snprintf(buf, sizeof(buf), "%lld", (long long)v);
        else
            snprintf(buf, sizeof(buf), "%.17g", v);
        out = strdup(buf);
    } else if (cJSON_IsBool(item) || cJSON_IsNull(item)) {
        out = strdup(item->type == cJSON_True ? "true" :
                      item->type == cJSON_False ? "false" : "null");
    }
    cJSON_Delete(root);
    return out;
}

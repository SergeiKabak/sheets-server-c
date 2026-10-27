#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "curl_mini.h"

/* ---------- URL-кодирование для application/x-www-form-urlencoded ---------- */
char *url_encode(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char *out = malloc(len * 3 + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = c;
        } else {
            o += snprintf(out + o, 4, "%%%02X", c);
        }
    }
    out[o] = '\0';
    return out;
}

/* ---------- коллбэк записи тела ответа ---------- */
static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t total = size * nmemb;
    char **buf = (char **)userdata;
    size_t cur = strlen(*buf);
    size_t need = cur + total + 1;
    char *tmp = realloc(*buf, need);
    if (!tmp) return 0;
    *buf = tmp;
    memcpy(*buf + cur, ptr, total);
    (*buf)[cur + total] = '\0';
    return total;
}

static size_t header_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    (void)ptr; (void)size; (void)nmemb; (void)userdata;
    return size * nmemb;
}

/* ---------- вспомогательная функция для установки http_code ---------- */
static void set_http_code(long http_code, int *status) {
    if (status) {
        *status = (int)http_code;
        /* Если сервер не вернул код, но тело пришло — считаем 200 */
        if (*status == 0) *status = 200;
    }
}

/* ---------- GET запрос ---------- */
char *http_get(const char *url, int *status) {
    if (status) *status = 0;
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    char *buf = malloc(1);
    if (!buf) { curl_easy_cleanup(curl); return NULL; }
    buf[0] = '\0';

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 25L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);

    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        free(buf);
        return NULL;
    }
    set_http_code(http_code, status);
    return buf;
}

/* ---------- POST с application/x-www-form-urlencoded ---------- */
int http_post_form(const char *url, const char *post_fields) {
    CURL *curl = curl_easy_init();
    if (!curl) return 0;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_fields);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 25L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);

    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    set_http_code(http_code, NULL);
    return (rc == CURLE_OK) ? (int)http_code : 0;
}

/* ---------- POST с application/json ---------- */
int http_post_json(const char *url, const char *json) {
    CURL *curl = curl_easy_init();
    if (!curl) return 0;

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 25L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);

    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    set_http_code(http_code, NULL);
    return (rc == CURLE_OK) ? (int)http_code : 0;
}

/* ---------- Универсальный запрос ---------- */
int http_request(const char *method, const char *url,
                 const char *content_type, const char *body,
                 const char *bearer, int *status) {
    if (status) *status = 0;
    CURL *curl = curl_easy_init();
    if (!curl) return 0;

    struct curl_slist *headers = NULL;
    if (content_type) {
        char h[256];
        snprintf(h, sizeof(h), "Content-Type: %s", content_type);
        headers = curl_slist_append(headers, h);
    }
    if (bearer && *bearer) {
        char h[512];
        snprintf(h, sizeof(h), "Authorization: Bearer %s", bearer);
        headers = curl_slist_append(headers, h);
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    if (body) curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 25L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);

    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    set_http_code(http_code, status);
    return (rc == CURLE_OK) ? (int)http_code : 0;
}

/* Универсальный запрос с дополнительными заголовками.
   extra — NULL-terminated массив строк "Name: value". */
char *http_request_h(const char *method, const char *url,
                 const char *content_type, const char *body,
                 const char *const *extra, int *status) {
    if (status) *status = 0;
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    struct curl_slist *headers = NULL;
    if (content_type) {
        char h[256];
        snprintf(h, sizeof(h), "Content-Type: %s", content_type);
        headers = curl_slist_append(headers, h);
    }
    if (extra)
        for (int i = 0; extra[i]; i++)
            headers = curl_slist_append(headers, extra[i]);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    if (body) curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 25L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);

    char *buf = malloc(1);
    if (!buf) { curl_slist_free_all(headers); curl_easy_cleanup(curl); return NULL; }
    buf[0] = '\0';
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);

    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        free(buf);
        return NULL;
    }
    set_http_code(http_code, status);
    return buf;
}

/* Универсальный запрос с возвратом тела ответа и HTTP-кода.
   caller отвечает за free(out). */
char *http_request_full(const char *method, const char *url,
                 const char *content_type, const char *body,
                 const char *bearer, int *status) {
    char auth[520];
    const char *extra[2] = { NULL, NULL };
    if (bearer && *bearer) {
        snprintf(auth, sizeof(auth), "Authorization: Bearer %s", bearer);
        extra[0] = auth;
    }
    return http_request_h(method, url, content_type, body, extra, status);
}

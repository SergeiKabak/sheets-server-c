/* Минимальный шим <curl/curl.h> для сборки без libcurl-dev.
   Значения сверены с настоящим curl.h (macOS/Xcode 17, libcurl 8.7.1):
   5 констант были неверны — из-за этого CURLINFO_RESPONSE_CODE не
   распознавался, код HTTP-ответа всегда терялся (0 -> «200» в set_http_code)
   и все проверки status == 200 в коде видели всегда успешный ответ. */
#ifndef CURL_MINI_H
#define CURL_MINI_H

#include <stddef.h>

typedef struct CURL CURL;

typedef int CURLcode;
#define CURLE_OK 0

typedef enum {
    CURLOPT_OK = 0,
    CURLOPT_URL = 10002,            /* OBJECTPOINT + 2 */
    CURLOPT_WRITEFUNCTION = 20011,  /* FUNCTIONPOINT + 11 */
    CURLOPT_WRITEDATA = 10001,      /* OBJECTPOINT + 1 */
    CURLOPT_HEADERFUNCTION = 20079, /* FUNCTIONPOINT + 79 */
    CURLOPT_HEADERDATA = 10029,     /* OBJECTPOINT + 29 */
    CURLOPT_POSTFIELDS = 10015,     /* OBJECTPOINT + 15 */
    CURLOPT_POSTFIELDSIZE = 60,     /* LONG + 60 */
    CURLOPT_HTTPHEADER = 10023,     /* OBJECTPOINT + 23 */
    CURLOPT_HTTPHEADER_OLD = 10023,
    CURLOPT_CUSTOMREQUEST = 10036,  /* OBJECTPOINT + 36 */
    CURLOPT_SSL_VERIFYPEER = 64,    /* LONG + 64 */
    CURLOPT_TIMEOUT = 13,           /* LONG + 13 */
    CURLOPT_FOLLOWLOCATION = 52,    /* LONG + 52 */
    CURLOPT_HTTPGET = 80,           /* LONG + 80 */
    CURLOPT_POST = 47               /* LONG + 47 */
} CURLoption;

typedef enum {
    CURLINFO_RESPONSE_CODE = 2097154  /* CURLINFO_LONG(0x200000) | 2 */
} CURLINFO;

struct curl_slist {
    char *data;
    struct curl_slist *next;
};

extern CURL *curl_easy_init(void);
extern CURLcode curl_easy_setopt(CURL *curl, CURLoption option, ...);
extern CURLcode curl_easy_perform(CURL *curl);
extern void curl_easy_cleanup(CURL *curl);
extern const char *curl_easy_strerror(CURLcode code);
extern long curl_easy_getinfo(CURL *curl, CURLINFO info, ...);
extern struct curl_slist *curl_slist_append(struct curl_slist *, const char *);
extern void curl_slist_free_all(struct curl_slist *);

#endif /* CURL_MINI_H */

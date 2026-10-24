#ifndef CURL_MINI_H
#define CURL_MINI_H

#include <stddef.h>

typedef struct CURL CURL;

typedef int CURLcode;
#define CURLE_OK 0

typedef enum {
    CURLOPT_OK = 0,
    CURLOPT_URL = 10002,
    CURLOPT_WRITEFUNCTION = 20011,
    CURLOPT_WRITEDATA = 10001,
    CURLOPT_HEADERFUNCTION = 20027,
    CURLOPT_HEADERDATA = 10029,
    CURLOPT_POSTFIELDS = 10015,
    CURLOPT_POSTFIELDSIZE = 10014,
    CURLOPT_HTTPHEADER = 10023,
    CURLOPT_HTTPHEADER_OLD = 10023,
    CURLOPT_CUSTOMREQUEST = 10036,
    CURLOPT_SSL_VERIFYPEER = 81,
    CURLOPT_TIMEOUT = 13,
    CURLOPT_FOLLOWLOCATION = 52,
    CURLOPT_HTTPGET = 80,
    CURLOPT_POST = 41
} CURLoption;

typedef enum {
    CURLINFO_RESPONSE_CODE = 2097
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

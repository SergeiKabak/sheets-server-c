#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <time.h>
#include <fcntl.h>

#define CACHE_TTL 60
#define BUFFER_SIZE (512 * 1024)
#define MAX_SHEETS 50
#define MAX_SHEET_NAME 128
#define CACHE_FILE "/tmp/sheets_cache.html"

static const char *SPREADSHEET_ID;
static const char *GOOGLE_API_KEY;
static const char *FROM_COL;
static const char *TO_COL;
static const char *SEARCH_NAME;

static void init_config(void) {
    SPREADSHEET_ID = getenv("SPREADSHEET_ID");
    GOOGLE_API_KEY = getenv("GOOGLE_API_KEY");
    FROM_COL = getenv("FROM_COL");
    TO_COL = getenv("TO_COL");
    SEARCH_NAME = getenv("SEARCH_NAME");
}

static char *fetch_url(const char *url) {
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "curl -sS --max-time 10 '%s'", url);
    
    FILE *fp = popen(cmd, "r");
    if (!fp) return NULL;
    
    char *buf = malloc(BUFFER_SIZE);
    if (!buf) { pclose(fp); return NULL; }
    
    size_t total = 0;
    size_t n;
    while ((n = fread(buf + total, 1, BUFFER_SIZE - total - 1, fp)) > 0) {
        total += n;
        if (total >= BUFFER_SIZE - 1) break;
    }
    buf[total] = '\0';
    
    int status = pclose(fp);
    if (status != 0 || total == 0) {
        free(buf);
        return NULL;
    }
    return buf;
}

static const char *find_key_bounded(const char *json, size_t len, const char *key) {
    char search[256];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *end = json + len;
    const char *p = json;
    while (p < end) {
        const char *found = strstr(p, search);
        if (!found || found >= end) return NULL;
        const char *val = found + strlen(search);
        while (val < end && *val && *val != ':') val++;
        if (val < end && *val == ':') val++;
        while (val < end && *val && (*val == ' ' || *val == '\t')) val++;
        if (val < end) return val;
        p = found + 1;
    }
    return NULL;
}

static char *url_encode(const char *src) {
    size_t len = strlen(src);
    char *dst = malloc(len * 3 + 1);
    if (!dst) return NULL;
    char *d = dst;
    for (const unsigned char *s = (const unsigned char *)src; *s; s++) {
        if ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') ||
            (*s >= '0' && *s <= '9') || *s == '-' || *s == '_' || *s == '.' || *s == '~') {
            *d++ = *s;
        } else {
            d += sprintf(d, "%%%02X", *s);
        }
    }
    *d = '\0';
    return dst;
}

static char *extract_string(const char *pos) {
    if (!pos || *pos != '"') return NULL;
    pos++;
    const char *end = pos;
    while (*end && *end != '"') {
        if (*end == '\\') end++;
        end++;
    }
    int len = end - pos;
    char *s = malloc(len + 1);
    if (!s) return NULL;
    memcpy(s, pos, len);
    s[len] = '\0';
    return s;
}

static int is_true(const char *pos) {
    return (pos && strncmp(pos, "true", 4) == 0);
}

static int get_visible_sheets(char sheets[][MAX_SHEET_NAME], int max_sheets) {
    char url[1024];
    snprintf(url, sizeof(url),
        "https://sheets.googleapis.com/v4/spreadsheets/%s"
        "?fields=sheets.properties(sheetId,title,hidden)&key=%s",
        SPREADSHEET_ID, GOOGLE_API_KEY);
    
    char *response = fetch_url(url);
    if (!response) return 0;
    
    int count = 0;
    const char *p = response;
    
    while ((p = strstr(p, "\"properties\"")) && count < max_sheets) {
        p += 12;
        while (*p && *p != '{') p++;
        if (!*p) break;
        
        const char *prop_start = p;
        int brace = 1;
        const char *prop_end = prop_start + 1;
        while (*prop_end && brace > 0) {
            if (*prop_end == '{') brace++;
            if (*prop_end == '}') brace--;
            prop_end++;
        }
        size_t prop_len = prop_end - prop_start;
        
        const char *hidden_pos = find_key_bounded(prop_start, prop_len, "hidden");
        if (hidden_pos && is_true(hidden_pos)) {
            p = prop_end;
            continue;
        }
        
        const char *title_pos = find_key_bounded(prop_start, prop_len, "title");
        char *title = extract_string(title_pos);
        if (title) {
            strncpy(sheets[count], title, MAX_SHEET_NAME - 1);
            sheets[count][MAX_SHEET_NAME - 1] = '\0';
            free(title);
            count++;
        }
        
        p = prop_end;
    }
    
    free(response);
    return count;
}

static int extract_string_array(const char *pos, char ***out) {
    if (!pos || *pos != '[') return 0;
    pos++;
    
    int cap = 16;
    char **arr = malloc(sizeof(char *) * cap);
    int count = 0;
    
    while (*pos && *pos != ']') {
        if (*pos == '"') {
            char *s = extract_string(pos);
            if (s) {
                if (count >= cap) {
                    cap *= 2;
                    arr = realloc(arr, sizeof(char *) * cap);
                }
                arr[count++] = s;
                pos++;
                while (*pos && *pos != '"') {
                    if (*pos == '\\') pos++;
                    pos++;
                }
            }
        }
        if (*pos) pos++;
    }
    
    *out = arr;
    return count;
}

static void free_string_array(char **arr, int count) {
    for (int i = 0; i < count; i++) free(arr[i]);
    free(arr);
}

/* Parse a JSON array of arrays (rows) from the "values" field.
   Returns the number of rows parsed. Each row is a string array. */
static int parse_rows(const char *json, char ***rows_out, int **row_lens_out) {
    if (!json) return 0;
    
    /* Find the outer array start */
    const char *outer = strchr(json, '[');
    if (!outer) return 0;
    outer++;
    
    int cap = 32;
    char **rows = malloc(sizeof(char *) * cap);
    int *lens = malloc(sizeof(int) * cap);
    int count = 0;
    
    while (*outer && *outer != ']') {
        /* Skip whitespace */
        while (*outer && (*outer == ' ' || *outer == '\t' || *outer == '\n')) outer++;
        if (!*outer || *outer == ']') break;
        
        if (*outer == '[') {
            char **arr = NULL;
            int len = extract_string_array(outer, &arr);
            if (len > 0) {
                if (count >= cap) {
                    cap *= 2;
                    rows = realloc(rows, sizeof(char *) * cap);
                    lens = realloc(lens, sizeof(int) * cap);
                }
                rows[count] = (char *)arr;
                lens[count] = len;
                count++;
            } else {
                free(arr);
            }
            
            /* Advance past this inner array */
            int brace = 0;
            while (*outer) {
                if (*outer == '[') brace++;
                if (*outer == ']') brace--;
                outer++;
                if (brace == 0) break;
            }
        } else {
            outer++;
        }
    }
    
    *rows_out = rows;
    *row_lens_out = lens;
    return count;
}

static void free_rows(char **rows, int *lens, int count) {
    for (int i = 0; i < count; i++) {
        free_string_array((char **)rows[i], lens[i]);
    }
    free(rows);
    free(lens);
}

static void build_and_save_cache(void) {
    char sheets[MAX_SHEETS][MAX_SHEET_NAME];
    int sheet_count = get_visible_sheets(sheets, MAX_SHEETS);
    
    size_t cap = 65536;
    char *html = malloc(cap);
    size_t len = 0;
    
    #define APPEND(fmt, ...) do { \
        int n = snprintf(html + len, cap - len, fmt, ##__VA_ARGS__); \
        if (n > 0) len += n; \
        if (len + 4096 > cap) { cap *= 2; html = realloc(html, cap); } \
    } while(0)
    
    APPEND("<!DOCTYPE html><html><head><meta charset=\"UTF-8\">"
           "<style>body{font-family:Arial,sans-serif;margin:32px}"
           "th{background-color:#4CAF50;color:white}"
           "tr:nth-child(even){background-color:#f2f2f2}"
           "tr:hover{background-color:#ddd}"
           "td,th{text-align:center}</style></head><body>");
    
    if (sheet_count == 0) {
        APPEND("<p style='color:red'>Нет данных</p>");
    } else {
        for (int i = sheet_count - 1; i >= 0; i--) {
            /* Запрашиваем весь диапазон C:AJ — API вернёт только непустые строки */
            char range[256];
            snprintf(range, sizeof(range), "%s!%s:%s", sheets[i], FROM_COL, TO_COL);
            
            char *encoded_range = url_encode(range);
            char url[2048];
            snprintf(url, sizeof(url),
                "https://sheets.googleapis.com/v4/spreadsheets/%s/values/%s?key=%s",
                SPREADSHEET_ID, encoded_range, GOOGLE_API_KEY);
            free(encoded_range);
            
            char *response = fetch_url(url);
            if (!response) continue;
            
            const char *values_pos = strstr(response, "\"values\"");
            if (!values_pos) { free(response); continue; }
            
            char **rows = NULL;
            int *row_lens = NULL;
            int row_count = parse_rows(values_pos, &rows, &row_lens);
            
            if (row_count == 0) { free(response); continue; }
            
            /* Ищем строку, где первый элемент (колонка C) содержит искомое имя */
            int found_idx = -1;
            for (int r = 0; r < row_count; r++) {
                char **row = (char **)rows[r];
                int rlen = row_lens[r];
                if (rlen > 0 && row[0] && strcmp(row[0], SEARCH_NAME) == 0) {
                    found_idx = r;
                    break;
                }
            }
            
            if (found_idx < 0) {
                free_rows(rows, row_lens, row_count);
                free(response);
                continue;
            }
            
            /* Берём найденную строку и следующую за ней */
            char **shift1 = (char **)rows[found_idx];
            int shift1_len = row_lens[found_idx];
            
            char **shift2 = NULL;
            int shift2_len = 0;
            if (found_idx + 1 < row_count) {
                shift2 = (char **)rows[found_idx + 1];
                shift2_len = row_lens[found_idx + 1];
            }
            
            APPEND("<table border='1' cellpadding='8' cellspacing='8' "
                   "style='border-collapse:collapse;width:100%%'>"
                   "<tr><td><strong>%s</strong></td>", sheets[i]);
            
            if (shift1_len > 0) {
                int j = shift1_len >= 2 ? shift1_len - 2 : 0;
                for (int k = 0; k < shift1_len; k++) {
                    if (k == j) {
                        APPEND("<th>Смен</th>");
                        break;
                    }
                    APPEND("<th>%d</th>", k);
                }
                APPEND("</tr><tr>");
                
                for (int k = 0; k < shift1_len; k++) {
                    APPEND("<td>%s</td>", shift1[k] ? shift1[k] : "");
                }
            }
            
            APPEND("</tr><tr>");
            
            for (int k = 0; k < shift2_len; k++) {
                APPEND("<td>%s</td>", shift2[k] ? shift2[k] : "");
            }
            
            APPEND("</tr></table><br><br>");
            
            free_rows(rows, row_lens, row_count);
            free(response);
        }
    }
    
    APPEND("</body></html>");
    
    FILE *fp = fopen(CACHE_FILE, "w");
    if (fp) {
        fwrite(html, 1, len, fp);
        fclose(fp);
    }
    
    free(html);
}

static void background_updater(void) {
    while (1) {
        build_and_save_cache();
        sleep(CACHE_TTL);
    }
}

static char *read_cache_file(size_t *out_len) {
    FILE *fp = fopen(CACHE_FILE, "r");
    if (!fp) return NULL;
    
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    if (size <= 0) { fclose(fp); return NULL; }
    
    char *buf = malloc(size + 1);
    if (!buf) { fclose(fp); return NULL; }
    
    size_t n = fread(buf, 1, size, fp);
    buf[n] = '\0';
    *out_len = n;
    fclose(fp);
    return buf;
}

static void send_response(int client_fd, const char *body, size_t body_len) {
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=UTF-8\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n", body_len);
    
    write(client_fd, header, hlen);
    write(client_fd, body, body_len);
}

int main(void) {
    int port = 8080;
    char *port_env = getenv("PORT");
    if (port_env) {
        port = atoi(port_env);
    }
    init_config();

    if (!SPREADSHEET_ID || !GOOGLE_API_KEY || !SEARCH_NAME) {
        fprintf(stderr, "[ERROR] Обязательные переменные окружения не заданы:\n");
        if (!SPREADSHEET_ID) fprintf(stderr, "  - SPREADSHEET_ID\n");
        if (!GOOGLE_API_KEY) fprintf(stderr, "  - GOOGLE_API_KEY\n");
        if (!SEARCH_NAME)    fprintf(stderr, "  - SEARCH_NAME\n");
        return 1;
    }

    printf("[INFO] Сервер запускается на порту: %d\n", port);
    printf("[INFO] SEARCH_NAME = %s\n", SEARCH_NAME);
    
    pid_t pid = fork();
    if (pid == 0) {
        background_updater();
        exit(0);
    }
    
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }
    
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(port)
    };
    
    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    
    if (listen(server_fd, 10) < 0) {
        perror("listen");
        return 1;
    }
    
    
    
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) continue;
        
        size_t html_len = 0;
        char *html = read_cache_file(&html_len);
        
        if (html) {
            send_response(client_fd, html, html_len);
            free(html);
        } else {
            const char *err = "<html><body>Cache not ready</body></html>";
            send_response(client_fd, err, strlen(err));
        }
        
        shutdown(client_fd, SHUT_WR);
        close(client_fd);
    }
    
    close(server_fd);
    return 0;
}
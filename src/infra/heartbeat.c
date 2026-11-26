#include "infra/heartbeat.h"

#include "infra/file_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *hb_path(void) {
    const char *p = getenv("HEARTBEAT_PATH");
    return (p && *p) ? p : HEARTBEAT_FILE;
}

void heartbeat_write(long long at, int rows, int errs) {
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%lld %d %d\n", at, rows, errs);
    if (n <= 0 || (size_t)n >= sizeof(buf)) return;
    file_write_atomic(hb_path(), buf, (size_t)n);
}

int heartbeat_read(Heartbeat *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    char buf[64];
    long n = file_read_all(hb_path(), buf, sizeof(buf));
    if (n <= 0) return -1;
    long long at = 0;
    int rows = 0, errs = 0;
    if (sscanf(buf, "%lld %d %d", &at, &rows, &errs) != 3 || at <= 0) return -1;
    out->at = at;
    out->rows = rows;
    out->errs = errs;
    return 0;
}

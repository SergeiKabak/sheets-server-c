#include "infra/file_store.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int file_write_atomic(const char *path, const char *data, size_t len) {
    if (!path || !data) return -1;
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.%d.tmp", path, (int)getpid());
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    if (len && fwrite(data, 1, len, f) != len) {
        fclose(f);
        remove(tmp);
        return -1;
    }
    fclose(f);
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return -1;
    }
    return 0;
}

long file_read_all(const char *path, char *out, size_t cap) {
    if (!path || !out || cap == 0) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t got = fread(out, 1, cap - 1, f);
    fclose(f);
    out[got] = '\0';
    return (long)got;
}

int file_is_fresh(const char *path, int max_age) {
    if (!path) return -1;
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    if (max_age >= 0 && time(NULL) - st.st_mtime > max_age) return 0;
    return 1;
}

void file_remove(const char *path) {
    if (path) remove(path);
}

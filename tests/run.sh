#!/usr/bin/env bash
# Локальный тест заполнения Time tracking без обращения к Google API.
set -euo pipefail
cd "$(dirname "$0")/.."

key="$(mktemp /tmp/trk_test_key.XXXXXX.pem)"
trap 'rm -f "$key" /tmp/test_tracking_fill' EXIT
openssl genrsa -out "$key" 2048 2>/dev/null

# HTTP-слой подменяется заглушками внутри теста, поэтому http_client.c
# в сборку не входит.
gcc -O0 -g -Isrc -Wno-unused-function -Wno-unused-parameter \
    tests/test_tracking_fill.c \
    src/app/tracking_service.c src/app/config.c \
    src/app/tracking_cache.c \
    src/infra/supabase.c src/infra/google_auth.c src/infra/google_sheets.c \
    src/infra/file_store.c src/infra/clock.c \
    src/core/json_text.c src/core/strbuf.c src/core/num.c src/core/url_codec.c \
    src/core/date_time.c src/core/shift.c \
    src/domain/timesheet.c src/domain/schedule.c src/domain/time_entry.c \
    src/vendor/cJSON.c \
    -o /tmp/test_tracking_fill -lcurl
/tmp/test_tracking_fill "$key"

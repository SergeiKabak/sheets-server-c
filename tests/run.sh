#!/usr/bin/env bash
# Локальный тест заполнения Time tracking без обращения к Google API.
set -euo pipefail
cd "$(dirname "$0")/.."

key="$(mktemp /tmp/trk_test_key.XXXXXX.pem)"
trap 'rm -f "$key" /tmp/test_tracking_fill' EXIT
openssl genrsa -out "$key" 2048 2>/dev/null
gcc -O0 -g -I. -Wno-unused-function -Wno-unused-parameter \
    tests/test_tracking_fill.c supabase.c cJSON.c -o /tmp/test_tracking_fill -l:libcurl.so.4
/tmp/test_tracking_fill "$key"

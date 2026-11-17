#!/usr/bin/env bash
# Host tests for Dial Racer's logic (no device, no ESP-IDF needed).
set -euo pipefail
cd "$(dirname "$0")"
OUT=$(mktemp -d)
cc -std=c11 -Wall -Wextra -Werror -O1 -o "$OUT/test_racer" test_racer.c ../main/racer.c -lm
"$OUT/test_racer"
rm -rf "$OUT"

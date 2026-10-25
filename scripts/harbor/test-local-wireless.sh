#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 tobagin
# SPDX-License-Identifier: AGPL-3.0-only
set -euo pipefail
source "$(dirname "$0")/common.sh"
mkdir -p "$SCR/tmp/local-wireless-test"
locked "${CXX:-c++}" -std=c++20 -Wall -Wextra -Wconversion -Werror -pthread \
    -I "$SRC/src" "$SRC/scripts/harbor/tests/local_wireless.cpp" \
    "$SRC/src/core/hle/service/ldn/local_wireless_transport.cpp" \
    -o "$SCR/tmp/local-wireless-test/check"
"$SCR/tmp/local-wireless-test/check"

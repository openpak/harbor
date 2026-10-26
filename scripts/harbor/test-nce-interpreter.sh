#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 tobagin
# SPDX-License-Identifier: AGPL-3.0-only
set -euo pipefail
. "$(dirname "$0")/common.sh"

# Exercise the real interpreter and decoder with tracked-memory callbacks and a
# portable signal-context fixture; no ARM hardware or game content is required.
TEST_SOURCE="${HARBOR_NCE_SOURCE:-$SRC}"
DYNARMIC_SOURCE="${HARBOR_DYNARMIC_SOURCE:-$SRC/src/dynarmic/src}"
FIXTURE="$TEST_SOURCE/scripts/harbor/tests/nce-interpreter"
OUT="$SCR/tests/nce-interpreter"
mkdir -p "$OUT"
locked "${CXX:-c++}" -std=c++20 -O1 -g -fsanitize=address,undefined \
    -I "$FIXTURE/shim" -I "$TEST_SOURCE/src" -I "$DYNARMIC_SOURCE" \
    "$TEST_SOURCE/src/core/arm/nce/interpreter_visitor.cpp" "$FIXTURE/main.cpp" \
    -o "$OUT/check"
"$OUT/check"

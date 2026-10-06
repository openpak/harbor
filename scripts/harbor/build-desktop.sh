#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 tobagin
# SPDX-License-Identifier: AGPL-3.0-only
#
# Build the Qt desktop frontend (binary: $BUILD/bin/harbor).
set -euo pipefail
. "$(dirname "$0")/common.sh"

refuse_while_emulator_runs

BUILD="$SCR/build/desktop"
JOBS="${NUM_JOBS:-12}"

cd "$SRC"
cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DYUZU_TESTS=OFF \
    -DENABLE_WEB_SERVICE=OFF -DENABLE_UPDATE_CHECKER=OFF -DUSE_DISCORD_PRESENCE=OFF \
    -DYUZU_USE_BUNDLED_QT=OFF -DENABLE_QT_TRANSLATION=OFF \
    "$@"
locked cmake --build "$BUILD" -j"$JOBS"

print_elapsed
ls -l "$BUILD/bin/harbor"

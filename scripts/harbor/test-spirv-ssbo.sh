#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
src=$(cd "$(dirname "$0")/../.." && pwd)
: "${HARBOR_DESKTOP_BUILD:?Set HARBOR_DESKTOP_BUILD to a configured and built desktop directory}"
: "${HARBOR_TEST_OUTPUT:?Set HARBOR_TEST_OUTPUT to a scratch directory outside /tmp}"
python3 "$src/scripts/harbor/tests/ssbo-fallback/run.py" "$src" "$HARBOR_DESKTOP_BUILD" "$HARBOR_TEST_OUTPUT"

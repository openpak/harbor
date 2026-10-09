#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 tobagin
# SPDX-License-Identifier: AGPL-3.0-only
#
# Harbor's enrolment against a local nx-baas with throwaway device CA keys (never the real ones):
# enrol, dauth with the CA-issued certificate, a second live session refused, a copy with other
# keys refused, the restored console (same id, same keys) accepted, and a server without keys
# inert. Prints "enrolment_check.sh: all checks passed" and exits 0.
#
#   enrolment_check.sh <nx-baas checkout> <work dir> [nlohmann_json single_include dir]
#
# Needs go, openssl (with sect233r1), curl, jq and a C++20 compiler. Keys and files stay in the
# work dir; nothing is printed that identifies the test console.
set -euo pipefail
NXBAAS="$(realpath "$1")"
WORK="$(realpath -m "$2")"
SRC="$(cd "$(dirname "$0")/../../../.." && pwd)"
JSON_INC="${3:-$SRC/.cache/cpm/nlohmann_json/v3.12.0/single_include}"
PORT="${ENROLMENT_CHECK_PORT:-28443}"
mkdir -p "$WORK"
rm -rf "$WORK/ca" "$WORK/data" "$WORK/a" "$WORK/b" "$WORK"/*.json "$WORK/server.log"
mkdir -p "$WORK/ca" "$WORK/data" "$WORK/gotmp"
export TMPDIR="$WORK/gotmp"

fail() { echo "  FAIL $*"; exit 1; }
ok() { echo "  ok   $*"; }

c++ -std=c++20 -O1 -I"$SRC/src" -I"$JSON_INC" \
    "$SRC/src/core/harbor_calibration/check/enrolment_check.cpp" \
    "$SRC/src/core/harbor_calibration/cal0.cpp" "$SRC/src/core/harbor_calibration/enrolment_wire.cpp" \
    -lcrypto -o "$WORK/enrolment_check"
T="$WORK/enrolment_check"

# Throwaway keys: the BaaS token key, HD00000001 (sect233r1) and the TLS device CA.
openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out "$WORK/ca/baas.pem" 2>/dev/null
openssl ecparam -name sect233r1 -genkey -noout -out "$WORK/ca/hd00000001.pem"
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -days 3650 \
    -subj "/CN=Test Harbor Device CA" \
    -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign,cRLSign" \
    -keyout "$WORK/ca/device-ca.key" -out "$WORK/ca/device-ca.pem" 2>/dev/null

(cd "$NXBAAS" && go build -o "$WORK/nx-baas" .)
SERVER=""
stop() {
    if [ -n "$SERVER" ]; then
        kill "$SERVER" 2>/dev/null || true
        wait "$SERVER" 2>/dev/null || true
        SERVER=""
    fi
}
trap stop EXIT
start() { # start 1|0: with the CA keys or without
    stop
    local keys=()
    if [ "$1" = 1 ]; then
        keys=(NX_DEVICE_CA_ECC_KEY_FILE="$WORK/ca/hd00000001.pem"
            NX_DEVICE_CA_TLS_CERT_FILE="$WORK/ca/device-ca.pem"
            NX_DEVICE_CA_TLS_KEY_FILE="$WORK/ca/device-ca.key")
    fi
    env NX_ADDR="127.0.0.1:$PORT" NX_API_ADDR="127.0.0.1:$((PORT + 1))" NX_DATA="$WORK/data" \
        NX_CA_DIR="$WORK/data/no-ca" BAAS_SIGNING_KEY="$WORK/ca/baas.pem" NX_VERSION_GATE=off \
        "${keys[@]}" "$WORK/nx-baas" >>"$WORK/server.log" 2>&1 &
    SERVER=$!
    for _ in $(seq 100); do
        curl -sk -o /dev/null "https://127.0.0.1:$PORT/" && return
        sleep 0.1
    done
    fail "nx-baas did not start (see $WORK/server.log)"
}

# req <console dir> <path> [body file] [session]: the status in $STATUS, the body in out.json.
HOST=dauth-lp1.ndas.srv.nintendo.net
req() {
    local args=(-sk --resolve "$HOST:$PORT:127.0.0.1" --cert "$1/tls.pem" --key "$1/tls.key"
        -o "$WORK/out.json" -w '%{http_code}' -X POST -H 'Content-Type: application/json')
    if [ -n "${3:-}" ]; then args+=(--data-binary "@$3"); fi
    if [ -n "${4:-}" ]; then args+=(-H "X-OpenPak-Console-Session: $4"); fi
    STATUS=$(curl "${args[@]}" "https://$HOST:$PORT$2")
}
enrol() { # enrol <console dir>
    req "$1" /harbor/v1/devices/challenge
    [ "$STATUS" = 200 ] || fail "challenge $STATUS"
    "$T" request "$1" "$WORK/out.json" >"$WORK/request.json"
    req "$1" /harbor/v1/devices "$WORK/request.json"
}
TOKENS="$WORK/tokens.json"
echo '{"token_requests":[{"client_id":"8f849b5d34778d8e"}]}' >"$TOKENS"
IN_USE='{"action":"make_new_console","error":"device_id_in_use"}'

start 1
"$T" new "$WORK/a"
enrol "$WORK/a"
[ "$STATUS" = 200 ] || fail "enrolment $STATUS $(cat "$WORK/out.json")"
cp "$WORK/out.json" "$WORK/signed-first.json"
[ "$("$T" apply "$WORK/a" "$WORK/out.json")" = enrolled ] || fail "apply"
ok "enrol: the device CA signed all four certificates; the calibration took them and is enrolled"

req "$WORK/a" /v8/device_auth_tokens "$TOKENS" session-one
[ "$STATUS" = 200 ] || fail "dauth $STATUS"
ok "dauth: the CA-issued certificate signs in"
req "$WORK/a" /v8/device_auth_tokens "$TOKENS" session-two
[ "$STATUS" = 409 ] && [ "$(cat "$WORK/out.json")" = "$IN_USE" ] ||
    fail "second session $STATUS $(cat "$WORK/out.json")"
ok "one live console: a second session of the same console gets 409 device_id_in_use"

"$T" rekey "$WORK/a" "$WORK/b"
enrol "$WORK/b"
[ "$STATUS" = 409 ] && [ "$(cat "$WORK/out.json")" = "$IN_USE" ] ||
    fail "duplicate $STATUS $(cat "$WORK/out.json")"
ok "duplicate: the same device id with other keys gets 409 device_id_in_use"

enrol "$WORK/a"
[ "$STATUS" = 200 ] || fail "restored $STATUS"
cmp -s <(jq -S . "$WORK/out.json") <(jq -S . "$WORK/signed-first.json") ||
    fail "the restored console got other certificates"
ok "restored console (same id, same keys): its certificates again"

start 0
req "$WORK/a" /harbor/v1/devices/challenge
[ "$STATUS" = 503 ] && grep -q enrolment_unavailable "$WORK/out.json" || fail "inert challenge $STATUS"
req "$WORK/b" /v8/device_auth_tokens "$TOKENS" session-three
[ "$STATUS" = 200 ] || fail "inert dauth $STATUS"
grep -q "enrolment off" "$WORK/server.log" || fail "the server did not say the enrolment is off"
ok "no keys: the enrolment is unavailable (503) and dauth answers as before"

echo "enrolment_check.sh: all checks passed"

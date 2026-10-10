#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 tobagin
# SPDX-License-Identifier: AGPL-3.0-only
#
# Measures what two run-desktop.sh --netns households see of each other: each asks two public STUN
# servers for its mapping, then both send to the other's public mapping for 12 s.
#
#   household-check.sh <uplink A> <uplink B> [port] [--oneway]
#
# Expected on a router with a working hairpin (n5air, 2026-10-10, uplinks 10.87.0.3 and 10.87.0.9):
#   A ... map1 ('<public>', P) map2 ('<public>', P)      same P twice: endpoint-independent mapping
#   A received from [(('<public>', Q), 'hello from B ')] the punch arrives through the hairpin
# With --oneway only B sends; A receiving nothing means address-dependent filtering.
set -euo pipefail
[ $# -ge 2 ] || { echo "usage: household-check.sh <uplink A> <uplink B> [port] [--oneway]" >&2; exit 2; }
A="$1"; B="$2"; PORT="${3:-45000}"; ONEWAY="${4:-}"
DIR="$(mktemp -d "${TMPDIR:-$HOME/REPOS/Openpak/scratch/harbor-eden/tmp}/household-check.XXXXXX")"
trap 'rm -rf "$DIR"' EXIT

cat > "$DIR/probe.py" <<'EOF'
import os, select, socket, struct, sys, time
me, peer, port, d, oneway = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4], sys.argv[5] == "--oneway"
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("0.0.0.0", port))
def stun(host, p):
    tx = os.urandom(12)
    s.sendto(struct.pack("!HHI", 1, 0, 0x2112A442) + tx, (socket.gethostbyname(host), p))
    end = time.time() + 3
    while time.time() < end:
        if not select.select([s], [], [], 0.5)[0]:
            continue
        data, _ = s.recvfrom(2048)
        if data[8:20] != tx:
            continue
        i = 20
        while i < len(data):
            t, l = struct.unpack("!HH", data[i:i + 4]); v = data[i + 4:i + 4 + l]
            if t == 0x20:  # XOR-MAPPED-ADDRESS
                ip = struct.unpack("!I", v[4:8])[0] ^ 0x2112A442
                return socket.inet_ntoa(struct.pack("!I", ip)), struct.unpack("!H", v[2:4])[0] ^ 0x2112
            i += 4 + ((l + 3) & ~3)
m1, m2 = stun("stun.l.google.com", 19302), stun("stun.cloudflare.com", 3478)
print(me, "local", s.getsockname(), "map1", m1, "map2", m2, flush=True)
if not m1:
    sys.exit(1)
open(f"{d}/{me}.map", "w").write(f"{m1[0]} {m1[1]}\n")
end = time.time() + 10
while not os.path.exists(f"{d}/{peer}.map") and time.time() < end:
    time.sleep(0.1)
pip, pp = open(f"{d}/{peer}.map").read().split()
got, end = set(), time.time() + 12
while time.time() < end:
    if not (oneway and me == "A"):
        s.sendto(f"hello from {me} #".encode(), (pip, int(pp)))
    while select.select([s], [], [], 0.5)[0]:
        data, src = s.recvfrom(2048)
        if data.startswith(b"hello"):
            got.add((src, data.split(b"#")[0].decode()))
print(me, "received from", sorted(got), flush=True)
EOF

P=(-4 -t none -u none -T none -U none --config-net --quiet)
pasta "${P[@]}" -a 192.168.250.2/24 -g 192.168.250.1 -o "$A" -- python3 -I "$DIR/probe.py" A B "$PORT" "$DIR" "$ONEWAY" &
pasta "${P[@]}" -a 192.168.251.2/24 -g 192.168.251.1 -o "$B" -- python3 -I "$DIR/probe.py" B A "$PORT" "$DIR" "$ONEWAY" &
wait

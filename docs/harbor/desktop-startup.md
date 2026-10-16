# Desktop startup and library scans

2026-10-13: the owner reported unusably slow boot/navigation after the Castle
Crashers rendering work. The desktop binary was already a Release build
(`-O3 -DNDEBUG`). A live startup trace showed the UI thread scanning external
update/DLC NSPs on the media filesystem before the console could boot; small
reads sometimes took more than half a second. The owner closed the instance
before HOME loaded. This establishes a startup I/O problem, not a measured
regression caused by the shader changes.

The content providers now cache parsed NSP metadata under the user's cache
directory (`container-metadata-v1`). Each cache is tied to the source path,
size, modification timestamp, scan scope and optional base-title filter.
Changed or corrupt caches fall back to the regular parser. Cached offsets
and lengths are validated before provider mutation. Tickets still load from
the original package; no keys, tickets or game bytes are stored in this cache.
Update versions, version labels and source tracking are retained. XCI,
split-file and Android content-URI inputs retain their normal parser paths
when they cannot use a host-file cache.

Console-mode registration skips the second pass that populates the hidden
Qt desktop game list. It still registers games, updates and DLC for the
firmware. Desktop game-list mode continues to populate its visible list.
Registration also avoids constructing an extra loader before parsing a
container. The console-only mode is retained on subsequent library refreshes.

The first scan of an uncached or changed NSP still parses it. Do not promise
instant cold startup or infer Castle Crashers compatibility from these fixes.
The existing shader changes are preserved.

Build mirror: the existing desktop CMake tree uses
`scratch/harbor-eden/build-source-castle`, not the canonical `src` checkout.
These changed files are synchronized into that mirror before the rebuild.
Keep the canonical checkout at `emulators/harbor`; do not relocate it or its
build/worktree target.

Validation and measured boot timings will be recorded after the rebuild.

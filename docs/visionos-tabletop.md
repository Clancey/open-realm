# Warcraft III visionOS tabletop

The native visionOS product statically links the existing Warcraft III engine
and game. The engine/server remains authoritative; Swift copies immutable
transport ABI v3 snapshots, posts typed commands, and renders copied asset ABI
v2 descriptors. RealityKit is the only visible visionOS renderer. Loading stays
owned by `ca_loading` until `CL_PrepRefresh()` publishes `ca_active`.

Detailed controls and asset contracts live in:

- [visionos-tabletop-controls.md](visionos-tabletop-controls.md)
- [wc3-visionos-assets.md](wc3-visionos-assets.md)

The parallel snapshot-only StarCraft II foundation is documented in
[games/starcraft-2/docs/visionos-foundation.md](../games/starcraft-2/docs/visionos-foundation.md).

## Source layout

The tabletop host lifecycle state machine and headless null client/renderer/UI
seams are platform-neutral C with no Apple/ObjC/Swift dependency and live under
the shared `platform/tabletop/` tree, not under `platform/apple/visionos/`:

```text
platform/tabletop/bridge/bz_tabletop_lifecycle.{c,h}     # pthreads host state machine
platform/tabletop/client/bz_tabletop_client_glue.h       # headless client seam contract
platform/tabletop/client/{cl_console,cl_fx,cl_input,cl_scrn,r,s,ui}_tabletop_null.c
```

visionOS is the only host today; Android/Meta Quest is expected to link this
same shared code later without modification. Only genuinely Apple-specific
glue (`bz_tabletop_bridge.h/.mm`, `bz_tabletop_swift.h`, the module map, and the
ObjC++ link smoke test) stays under
`platform/apple/visionos/tabletop/bridge/`.

A Meta Quest (Android/NDK + OpenXR) native build shell that links this same
shared code is documented in
[quest-tabletop.md](quest-tabletop.md).

## Local data contract

Retail archives are local-only and must never be committed. Set:

```sh
export BZ_WC3_DATA_DIR="$HOME/Downloads/Warcraft III"
```

The source directory must contain regular, non-symlink files:

```text
War3.mpq
War3x.mpq
War3xLocal.mpq
```

The build stages exactly those archives under
`OpenRealmTabletop.app/Resources/Warcraft III`. Verify source and staging with:

```sh
make visionos-verify-wc3-source
make test-visionos-wc3-data
```

## Build and bundle gates

```sh
make test-visionos-tabletop-host
make test-bz-tabletop-lifecycle
make test-bz-tabletop-transport
make test-bz-tabletop-assets
make test-bz-tabletop-catalog
make visionos-tabletop
```

`make visionos-tabletop` builds and link-checks static arm64 `xrsimulator` and
`xros` engine/bridge archives, compiles the native shell, and verifies both app
bundles. `verify-bundle.sh` enforces Mach-O platform/minimum OS, arm64, bundle
identity, required live ABI symbols, static/system-only dependencies, no
embedded frameworks, no SDL scene delegate, no developer paths, exact MPQs,
simulator ad-hoc signing, and unsigned device output. The visionOS client uses
the headless null renderer and never creates an SDL window or OpenGL context.

Outputs:

```text
build/visionos/tabletop/xrsimulator/OpenRealmTabletop.app
build/visionos/tabletop/xros/OpenRealmTabletop.app
```

## Disposable simulator acceptance

Never address the protected demo simulator:

```text
0FDBB103-A3CE-461F-AF41-30CDBEC010FE
```

`launch-tabletop-simulator.sh` always creates a fresh Apple Vision Pro
simulator, boots and installs with bounded waits, stages MPQs only into that
app container, passes runtime settings through `SIMCTL_CHILD_*`, captures
stdout/stderr directly, checks the launched process in one-second cancellable
hold slices, then terminates, shuts down, and deletes the simulator. Successful
cleanup prints the deleted UDID. It never uses `booted`.

Do not add broad retries. Retry only after captured stderr proves a transient
CoreSimulator service failure, and match only that evidenced failure. Capture
producer output before searching it; do not combine `pipefail` with
`producer | grep -q`, which can turn successful early matches into SIGPIPE
failures.

The canonical 128x128 real-art gate is:

```sh
OPENREALM_TABLETOP_LOG_DIR=build/visionos/tabletop/acceptance/human02-roc \
  make visionos-tabletop-simulator-acceptance
OPENREALM_TABLETOP_TFT=1 \
OPENREALM_TABLETOP_LOG_DIR=build/visionos/tabletop/acceptance/human02-tft \
  make visionos-tabletop-simulator-acceptance
```

Run additional maps as separate invocations so every map gets a fresh
simulator. Non-Human02 maps require exact ITEM classes, including an explicit
empty value:

```sh
# Small deterministic ROC map: 96x96, nine chunks, no ITEM phase.
OPENREALM_TABLETOP_MAP=Human01 \
OPENREALM_TABLETOP_EXPECTED_ITEM_CLASSES='' \
OPENREALM_TABLETOP_REQUIRED_CATEGORIES='unit building resource doodad destructable' \
OPENREALM_TABLETOP_POST_STABLE_WAIT=30 \
OPENREALM_TABLETOP_LOG_DIR=build/visionos/tabletop/acceptance/human01 \
  make visionos-tabletop-simulator-acceptance

# Rich ROC map: 128x96, twelve chunks.
OPENREALM_TABLETOP_MAP=NightElf01 \
OPENREALM_TABLETOP_EXPECTED_ITEM_CLASSES='' \
OPENREALM_TABLETOP_REQUIRED_CATEGORIES='unit building resource doodad' \
OPENREALM_TABLETOP_POST_STABLE_WAIT=60 \
OPENREALM_TABLETOP_LOG_DIR=build/visionos/tabletop/acceptance/nightelf01 \
  make visionos-tabletop-simulator-acceptance

# Rich TFT map with authored ITEM metadata: 96x128, twelve chunks.
OPENREALM_TABLETOP_TFT=1 \
OPENREALM_TABLETOP_MAP=NightElfX01 \
OPENREALM_TABLETOP_EXPECTED_ITEM_CLASSES='31656872,33656872,6e616d72,746c6870' \
OPENREALM_TABLETOP_REQUIRED_CATEGORIES='unit resource doodad destructable item' \
OPENREALM_TABLETOP_POST_STABLE_WAIT=90 \
OPENREALM_TABLETOP_LOG_DIR=build/visionos/tabletop/acceptance/nightelfx01 \
  make visionos-tabletop-simulator-acceptance
```

The matrix covers ROC/TFT product selection, terrain floor, cliffs, water/fog
materials, doodads, units, billboards/atlases, authored textures, and ITEM
metadata without committing archives or generated artifacts.

## Performance and readiness contract

- Terrain chunks are at most 32x32 cells. A 128x128 map produces at most 16
  terrain mesh entities.
- One copied fog descriptor produces exactly one fog entity.
- Stable production acceptance requires authored models, geosets, textured
  materials, zero model/material placeholders, zero placeholder/metadata logs,
  and the requested semantic categories.
- Initial and stable summaries must keep cache misses unchanged while hits
  increase. Same-generation Swift reconciliation performs no chunk, fog,
  entity, decode, or disk work.
- C and Swift tests cover cache miss/hit/reset, retained asset/terrain lifetime,
  concurrent shutdown/publication, and immutable copied-value reuse.

The renderer cache is versioned and edition-separated:

```text
<Application Support>/OpenRealm/WarcraftRenderer/v2/{roc,tft}/
```

## Product and bridge state

The native product reducer is:

```text
menu -> loading -> playing <-> paused -> victory|defeat|draw
  ^         |          |                    |
  +---------+----------+---- error ----------+
  +---------------- return to menu ------------+
```

`TabletopSessionModel` owns one lifecycle generation, polling worker,
conversion worker, and latest-value mailboxes. It awaits the first active
snapshot before entering `playing`, cancels and joins workers on stop, rejects
stale epochs, and starts retries/next maps with a fresh bridge instance.
Snapshots contain copied values only; retained C snapshots and asset handles
are released before RealityKit presentation.

Commands are typed (`select`, smart entity/point, target point, button,
cancel), include session/generation identity, and are validated before crossing
the ABI. No free-form command string crosses from Swift. Server state controls
selection, actions, resources, loading, and game result.

## Desktop and sanitizer regression commands

```sh
make openwarcraft3
build/bin/openwarcraft3 -data "$BZ_WC3_DATA_DIR" \
  +map Maps/Campaign/Human02.w3m +com_frame_limit 100
build/bin/openwarcraft3 -data "$BZ_WC3_DATA_DIR" -tft \
  +map Maps/Campaign/Human02.w3m +com_frame_limit 100

make run-sc2 SC2DATA="$HOME/Downloads/Starcraft II/StarCraft2" \
  ARGS="+com_frame_limit 100"
make test

BREW_PREFIX=$(brew --prefix)
BASE_CFLAGS="-Wall -Wmisleading-indentation -fno-common -I. -Ishared -Ishared/types \
  -DGL_SILENCE_DEPRECATION -I$BREW_PREFIX/include -arch arm64 -g"
BASE_LDFLAGS="-Lbuild/lib -L$BREW_PREFIX/lib -arch arm64"
make -B test-bz-tabletop-lifecycle test-bz-tabletop-transport \
  test-bz-tabletop-assets CC=clang CFLAGS="$BASE_CFLAGS -fsanitize=thread" \
  LDFLAGS="$BASE_LDFLAGS -fsanitize=thread"
ASAN_OPTIONS=detect_leaks=0 \
make -B test-bz-tabletop-lifecycle test-bz-tabletop-transport \
  test-bz-tabletop-assets CC=clang CFLAGS="$BASE_CFLAGS -fsanitize=address,undefined" \
  LDFLAGS="$BASE_LDFLAGS -fsanitize=address,undefined"
```

## Physical-device and deferred scope

`visionos-tabletop-xros` intentionally emits an unsigned device bundle.
Provisioning, entitlements, team signing, installation, and physical testing
remain manual. Validate immersive-space opening, board placement, hand/indirect
gestures, targeting, pause/background behavior, audio, comfort, and sustained
thermal performance on hardware.

Multiplayer orchestration, discovery, lobby/product UX, and multi-client
acceptance are deferred. The transport must continue to consume authoritative
network state; Swift must not infer gameplay or bypass the server.

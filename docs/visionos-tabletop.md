# visionOS tabletop runtime and native shell

Layer 1 establishes a callable, statically-linked, visionOS-compatible
Warcraft III engine runtime that a later layer can host from Swift/RealityKit
without disturbing the desktop `openwarcraft3`/`opensc2` executables. This
layer does **not** implement asset export/MPQ bundling, gameplay controls,
multiplayer, or a snapshot bridge. The data layer supplies the local-only build
contract for staging legally owned Warcraft III MPQs; no retail data is
committed.

The native shell under `platform/apple/visionos/tabletop/app/` adds a SwiftUI
launcher and RealityKit mixed immersive board. Its pure Swift transport seam
supports deterministic procedural fixtures and a thin live actor over the
frozen Layer-2 C ABI plus the existing Objective-C++ lifecycle host. Live mode
copies retained C snapshots into Swift values, releases the C snapshot, then
publishes to RealityKit; it never exposes engine-owned pointers to the UI.

## Native Swift shell seam

```
TabletopSnapshotTransport (pure Swift protocol)
        +-- FixtureSnapshotTransport (deterministic actor)
        +-- LiveTabletopTransport (Layer-2 C ABI + lifecycle host)
                         |
TabletopGenerationDeduplicator (~30 Hz poll)
        |
TabletopSnapshotConverter (value copy + placement)
        |
TabletopSceneState (pure reconciliation plan)
        |
RealityTabletopReconciler (RealityKit ownership)
```

- Snapshot/model/reducer/placement/reconciliation/gesture files do not import
  SwiftUI or RealityKit.
- `TabletopSnapshotTransport.poll()` returns copied Swift values. The session
  polls at approximately 30 Hz, deduplicates by generation, and converts into
  a separate render snapshot before publication on the main actor.
- `FixtureSnapshotTransport` emits at most 49 terrain tiles and three entities.
  It advances generation every six polls so both deduplication paths run.
- `LiveTabletopTransport` starts/stops `BZTabletopBridge`, retains with
  `BZ_TT_Latest()`, validates `BZ_TABLETOP_ABI_VERSION`, and copies connection,
  map, player/resource, selection, every entity POD field, fog planes, and
  nested unit-layout/button/inventory/queue strings and arrays into
  framework-free Swift values. It records ABI overflow and duplicate slot IDs,
  surfaces their current values as a non-fatal diagnostic, and a tested lease
  helper always releases before returning or throwing.
  Typed `TabletopCommand` values call only the five validated `BZ_TT_Post*`
  entry points. Tap selection uses the ABI's documented generation-zero bypass
  because the engine publishes around 60 Hz while rendering polls around 30 Hz;
  the Swift session ID still rejects commands crossing lifecycle restarts.
- The launcher opens the `tabletop` mixed `ImmersiveSpace` automatically. It
  first starts the selected transport and waits up to three cancellable seconds
  for an ABI-validated snapshot, then opens the space. It dismisses its window
  only after `.opened`; missing data, startup, ABI, timeout, cancellation, and
  open failures retain an actionable Retry UI. Runtime/queue failures offer a
  return to that launcher from the immersive error panel.
- RealityKit owns the visible surface. SDL is neither a visible surface nor a
  scene delegate on this lane.

Live mode is the production default. `BZ_TABLETOP_DATA_PATH` defaults to the
app's `Resources/Warcraft III` directory and `BZ_TABLETOP_MAP` defaults to
`Human02`; an explicit `BZ_TABLETOP_CONNECT` without a map selects remote mode.
Set `BZ_TABLETOP_MODE=fixture` only for deterministic tests.
Preflight follows the engine's real data rules and filesystem metadata: it
accepts a regular MPQ file, or the exact root-relative loose
`Scripts/common.j` plus a regular `.w3m`/`.w3x` for local-map startup
(`common.j` alone is sufficient for a remote connect). Arbitrary nonempty
directories are rejected in the launcher rather than producing a fake empty
live session. An unknown mode is surfaced as an error, never silently demoted
to fixtures. Production builds invoke `wc3_data.sh stage` and fail unless
`${BZ_WC3_DATA_DIR:-$HOME/Downloads/Warcraft III}` contains exactly
`War3.mpq`, `War3x.mpq`, and `War3xLocal.mpq`; the bundle verifier requires
exactly those three files under `Resources/Warcraft III`. An optional
`BZ_TABLETOP_RESOURCE_HOOK` remains available for non-MPQ future resources
after that required stage.

`LiveTabletopTransport` uses the append-only
`BZ_TTSnapshot_ConfigStringCount()` accessor and copies every slot in
`[0, count)` before releasing the retained snapshot. Within that documented
range, `BZ_TTSnapshot_ConfigString()` returning `false` becomes an explicit
empty Swift entry. The shell never imports or guesses `MAX_CONFIGSTRINGS`.

## Native asset and terrain ABI

`platform/bridge/bz_tabletop_assets.h` is a separate, versioned C ABI for a
future RealityKit renderer. It deliberately does not widen the frozen snapshot
transport:

- `BZ_TTA_RegisterConfigString()` resolves immutable image/model assets from a
  retained snapshot's configstring identity through the engine filesystem and
  MPQ search order. Callers never submit guessed archive paths.
- `BZ_TTA_LatestTerrain()` returns a retained deep copy of authoritative
  `world.map` terrain. The descriptor includes bounds, corner/tile/chunk
  dimensions, corrected ground and water heights, ground/cliff IDs and
  variations, cliff levels, and ramp/water/blight/boundary flags.
- `BZ_TTTerrain_ReferencedTextureCount()` and
  `BZ_TTTerrain_ReferencedTexture()` expose only corner-referenced terrain
  texture registrations while preserving their original W3E table indices.
- Handles are opaque and explicitly retained/released. Descriptor accessors copy
  plain C POD values; payloads remain immutable, so concurrent readers require
  no renderer-thread lock.
- Missing or malformed registrations return cached, valid placeholder
  descriptors with an explicit status. Only the winning cache insertion logs,
  which keeps concurrent first-time misses log-once.
- The public header includes no engine, Objective-C, Swift, SDL, OpenGL, or
  RealityKit types. Generic ownership/cache publication lives in
  `platform/bridge/`; Warcraft W3E/BLP/MDX translation lives in
  `games/warcraft-3/visionos/`.
- `OpenRealmTabletopBridge` re-exports the asset header, so Swift can import the
  ABI without a second bridging module.

The engine archive builds the Warcraft translation as a separate unity object
from the game object. See [wc3-visionos-assets.md](wc3-visionos-assets.md) for
the descriptor contract, format facts, tests, and known export gaps.

### Production shell build and tests

```sh
make test-visionos-tabletop-host       # pure Swift executable tests on macOS
make test-bz-tabletop-assets           # C ABI, W3E, BLP, MDX, ownership/race tests
make visionos-tabletop-xrsimulator     # arm64 xrsimulator 2.0 app, ad-hoc signed
make visionos-tabletop-xros            # arm64 xros 2.0 app, unsigned
make visionos-tabletop                 # all three gates
make visionos-tabletop-simulator-acceptance # disposable-clone live MPQ gate
```

Host coverage includes launcher reduction, mode selection, lifecycle mapping,
generation deduplication, fixture queue hit/full/stale-session paths, command
lowering/error mapping, world bounds/overflow, duplicate-safe reconciliation,
gesture terminal suppression, polling cancellation, first-snapshot timeout
plumbing, complete configstring-slot copying, product identity, and snapshot
release on copy success/failure. `test-visionos-wc3-data` separately covers the
seven source/staging/missing-data/symlink/interruption contracts.

The app bundle is
`build/visionos/tabletop/<platform>/OpenRealmTabletop.app` with bundle ID
`org.openrealm.visionos.tabletop` and executable `OpenRealmTabletop`.
`verify-bundle.sh` rejects wrong deployment/device
metadata, missing indirect-input/multiple-scene/hand/world-sensing declarations,
SDL scene delegates, missing/extra/symlinked MPQs, embedded or developer-path dynamic
frameworks, absolute developer paths, desktop identity collisions, wrong Mach-O
platform or minimum OS, incorrect signing/identifier binding, and non-arm64
output. It also requires the linked lifecycle class, snapshot getter,
configstring-count accessor, and typed select-post symbols. No Xcode project is
used.

`launch-tabletop-simulator.sh` clones only a shutdown Apple Vision Pro device,
boots that disposable clone with a 120-second bound, installs identical signed
code, and stages the same three real MPQs into the clone's private app-data
container through `wc3_data.sh`. This avoids an unbounded CoreSimulator import
of the roughly 717 MB sealed production bundle without weakening either bundle
gate. It launches with `SIMCTL_CHILD_*`, captures stdout/stderr directly,
requires five seconds of residency plus transport initialization, first
snapshot, and `Human02` begin evidence, then terminates the app and deletes the
clone. It never addresses `booted` or takes over the user's active simulator.

Verify the real ROC map identity with the repository diagnostic tool before
launch:

```sh
make mpqtool
build/bin/mpqtool -mpq "${BZ_WC3_DATA_DIR:-$HOME/Downloads/Warcraft III}/War3.mpq" \
  ls Maps/Campaign | grep -F 'Human02.w3m'
build/bin/mpqtool -mpq "${BZ_WC3_DATA_DIR:-$HOME/Downloads/Warcraft III}/War3.mpq" \
  info Maps/Campaign/Human02.w3m
```

The local ROC archive check enumerated `Maps\Campaign\Human02.w3m` as a regular,
uncompressed, unencrypted 236,299-byte entry. This confirms the archive-relative
identity without extracting or committing the map.

The direct linker embeds the generated plist in `__TEXT,__info_plist` before
signing. The verifier requires that section, an exact signing identifier, and
`codesign --verify --deep --strict`; modifying the external plist after signing
then makes strict verification fail even though current `codesign -d` output
describes this direct-toolchain bundle as `Info.plist=not bound`.

## Architecture summary

```
common/main.c (desktop)          platform/apple/visionos/tabletop/bridge/
        |                                bz_tabletop_bridge.mm (ObjC++, NSObject)
        v                                        |
common/bz_runtime.{h,c}  <----------------------- bz_tabletop_lifecycle.{h,c}
  BZ_RuntimeInit/Frame/Shutdown                    (portable pthreads core)
```

- **`common/bz_runtime.h`/`.c`** — the reusable engine lifecycle (init / one
  frame / shutdown), factored out of the desktop `main()` loop. Both the
  desktop executable and the visionOS bridge call the exact same three
  functions; neither duplicates client/server/network bring-up.
- **`platform/apple/visionos/tabletop/null/cl_null.c`** *(Layer 1 only —
  retired in Layer 2, see below)* — a real, log-once (never silent) null
  implementation of the handful of `CL_*`/`Key_*`/`Cmd_ForwardToServer`
  symbols `common/bz_runtime.c` and `common/common.c` call unconditionally.
  It replaced the entire `client/` module (SDL window, SDL input polling,
  OpenGL renderer dispatch) for this static archive in Layer 1, since the
  real `common/*.c` and `server/*.c` never reference `client/`-only symbols
  directly. Layer 2 deletes this file and links the real `client/` networking/
  parse/state path instead — see the "Layer 2" section below for why and
  with what it was replaced.
- **`platform/apple/visionos/build.mk`** — builds
  `build/lib/visionos/<platform>/libopenwarcraft3-engine.a` (the headless
  engine) for `xrsimulator` and `xros` via `xcrun`/clang target triples and
  `ar`, with zero Xcode project involved.
- **`platform/apple/visionos/tabletop/bridge/`** — the Objective-C++
  lifecycle host: a portable pthreads state machine
  (`bz_tabletop_lifecycle.{h,c}`) plus a thin `NSObject` wrapper
  (`bz_tabletop_bridge.{h,mm}`) that forwards to it. Zero Swift/RealityKit
  code lives here.

## Building

```sh
make xrsimulator          # libopenwarcraft3-engine.a for the visionOS Simulator SDK
make xros                 # libopenwarcraft3-engine.a for the visionOS device SDK (unsigned)
make visionos             # both of the above

make xrsimulator-bridge   # + libopenwarcraft3-bridge.a, link-checked against the engine archive
make xros-bridge          # same, device SDK
make visionos-bridge      # both of the above
```

Each `<platform>` target produces:

```
build/lib/visionos/<platform>/libopenwarcraft3-engine.a   # common/ + server/ + real client/ + tabletop client/ glue (see "Layer 2" below; Layer 1 originally used cl_null.c here, now retired) + game/jass/sheet/shared
build/lib/visionos/<platform>/libopenwarcraft3-bridge.a   # bz_tabletop_lifecycle.o + bz_tabletop_bridge.o
build/lib/visionos/<platform>/bridge-link-smoke           # link-check binary (built, not run — see below)
```

`BZ_XR_MIN_OS` (default `1.0`) controls the `-target arm64-apple-xros<ver>[-simulator]`
triple's minimum OS version; override with `make xrsimulator BZ_XR_MIN_OS=2.0` if a
later layer needs a newer floor.

## Warcraft III bundle data contract

`platform/apple/visionos/scripts/wc3_data.sh` is the single build-time contract
between legally owned local Warcraft III data and a later app shell. It resolves
the source as:

```sh
${BZ_WC3_DATA_DIR:-$HOME/Downloads/Warcraft III}
```

The source must contain three non-empty files with these exact names:
`War3.mpq`, `War3x.mpq`, and `War3xLocal.mpq`. Other source entries — including
installer ISOs, `SETUP.MPQ`, documentation, or directories — are ignored. The
tool copies only the three allowlisted files and replaces the destination data
directory, so repeated builds remove stale resources instead of accumulating
them.

```sh
make visionos-verify-wc3-source
make visionos-stage-wc3-data BZ_XR_APP_STAGE_DIR="/tmp/OpenRealm Stage"
make visionos-verify-wc3-data BZ_XR_APP_STAGE_DIR="/tmp/OpenRealm Stage"
make test-visionos-wc3-data
```

The caller passes an app or staging **root**, never the resource directory
itself. Staging and verification use this exact layout:

```text
<app-or-staging-root>/
└── Resources/
    └── Warcraft III/
        ├── War3.mpq
        ├── War3x.mpq
        └── War3xLocal.mpq
```

Missing and empty inputs are diagnosed by exact absolute/caller-provided path
before the existing destination is touched. `verify-bundle` also rejects every
entry outside the three-file allowlist, plus symlinked `Resources`, data
directories, or MPQ files that could escape the bundle. A later shell build
should invoke the verifier after copying resources and before signing or
packaging.

### Read-only bundle data and writable application state

The app bundle is immutable at runtime. The shell must resolve
`Resources/Warcraft III` from its bundle and pass that directory as the
engine's `-data` argument; engine, transport, and UI code must never patch,
replace, download into, or otherwise write to those MPQs.

All mutable state — configuration, saves, downloaded maps, caches, diagnostics,
and logs — belongs beneath the app's writable Application Support directory
(resolved with the platform API and scoped to the app's bundle identifier).
The later shell owns creating and passing that writable location. It must not
use the bundle resource directory as a working directory or writable fallback.

The three MPQ names are explicitly ignored by Git. Synthetic tests create only
temporary, non-retail fixtures; local acceptance may validate a developer's
real source path but must not print, hash, copy into the repository, or commit
the proprietary contents.

### Why `bridge-link-smoke` is built but never run

`bridge-link-smoke` is a `-target arm64-apple-xros...-simulator`/`-target
arm64-apple-xros...` Mach-O. Unlike the desktop test binaries elsewhere in
this repo, it cannot be executed directly from a bare `xcrun`/clang
toolchain — running a simulator-target binary requires `simctl`
(booting/spawning inside an actual Simulator runtime), and a device-target
binary requires a signed app bundle on real hardware. The `<platform>-bridge`
Make targets therefore stop at "does this fully link with zero undefined
symbols against `libopenwarcraft3-engine.a` plus `Foundation`/`libz`/
`libpthread`" — which is the deliverable this layer promises.
`bridge-link-smoke` linking successfully for both `xrsimulator` and `xros`
is itself that proof: `otool -L` on the resulting binaries shows only
`Foundation`, `CoreFoundation`, `libobjc`, `libc++`, `libz`, and `libSystem`
— no leaked SDL symbols.

### Archive composition (why `common/world.c`/`routing.c` are excluded)

The "engine" unity object intentionally excludes `common/world.c` and
`common/routing.c`: `games/warcraft-3/game/g_world.c` already
`#include`s both files directly (the same trick the desktop `libgame.dylib`
relies on, since macOS's two-level namespace does not share symbols between
independently-loaded dylibs). Including a second copy in one *static*
archive would duplicate-define `CM_*`/world symbols; neither `common/*.c`
nor `server/*.c` reference those symbols directly (only the WC3 game module
does, via its own copy), so excluding the root copy from the engine object
is correct.

## Lifecycle state machine

`bz_tabletop_lifecycle.h` (plain C, no Objective-C, no Foundation — see its
header comment for why it avoids `common/shared.h`'s `LPCSTR`/`BOOL` types)
defines:

```
IDLE -> STARTING -> RUNNING <-> SUSPENDED
                       |            |
                       +--> STOPPED <+
STARTING -> FAILED (bad -data/+map/… — BZ_TabletopLastError() explains why)
```

`STOPPED` and `FAILED` are **terminal** — the engine thread is single-shot
per lifecycle instance and is never restarted or rejoined into a fresh run.
Once either is reached, `BZ_TabletopStart()` rejects any further call on
that instance (logs once, no-op); a caller that needs to run again must
`BZ_TabletopCreate()` a new instance. This matches the intended
static-archive + swiftc/clang host architecture: an embedding app spins up
one `BZTabletopBridge`/lifecycle per session and discards it on shutdown,
rather than pooling/reusing engine threads.

- `BZ_TabletopCreate(argc, argv)` — allocate, deep-copy args, no thread yet.
- `BZ_TabletopStart(lc)` — spawns the one dedicated, single-shot engine
  thread (never the caller's thread), blocks until `BZ_RuntimeInit()`
  completes (state leaves `STARTING`). No-op if already
  `STARTING`/`RUNNING`/`SUSPENDED`; rejected (logged, no-op) if the instance
  has already reached the terminal `FAILED`/`STOPPED` state. The state
  check and the `IDLE` → `STARTING` claim happen atomically under one lock
  hold, so two threads racing `BZ_TabletopStart()` on a fresh instance
  cannot both pass and both spawn a thread for it.
- `BZ_TabletopSuspend`/`Resume(lc)` — pause/resume per-frame ticking without
  tearing the engine down.
- `BZ_TabletopStop(lc)` — requests an orderly shutdown. From any thread other
  than the engine thread, blocks until `BZ_RuntimeShutdown()` has run and the
  engine thread has exited, then joins it. **Re-entrant**: if called from
  *inside* the engine thread itself (see `Sys_Quit()` below), it only flags
  the stop request and returns immediately — a thread cannot join itself —
  leaving the actual join for a later call from another thread.
  Idempotent: safe to call repeatedly, from any state. If two external
  threads call `BZ_TabletopStop()` at the same time, only one of them
  actually performs `pthread_join()` — a mutex-guarded `joining` flag makes
  the rest wait on a condvar for that join to finish instead of racing their
  own `pthread_join()` call against it, since POSIX leaves concurrent joins
  on the same thread undefined.
- `BZ_TabletopDestroy(lc)` — calls `BZ_TabletopStop()` first, then frees.
- `BZ_TabletopGetState`/`LastError(lc)` — read-only, thread-safe.

### Threading contract

Every `BZ_RuntimeInit`/`Frame`/`Shutdown` call happens on the one dedicated
engine thread `BZ_TabletopStart()` creates — satisfying
`common/bz_runtime.h`'s "callers must serialize onto one thread"
requirement. The engine thread's loop:

1. `BZ_RuntimeInit()` → on failure, publish `FAILED` (unblocks `Start()`).
   On success, check whether a stop was already requested (an external
   `BZ_TabletopStop()` call can arrive from another thread while this is
   still running) **before** publishing anything: if so, skip `RUNNING`
   entirely and fall straight to step 3's shutdown path; otherwise publish
   `RUNNING` (unblocks `Start()`) and continue to step 2. This closes the
   one remaining way a background startup path could otherwise make the
   visible state transition *away* from an already-requested stop, even
   transiently — the only state transitions ever seen from here on are
   forward towards `STOPPED`.
2. Loop: wait out `SUSPENDED`, then `BZ_RuntimeFrame(elapsed_ms)` using
   `clock_gettime(CLOCK_MONOTONIC, …)` (never `SDL_GetTicks`, since no SDL is
   linked). No display link drives frames yet in this layer — the loop
   paces itself with a 16ms `nanosleep` between frames; a later layer swaps
   this for a real display-link-driven `BZ_TabletopTick()`-style call if
   needed.
3. Break the loop on an external stop request **or** `BZ_RuntimeFrame()`
   returning `false` (the frame limit or a console "quit" was reached
   internally) — both cases (and step 1's early-stop path above) call
   `BZ_RuntimeShutdown()` (idempotent) before publishing `STOPPED`, which is
   itself terminal: nothing writes to the state after this point.

### `Sys_Quit()` and thread-local self-identification

`common/common.h` requires a global, parameterless `Sys_Quit()`;
`bz_tabletop_lifecycle.c` supplies it using a `static __thread
bzTabletopLifecycle_t *tls_current_lc` pointer set at the top of the engine
thread's main function. `Com_Quit()` always calls `BZ_RuntimeShutdown()`
*then* `Sys_Quit()` (see `common/common.c`), so by the time `Sys_Quit()` runs,
the engine has already torn itself down — its only job is to signal *this*
lifecycle instance to stop, via `BZ_TabletopStop()`'s re-entrant path. This
correctly scopes multiple concurrent lifecycle instances to their own engine
thread without a process-wide singleton.

## Testing

```sh
make test-bz-tabletop-lifecycle   # this layer's lifecycle suite alone
make test                         # full suite, includes the above
```

`games/warcraft-3/tests/test_bz_tabletop_lifecycle.c` runs entirely on the
desktop toolchain (no visionOS SDK required) against the same stubbed
`CL_*`/`SV_*` boundary `test_bz_runtime.c` uses, since the engine thread
calls the real `common/bz_runtime.c`. Covered scenarios: valid init reaches
`RUNNING`; bad init reaches `FAILED` with a correct `BZ_TabletopLastError()`
string; suspend/resume transitions; external stop blocks until `STOPPED` and
is idempotent; concurrent/racing `BZ_TabletopStop()` calls from two threads
exercise the internal join-serialization path (a `joining` flag guarded by
the lifecycle mutex ensures only one caller ever calls `pthread_join()` on
the engine thread — concurrent callers instead wait on the condvar for the
first join to finish, since POSIX leaves concurrent `pthread_join()` calls
on the same thread undefined); `BZ_TabletopStart()` called again after
reaching the terminal `STOPPED` or `FAILED` state is rejected as a no-op
(state does not regress, `CL_Init()`/init is not re-run);
`BZ_TabletopDestroy()` without a prior explicit stop still joins cleanly;
`BZ_TabletopStop()` called while the engine thread is still inside
`BZ_RuntimeInit()` (still `STARTING`) — verified with a deliberately slowed
stub `CL_Init()` to widen the race window — reaches `STOPPED` having run
shutdown exactly once and having never entered the frame loop, without ever
publishing `RUNNING`; and reaching `+com_frame_limit` drives the engine to
`STOPPED` on its own (via the `Sys_Quit()` thread-local path) without any
external `BZ_TabletopStop()` call. The concurrent-stop scenario was
additionally verified with ThreadSanitizer (`-fsanitize=thread`), which
reported zero data races across multiple runs after the join-serialization
fix (an earlier revision without it reliably aborted under TSan inside
`pthread_join`).

### Local verification baseline and methodology

This layer's build/test/link-check results above were verified against
Xcode 26.6 (`xcodebuild -version`) with the `xrsimulator`/`xros` SDKs at
26.5 (`xcrun --sdk <sdk> --show-sdk-version`) — record any future baseline
bump here rather than assuming it implicitly. Two methodology notes for
whoever extends this layer's verification (e.g. inspecting `otool -L`/`nm`
output, or eventually running a built binary):

- Never combine `set -o pipefail` with an early-exiting consumer like
  `producer | grep -q pattern`: once `grep -q` finds its first match it
  exits immediately, and `producer` can then receive `SIGPIPE` on its next
  write — under `pipefail` that surfaces as a spurious non-zero exit even
  though the check itself passed. Redirect the producer's output to a file
  (or a shell variable) first, then `grep`/`grep -c` that captured output.
- Any acceptance/verification wait on an external process (a simulator
  boot, an app launch) must be bounded with an explicit timeout and remain
  cancellable — never an unbounded blocking wait — and must capture that
  process's `stderr` directly (e.g. explicit `2>` redirection to a file, or
  inheriting the caller's own stream) rather than discarding it, so a
  failure is diagnosable instead of silent. The native shell acceptance
  harness follows this contract; `bridge-link-smoke` remains a link-only gate.

## What this layer does not do

- No raw terrain data in the live ABI: fixture mode renders its procedural
  board, while live mode renders copied entities without inventing terrain.
- No proprietary Warcraft III data in source control. The data helper stages
  exactly the required locally owned MPQs into a caller-owned build directory;
  fixture mode does not silently replace missing production data.
- Tap selection posts a typed command in both modes. Dragging remains local
  placement scaffolding until an authoritative world-point interaction design
  lands; it does not fabricate engine coordinates.
- No SDL/OpenGL window, no SDL input polling, no Xcode project.
- No audio output: Layer 2 supplies the explicit, log-once no-op backend
  `platform/apple/visionos/tabletop/client/s_tabletop_null.c`.
- No device code signing: the `xros` static targets and production app are
  deliberately unsigned; the xrsimulator app is ad-hoc signed.
  Producing an installable device build requires an external provisioning
  profile/signing identity.

# Layer 2: real headless client + snapshot/command transport

Layer 2 replaces Layer 1's link-smoke-only null client with the **real**
`client/*.c` networking/parse/state path, and adds
`platform/bridge/bz_tabletop_transport.{h,c}` — a pure C, versioned,
Objective-C/Swift/SDL/RealityKit-free ABI that the native shell above imports
through its local module map to read
authoritative snapshots and post typed commands. It still does **not**
implement Swift/SwiftUI/RealityKit, app creation/signing, asset decoding/export,
visible rendering, audio, menus, or multiplayer — those remain later layers.
Do not confuse `bz_tabletop_transport.{h,c}` (this layer, a plain-C ABI
under `platform/bridge/`) with the Objective-C++ lifecycle bridge class
`BZTabletopBridge` (`platform/apple/visionos/tabletop/bridge/`, Layer 1) —
they are two distinct, separately-versioned surfaces.

## Why a real client, not a bigger null client

Layer 1's `cl_null.c` never parsed a single server packet — it existed only
to satisfy link-time symbol requirements. A snapshot transport needs
**authoritative** state: `cl.ents`/`cl.playerstate`/`cl.selection`/`cl.fow`/
configstrings as decoded by the real `client/cl_parse.c`, never by reading
`ge->edicts` directly out of the server (that would bypass client-side
prediction/interpolation and the client/server boundary this engine relies
on everywhere else). So Layer 2 retires `cl_null.c` and links the real
`client/cl_main.c`, `cl_parse.c`, `cl_view.c`, `cl_tent.c`, and `keys.c`,
replacing only the renderer/input/sound/UI *drawing* seams those files call
unconditionally — never the network/parse/state logic itself.

## New headless client glue: `platform/apple/visionos/tabletop/client/`

Each file is a small, explicit, named replacement for exactly the symbols
the real, linked client files call unconditionally — never a silent or
partial reimplementation of the excluded module:

| File | Replaces | What it does |
|---|---|---|
| `r_tabletop_null.c` | `renderer/` (SDL/OpenGL) | `R_GetAPI()`/`R_StdoutGetAPI()` — every entry point is a harmless placeholder or named no-op; creates no window/GL context; logs only one-time Init/Shutdown/RegisterMap events, never per-frame. Also owns `BZ_TT_Init()`/`BZ_TT_Shutdown()` pairing (bracketed by `re.Init()`/`re.Shutdown()`, the one seam guaranteed to bracket exactly one client session). |
| `s_tabletop_null.c` | `sound/` (SDL audio) | `S_Init()` logs once ("no audio backend (no-op)"); `S_PlaySound*()` are silent no-ops (called per-event, must not spam). |
| `ui_tabletop_null.c` | per-game `ui/` (menu/glue UI) | No-op `UI_GetAPI()`, plus a same-thread cache fed by `ui.UpdateUnitUI()` (from `CL_ParseUnitUI()`) that `BZTT_CopyCachedUnitUI()` reads back into `bzTTUnitLayout_t` — the one UI callback that carries data the transport's snapshot needs. |
| `cl_input_tabletop_null.c` | `cl_input.c`/`cl_input_w3.c`/`cl_input_wow.c` (SDL input) | Only the input-facing symbols other linked files call unconditionally; calls `BZ_TT_Drain()` from `CL_Input()` — the same point real mouse-driven commands would otherwise queue, always before `CL_SendCommand()`. |
| `cl_scrn_tabletop_null.c` | `cl_scrn.c` (SDL draw calls) | "Draw the frame" → `BZ_TT_PublishSnapshotFromClient()`; `svc_unit_ui` decode is forwarded verbatim to the UI glue cache. |
| `cl_console_tabletop_null.c` | `console.c` (SDL text input + ring buffer) | `CON_printf()` → `stderr` directly (more useful than the original, which was only visible if the in-game console screen was drawn). |
| `cl_fx_tabletop_null.c` | `cl_fx.c` (particle/sound entity events) | `CL_EntityEvent()` is a no-op — the event is still visible to the transport via `entityState_t.event`. |
| `bz_tabletop_client_glue.h` | — | Internal (non-ABI) seam declaring `BZTT_CopyCachedUnitUI()`; free to use engine types, unlike `bz_tabletop_transport.h`. |

`platform/apple/visionos/build.mk`'s `BZ_XR_CLIENT_SRCS` lists these files
(plus `bz_tabletop_transport.c` and the real client files) **explicitly**,
not via `find | sort` like the rest of the unity object: the null glue
files must compile ahead of real client files that call their symbols
without a header-visible prototype (e.g. `cl_view.c` calls
`CL_MouseOverGameplayUI()`, declared only in the excluded, SDL-tainted
`cl_input_local.h`) — mirroring how the desktop unity build's alphabetical
`CSRC` ordering happens to place `cl_input.c` before `cl_view.c` today.

## `bz_tabletop_transport.{h,c}` — the public ABI

`BZ_TABLETOP_ABI_VERSION` (currently `1`) must be bumped on any incompatible
struct/enum/function-signature change; the ABI is append-only (existing
fields/values are never renumbered or removed). The header includes nothing
but `<stdbool.h>`/`<stddef.h>`/`<stdint.h>` and no engine headers — every
type is a bounded, deep-copied POD value, never a live pointer into engine
state.

**Snapshots** (`bzTTSnapshot_t`, opaque) are immutable, reference-counted,
with a monotonically increasing `generation`. `BZ_TT_Latest()` returns a
retained reference the caller must `BZ_TTSnapshot_Release()`; once retained,
contents never change underneath the caller, from any thread. Accessors
expose: connection state (`bzTTConnState_t`, mirrors `connstate_t`); map
name/bounds (`false`/zeroed if no map is loaded — raw terrain tile/height
data has no public engine accessor yet, so it is intentionally **not**
exposed, a documented gap rather than fabricated data); the local player
(`bzTTPlayer_t` — number/team/color/race/uiflags/resources); selected entity
ids; visible entities (`bzTTEntity_t`, deep-copied subset of
`entityState_t`, capped at `BZ_TT_MAX_ENTITIES` = 1024 with an explicit
`EntitiesOverflowCount()` rather than silent truncation). Visibility matches
desktop `CL_AddEntities()`: only slots with `ce->current.model != 0` count
toward the cap; empty parser slots and model2/image-only slots are excluded. Fog-of-war
dimensions plus visible/explored planes; configstrings by index
(`BZ_TTSnapshot_ConfigStringCount()` returns the number of captured slots so
callers can iterate `[0, count)` without importing the engine-private
`MAX_CONFIGSTRINGS` constant; within that range `BZ_TTSnapshot_ConfigString()`
returning `false` means a validly-empty slot, and `false` at or beyond
`count` means out of range — added post-freeze, append-only, no ABI version
bump, to unblock Swift-side enumeration); and a
bounded, best-effort command-card/inventory/build-queue layout per unit
(`bzTTUnitLayout_t`, decoded from the legacy `svc_unit_ui` message — see
`CL_ParseUnitUI()` in `client/cl_scrn.c` — frequently all-zero today since
the primary command-card HUD is computed client-side in a game-specific UI
library, out of scope here; reported honestly as empty, not backfilled).

**Commands** are typed, never free-form strings, posted from any thread via
`BZ_TT_PostSelect/SmartEntity/SmartPoint/Button/Cancel()` into a bounded
256-entry (`BZ_TT_COMMAND_QUEUE_CAPACITY`) ring buffer. Every Post validates
entity ids (`< BZ_TT_ENTITY_ID_LIMIT`), counts (`<=
BZ_TT_MAX_SELECT_IDS_PER_COMMAND`), coordinates (finite floats), payload
length (button codes must be exactly `BZ_TT_BUTTON_CODE_LEN` = 4 bytes), the
caller's `abi_version` against `BZ_TABLETOP_ABI_VERSION`, and an optional
`observed_generation` staleness check (pass 0 to skip; otherwise rejected
with `BZ_TT_ERR_STALE_GENERATION` if a strictly newer snapshot has since
published). `BZ_TT_Drain()` — engine/client thread only, not safe to call
concurrently with itself — encodes every queued command through the
existing `clc_stringcmd` network command path (`select`/`smart`/
`smartpoint`/`button`/`cancel`, wire strings built with `SZ_Printf`), so a
native/Swift caller can never mutate game state directly; it only ever
reaches the game through the same command path a real player's input would.

**Synchronization**: one lock backs `Init`/`Shutdown`/`Publish`/`Latest`/
`Retain`/`Release`/`Post*`/`Drain`. `Publish`/`Drain` recheck
initialized/terminal state while holding that same lock, so `Shutdown`
(idempotent, safe from any thread) can never race a concurrent reader/
writer into observing half-torn-down state; the lock itself is a static
POSIX object, never destroyed, so no path can ever reach a destroyed mutex.

## Client integration order (per frame)

`BZ_TT_Drain()` runs from `CL_Input()`, **before** `CL_SendCommand()` in the
normal per-frame client loop — so typed commands are encoded into the same
outgoing netchan message a real player's input would have populated.
`BZ_TT_PublishSnapshotFromClient()` runs from `SCR_UpdateScreen()`'s
replacement, **after** the client has finished reading and parsing all
pending server packets for that frame (`cl.ents`/`cl.playerstate`/
`cl.selection`/`cl.fow`/configstrings are therefore always internally
consistent in a published snapshot — never a partially-applied delta).
Existing loopback listen-server semantics (`common/net.c`'s
`NET_SendLoopPacket`/`NET_GetLoopPacket`) are unmodified; the desktop
`openwarcraft3`/`opensc2` executables never link this transport at all, so
their behavior is unaffected.

## Testing

```sh
make test-bz-tabletop-transport   # this layer's transport suite alone
make test                         # full suite, includes the above
```

Three new files under `games/warcraft-3/tests/`:

- `test_bz_tabletop_transport.c` — ABI-level unit suite: lifecycle
  (not-initialized/terminal error ordering — see note below), snapshot
  generation/immutability/refcounting, content correctness (player,
  configstrings, map bounds, entities, selection, fog), every
  `BZ_TT_Post*` command plus its invalid inverse, queue overflow at
  capacity, stale-generation rejection, and concurrency stress tests
  (concurrent readers vs. publisher, terminal cleanup races, repeated
  publish/shutdown races).
- `test_bz_tabletop_transport_client.c` — integration tests using the
  **real** production wire-format functions, not a parallel reimplementation:
  builds a genuine wire message with `MSG_WriteDeltaEntity`/
  `MSG_WriteDeltaPlayerState`, decodes it with the real
  `CL_ParseServerMessage()`, then asserts `BZ_TT_PublishSnapshotFromClient()`
  produces the expected snapshot; and a real-loopback command-delivery test
  that drains a posted command through genuine `Netchan_Transmit()` →
  `NET_SendLoopPacket()` → `NET_GetPacket()` plumbing (no UDP socket or full
  server bootstrap needed — `cls.netchan.remote_address.type = NA_LOOPBACK`
  is sufficient).
- `test_bz_tabletop_transport_stubs.c` — the handful of link-time-only
  symbols the real linked files still need (`CL_BeginLoadingMap`,
  `Cvar_Integer/String`, `MemAlloc/MemFree`, `CM_GetWorldBounds`,
  `BZTT_CopyCachedUnitUI`, ...) instead of linking the full cvar/cmd
  subsystem or the real same-thread UI cache.

`games/warcraft-3/game.mk`'s `TEST_SRCS` wildcard (`find ... -name
'test_*.c'`) excludes all four of the above files explicitly, the same way
it already excludes `test_bz_tabletop_lifecycle.c` and friends — otherwise
they would also be swept into the main `test_openwarcraft3` binary, which
does not link `bz_tabletop_transport.c`, producing undefined-symbol link
errors. Any future `test_bz_tabletop_*` file must be added to this
exclusion list.

**Note on the "not initialized" vs. "terminal" test**: the transport's
globals default `g_initialized = false` and `g_terminal = true` (i.e. both
are true pre-`Init()`), and validation checks `!g_initialized` before
`g_terminal`. `BZ_TT_Shutdown()` never resets `g_initialized`, so
"not-initialized" is only observable as the very first test to touch the
module in a process, before any `BZ_TT_Init()` call has ever run — the test
file documents this ordering dependency inline.

Verified clean under ThreadSanitizer and AddressSanitizer +
UndefinedBehaviorSanitizer (410/410 assertions passing under all three
configurations, plus the plain build).

## Wire-protocol quirks discovered while building the real-parse test

- `common/msg.c`'s `entityStateFields[]`/`playerStateFields[]` tables are
  the ground truth for what actually crosses the wire.
  `entityState_t.sound/event/renderfx/ability` and
  `playerState_t.selected_entity` are **not** wire-transmitted (client-local
  only) — the transport still reports them from `cl.ents`, just never
  expect them to reflect a just-received packet delta in a test.
  `playerState_t.stats[]` wire-transmits only even indices among
  `{0,2,4,6,8,16,18,20,...}`; `PLAYERSTATE_RESOURCE_GOLD` (1),
  `_HERO_TOKENS` (3), and `_FOOD_USED` (5) are **not** wire indices — a
  pre-existing protocol characteristic, not a bug.
- `CL_ReadPacketEntities()` always sets `cl.num_entities =
  MAX_CLIENT_ENTITIES` (16384) regardless of how many entities a packet
  actually touched. Most are zero-model slots. Snapshot construction applies
  the established desktop predicate `ce->current.model != 0` before copy/cap
  accounting, so overflow reports only active entities beyond 1024.
  - One active client slot is one transport entity even when `model2` makes the
    desktop renderer emit a second attached render entity. Human02 therefore
    stabilizes at 2,397 transport entities (`1024 + overflow 1373`) while the text
    renderer reports 2,398 render entities; the one-entity difference is the
    attachment, not a dropped active slot. Three bounded equivalent-lifecycle
    runs identified the same source each time: client slot 67, class `hC02`,
    `model=28`, `model2=121`, connection state 3.
- `Netchan_Transmit()` resets `netchan->message.cursize` to 0 after handing
  off to `NET_SendPacket()`, and is a no-op if `cursize == 0` — useful for
  asserting both that a real send actually ran, and that draining an empty
  command queue never transmits an empty datagram.

## What Layer 2 does not do

- No Swift/SwiftUI/RealityKit code, no app creation/signing, no asset decoding/
  export, no visible rendering, no audio, no menus, no multiplayer beyond
  what `common/net.c` already provides headlessly (unchanged from Layer 1).
- No raw terrain tile/height data in snapshots (no public engine accessor
  exists yet — documented gap above, not silently substituted).
- No free-form command strings cross the ABI — only the five typed
  `bzTTCommandType_t` variants.
- Never reads server `ge->edicts` directly, and never adds fields to
  `entityState_t`, `playerState_t`, renderer structs, or game import/export
  APIs — every new field lives in the transport's own `bzTT*_t` value
  types.

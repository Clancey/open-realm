# visionOS tabletop runtime (Layer 1: callable static engine)

Layer 1 establishes a callable, statically-linked, visionOS-compatible
Warcraft III engine runtime that a later layer can host from Swift/RealityKit
without disturbing the desktop `openwarcraft3`/`opensc2` executables. This
layer does **not** implement the SwiftUI/RealityKit tabletop UI, asset
export/MPQ bundling, gameplay controls, multiplayer, or a later "snapshot
bridge" — those are separate, later layers.

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
- **`platform/apple/visionos/tabletop/null/cl_null.c`** — a real, log-once
  (never silent) null implementation of the handful of `CL_*`/`Key_*`/
  `Cmd_ForwardToServer` symbols `common/bz_runtime.c` and `common/common.c`
  call unconditionally. It replaces the entire `client/` module (SDL window,
  SDL input polling, OpenGL renderer dispatch) for this static archive — the
  real `common/*.c` and `server/*.c` never reference `client/`-only symbols
  directly, so this is the only replacement needed. See its header comment
  for the exact symbol list and log-once rationale.
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
build/lib/visionos/<platform>/libopenwarcraft3-engine.a   # common/ + server/ + cl_null.c + game/jass/sheet/shared
build/lib/visionos/<platform>/libopenwarcraft3-bridge.a   # bz_tabletop_lifecycle.o + bz_tabletop_bridge.o
build/lib/visionos/<platform>/bridge-link-smoke           # link-check binary (built, not run — see below)
```

`BZ_XR_MIN_OS` (default `1.0`) controls the `-target arm64-apple-xros<ver>[-simulator]`
triple's minimum OS version; override with `make xrsimulator BZ_XR_MIN_OS=2.0` if a
later layer needs a newer floor.

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

- `BZ_TabletopCreate(argc, argv)` — allocate, deep-copy args, no thread yet.
- `BZ_TabletopStart(lc)` — spawns one dedicated engine thread (never the
  caller's thread), blocks until `BZ_RuntimeInit()` completes (state leaves
  `STARTING`). Safe to call again after `FAILED`/`STOPPED` to restart — any
  previous thread is joined first.
- `BZ_TabletopSuspend`/`Resume(lc)` — pause/resume per-frame ticking without
  tearing the engine down.
- `BZ_TabletopStop(lc)` — requests an orderly shutdown. From any thread other
  than the engine thread, blocks until `BZ_RuntimeShutdown()` has run and the
  engine thread has exited, then joins it. **Re-entrant**: if called from
  *inside* the engine thread itself (see `Sys_Quit()` below), it only flags
  the stop request and returns immediately — a thread cannot join itself —
  leaving the actual join for a later call from another thread.
  Idempotent: safe to call repeatedly, from any state. If two external
  threads call `BZ_TabletopStop()` (or one calls `Stop()` while another calls
  `Start()`'s restart-join path) at the same time, only one of them actually
  performs `pthread_join()` — a mutex-guarded `joining` flag makes the rest
  wait on a condvar for that join to finish instead of racing their own
  `pthread_join()` call against it, since POSIX leaves concurrent joins on
  the same thread undefined.
- `BZ_TabletopDestroy(lc)` — calls `BZ_TabletopStop()` first, then frees.
- `BZ_TabletopGetState`/`LastError(lc)` — read-only, thread-safe.

### Threading contract

Every `BZ_RuntimeInit`/`Frame`/`Shutdown` call happens on the one dedicated
engine thread `BZ_TabletopStart()` creates — satisfying
`common/bz_runtime.h`'s "callers must serialize onto one thread"
requirement. The engine thread's loop:

1. `BZ_RuntimeInit()` → publish `RUNNING` or `FAILED` (unblocks `Start()`).
2. Loop: wait out `SUSPENDED`, then `BZ_RuntimeFrame(elapsed_ms)` using
   `clock_gettime(CLOCK_MONOTONIC, …)` (never `SDL_GetTicks`, since no SDL is
   linked). No display link drives frames yet in this layer — the loop
   paces itself with a 16ms `nanosleep` between frames; a later layer swaps
   this for a real display-link-driven `BZ_TabletopTick()`-style call if
   needed.
3. Break the loop on an external stop request **or** `BZ_RuntimeFrame()`
   returning `false` (the frame limit or a console "quit" was reached
   internally) — both cases call `BZ_RuntimeShutdown()` (idempotent) before
   publishing `STOPPED`.

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
on the same thread undefined); restart after stop correctly rejoins the
previous thread and reaches `RUNNING` again; `BZ_TabletopDestroy()` without a
prior explicit stop still joins cleanly; and reaching `+com_frame_limit`
drives the engine to `STOPPED` on its own (via the `Sys_Quit()` thread-local
path) without any external `BZ_TabletopStop()` call. The concurrent-stop
scenario was additionally verified with ThreadSanitizer
(`-fsanitize=thread`), which reported zero data races across five runs after
the join-serialization fix (an earlier revision without it reliably aborted
under TSan inside `pthread_join`).

## What this layer does not do

- No SwiftUI/RealityKit UI (later layer).
- No snapshot bridge / entity-state serialization (later layer).
- No Warcraft III asset export or MPQ bundling into an app bundle (later
  layer — this layer only proves the engine *links*, it does not ship data).
- No gameplay input/controls and no multiplayer/networking beyond what
  `common/net.c` already provides headlessly.
- No SDL/OpenGL window, no SDL input polling, no Xcode project.

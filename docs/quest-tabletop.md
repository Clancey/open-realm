# Meta Quest (Android/NDK + OpenXR) tabletop shell — Layers 5A/5B/5C

This document tracks layers 5A/5B/5C of a stacked Meta Quest port:

- Layer 1: [docs/visionos-tabletop.md](visionos-tabletop.md)'s extraction of
  `platform/tabletop/` — the portable pthreads lifecycle host and headless
  null client/renderer/UI seams — into shared, platform-neutral C.
- Layer 2 (`clancey-quest-android-openxr-shell`): a native Android/NDK +
  Khronos OpenXR build shell under `platform/android/quest/` that statically
  links the headless Warcraft III engine/game/asset/jass/sheet/shared source
  groups plus the shared tabletop lifecycle host, and proves the Khronos
  OpenXR Android loader + a minimal `xrCreateInstance` probe.
- Layer 3: replaces the layer-2 instance probe with a real OpenXR
  **session**, a Vulkan **stereo frame loop**, an `XR_FB_passthrough`
  **compositor layer**, and a minimal **head-tracked tabletop test scene**.
- Layer 4 (`clancey-quest-tabletop-lifecycle-bridge`): connects that host to
  the real, shared authoritative tabletop engine — a Quest-owned
  bridge/session adapter
  (`bz_quest_bridge.h`/[bz_quest_bridge.c](../platform/android/quest/app/src/main/cpp/bz_quest_bridge.c))
  creates/starts/suspends/resumes/stops/destroys the portable
  `platform/tabletop/bridge/bz_tabletop_lifecycle.h` engine-thread state
  machine on the matching Android lifecycle transitions; a deterministic,
  overridable data-directory resolver
  (`bz_quest_data.h`/`.c`) builds the engine's startup argv; and a plain-C
  diagnostic frame descriptor (`bz_quest_frame.h`/`.c`,
  `bz_quest_snapshot.h`/`.c`) proves the immutable tabletop snapshot really
  advances via a throttled log line — without drawing any Warcraft content.
- **Layer 5A (this layer, `clancey-quest-renderer-static-models`)**: the
  first renderer slice. Consumes the layer-4 tabletop asset/snapshot ABIs on
  the XR render thread to draw **static** Warcraft III model geometry and
  materials at their authoritative snapshot transforms, with real Vulkan GPU
  resource ownership/caching/upload. See
  "[Layer 5A: static Warcraft III model rendering](#layer-5a-static-warcraft-iii-model-rendering-bz_quest_wc3_c-bz_quest_vk_wc3c)"
  below for full scope, ownership, and evidence.
- **Layer 5B (`clancey-quest-renderer-terrain`)**: adds Warcraft III terrain
  rendering (ground layers, cliffs, water) using the shipped
  `bz_tabletop_assets.h` terrain ABI, Quest-local pure/capture/Vulkan modules,
  and one shared per-eye render pass interleaving terrain opaque -> model
  opaque -> terrain blended -> model blended.
- **Layer 5C (this layer, `clancey-quest-model-animation`)**: adds
  authoritative Warcraft III **model animation** on top of 5A's static
  models — skeletal/vertex hierarchy, sequence/global-sequence pose
  sampling, GPU (vertex-shader) skinning, and the dynamic material state
  (geoset alpha, team color/glow layer selection) an animated unit/building/
  doodad needs. See
  "[Layer 5C: Warcraft III model animation](#layer-5c-warcraft-iii-model-animation-bz_quest_wc3_anim-bz_quest_wc3_capturec-bz_quest_vk_wc3c)"
  below for full scope, ABI decision, ownership, and evidence.

**These layers still do not render fog of war, selection decals, particles/
effects, command-card/HUD surfaces, and still do not poll gameplay input,
play audio, or stage WC3 data onto the device.** See
[Current limitations](#current-limitations) and
`bz_quest_host.c`'s compile-time seams (`BZ_QUEST_ENABLE_*`, each guarded by
a `#error` until its real implementation lands) — `BZ_QUEST_ENABLE_ENGINE_START`
and `BZ_QUEST_ENABLE_BRIDGE_SNAPSHOTS` were layer 4's two seams;
`BZ_QUEST_ENABLE_WC3_RENDERER` is the one seam *layer 5A* replaces with a
real implementation (`BZ_QUEST_ENABLE_INPUT`, `BZ_QUEST_ENABLE_AUDIO`, and
`BZ_QUEST_ENABLE_DATA_STAGING` remain `#error`-gated for a later layer).

## Architecture boundary

```text
platform/android/quest/
  app/src/main/
    AndroidManifest.xml         # NativeActivity + Quest 3/3S manifest metadata
    cpp/
      CMakeLists.txt            # fetches BZ_XR_* source lists/cflags from
                                 # platform/apple/visionos/build.mk via
                                 # `make print-<VAR>`; links Vulkan + OpenXR;
                                 # regenerates shaders via build-shaders.sh
      bz_quest_host.c           # android_native_app_glue entry point: Android
                                 # command/lifecycle handling, drives
                                 # bz_quest_renderer's init/frame/shutdown
      bz_quest_log.h            # shared BZ_QUEST_LOGI/LOGE (tag baked in)
      bz_quest_pure.h/.c        # plain-C math/selection helpers (host-tested):
                                 # FOV->Vulkan projection, pose->view matrix,
                                 # mat4 multiply, format/extension-name
                                 # selection, passthrough-capability check,
                                 # XrVersion->VkVersion conversion
      bz_quest_scene.h/.c       # procedural tabletop test-scene generator
                                 # (host-tested, no Vulkan/OpenXR deps)
      bz_quest_xr.h/.c          # OpenXR instance/system/session/space/
                                 # swapchain/frame-loop/event lifecycle
      bz_quest_vk.h/.c          # Vulkan instance/device (via
                                 # XR_KHR_vulkan_enable2), render pass/
                                 # pipeline/per-eye render targets, per-frame
                                 # record+submit
      bz_quest_passthrough.h/.c # XR_FB_passthrough create/start/pause/
                                 # destroy + composition layer
      bz_quest_renderer.h/.c    # glues xr+vk+passthrough+scene into one
                                 # init()/frame()/shutdown() the host calls
      bz_quest_data.h/.c        # Warcraft III data-dir resolution + engine
                                 # argv construction (layer 4, host-tested,
                                 # no Android/OpenXR/Vulkan deps)
      bz_quest_frame.h/.c       # plain-C diagnostic frame descriptor:
                                 # snapshot/lifecycle-state summary + a pure
                                 # throttled-log decision (layer 4,
                                 # host-tested, no engine link dependency)
      bz_quest_snapshot.h/.c    # thin real BZ_TT_Latest()/BZ_TTSnapshot_*()
                                 # reader that populates a bz_quest_frame.h
                                 # descriptor and releases the snapshot every
                                 # call (layer 4, link-tested only, not
                                 # host-unit-tested - see its header comment)
      bz_quest_bridge.h/.c      # Quest-owned lifecycle/session adapter:
                                 # create/start/suspend/resume/stop/destroy
                                 # of bzTabletopLifecycle_t (layer 4,
                                 # host-tested against the real lifecycle +
                                 # runtime, no Android/OpenXR/Vulkan deps)
      shaders/
        tabletop_vert.vert      # GLSL source (committed) - the only "shader
        tabletop_frag.frag      # truth" in the repo; see "Shader build
                                 # pipeline" below
    res/values/strings.xml
  build.gradle, app/build.gradle, settings.gradle, gradle.properties
  gradlew, gradle/wrapper/       # pinned Gradle wrapper (see Prerequisites)
  scripts/
    verify-native-lib.sh         # forbidden-dependency/ABI/entry-point check
    test-source-sync.sh          # Make -> CMake source-list sync-contract test
    build-shaders.sh             # GLSL -> SPIR-V -> embedded-C-header pipeline
    bin2c.c                      # host tool: SPIR-V binary -> aligned C uint32_t array
  tests/
    test_bz_quest_pure.c         # bz_quest_pure.c unit tests (host-buildable)
    test_bz_quest_scene.c        # bz_quest_scene.c unit tests (host-buildable)
    test_bz_quest_data.c         # bz_quest_data.c unit tests (layer 4, host-buildable)
    test_bz_quest_frame.c        # bz_quest_frame.c unit tests (layer 4, host-buildable)
    test_bz_quest_pure_main.c    # runs the four suites above, wired into `make test`
    test_bz_quest_bridge.c       # bz_quest_bridge.c tests (layer 4) linking the
                                 # REAL bz_tabletop_lifecycle.c/bz_runtime.c,
                                 # exactly like games/warcraft-3/tests/
                                 # test_bz_tabletop_lifecycle.c already does
    test_bz_quest_bridge_main.c  # runs that suite, wired into `make test`
platform/android/quest/build.mk  # thin `make quest*`/`test-quest-*` wrappers
```

No SDL2, desktop OpenGL, Apple/Objective-C/Swift, `common/main.c`, or Meta's
proprietary Mobile SDK/VrApi may enter `platform/android/quest/`; this is
enforced by `scripts/verify-native-lib.sh` (run via `make
quest-verify-native-lib`), not just by convention. Every OpenXR/Vulkan/
Android type (`Xr*`, `Vk*`, `ANativeActivity`/`android_app`, etc.) is
confined to this directory's `bz_quest_*` modules — `platform/bridge/` and
`platform/tabletop/` headers never see one of these types.

### Source-list synchronization contract

Unchanged from layer 2: `platform/apple/visionos/build.mk` defines
`BZ_XR_ENGINE_SRCS`, `BZ_XR_GAME_SRCS`, `BZ_XR_ASSET_SRCS`,
`BZ_XR_JASS_SRCS`, `BZ_XR_SHEET_SRCS`, `BZ_XR_SHARED_SRCS`,
`BZ_XR_BASE_CFLAGS`, `BZ_XR_CFLAGS`, and `BZ_XR_FDF_CFLAGS` — all confirmed
100% portable (no Apple/arch-specific flags). Rather than copy those file
lists into a second, driftable literal for CMake,
`platform/android/quest/app/src/main/cpp/CMakeLists.txt` shells out to
`make -s print-<VAR>` (using the root `Makefile`'s generic
`print-%: ; @echo $*=$($*)` target) at configure time for exactly those
nine variables, fails the configure loudly (`FATAL_ERROR`) if any resolve
empty or unexpectedly, and generates one "unity" translation unit per
source group (mirroring `platform/apple/visionos/build.mk`'s own
`bz_xr_unity_o` unity-build macro, which some files' sibling-static-helper
visibility depends on).

`scripts/test-source-sync.sh` (`make test-quest-source-sync`, wired into
`make test`, no Gradle/NDK required) independently asserts all nine
variables still resolve non-empty and still contain a known sentinel file
from each group, and that an undefined variable name still resolves to an
empty `NAME=` line — catching drift in `build.mk` before it ever reaches a
slow Gradle/CMake configure.

`Sys_Quit()` is supplied by
`platform/tabletop/bridge/bz_tabletop_lifecycle.c` (not `common/main.c`,
which this build never links — verified absent from the linked `.so` via
`verify-native-lib.sh`).

Layer 4's four new Quest-only files (`bz_quest_data.c`, `bz_quest_frame.c`,
`bz_quest_snapshot.c`, `bz_quest_bridge.c`) are added directly to
`bz_quest_native`'s own literal source list in `CMakeLists.txt` — unlike the
nine `BZ_XR_*` variables above, they are not shared/portable engine source
groups, only Quest-specific host code, so there is nothing for
`print-<VAR>`/`test-source-sync.sh` to keep in sync for them.

## Prerequisites

Toolchain versions actually installed and used to build/verify this layer,
verified from this machine on 2026-07-31 (rerun the version commands below
before relying on them — this is one developer's toolchain snapshot, not a
pinned/reproducible-build guarantee):

| Tool | Verified version | Command |
|---|---|---|
| Android NDK | 27.2.12479018 | `ls "$ANDROID_HOME/ndk"` |
| Android SDK build-tools | up to 36.1.0 | `sdkmanager --list_installed` |
| CMake (SDK-managed, used by AGP) | 3.22.1 | `app/build.gradle`'s `externalNativeBuild.cmake.version` |
| Gradle (wrapper, pinned) | 8.9 | `platform/android/quest/gradle/wrapper/gradle-wrapper.properties` |
| Android Gradle Plugin | 8.5.2 | `platform/android/quest/build.gradle` |
| Java | Temurin 17 (`JAVA_HOME` must point here for AGP 8.5.x) | `/usr/libexec/java_home -V` |
| NDK `glslc` (shader compiler) | bundled at `$ANDROID_NDK_HOME/shader-tools/<host>/glslc` | `scripts/build-shaders.sh` resolves it automatically |
| adb | present, used only for `make quest-install-debug` (untested against real hardware) | `adb --version` |

Set `JAVA_HOME` to a JDK 17 install and `ANDROID_HOME`/`local.properties`'s
`sdk.dir` to your Android SDK before building — `local.properties` is
machine-local and gitignored, never committed.

## Build/run/log commands

```sh
# Fast, Gradle/NDK-free: fails loudly if the Make->CMake source-list sync
# contract above ever drifts. Runs as part of `make test`.
make test-quest-source-sync

# Host-native (no NDK/Gradle/Quest hardware) unit tests for bz_quest_pure.c/
# bz_quest_scene.c (projection/view-matrix math, format/extension/
# passthrough-capability selection, procedural scene generator), (layer 4)
# bz_quest_data.c/bz_quest_frame.c (data-dir/argv resolution, diagnostic
# frame descriptor + throttled-log decision), PLUS (layer 5A)
# bz_quest_wc3_render.c/bz_quest_wc3_cache.c (coordinate/scale math,
# render-list construction, GPU-cache hit/miss/eviction/shutdown
# bookkeeping - see "Layer 5A" below). Runs as part of `make test`.
make test-quest-host-tests

# Layer 4: bz_quest_bridge.c tests linking the REAL
# platform/tabletop/bridge/bz_tabletop_lifecycle.c and common/bz_runtime.c
# (not a stub) - exercises actual BZ_TabletopCreate/Start/Suspend/Resume/
# Stop/Destroy transitions against a real (synthetic test-fixture) data
# directory. Depends on `make test-assets` (builds build/tests/tests.mpq)
# and the shared/sheet dynamic libs, exactly like
# games/warcraft-3/game.mk's test-bz-tabletop-lifecycle. Runs as part of
# `make test`.
make test-quest-bridge

# Regenerate the SPIR-V-embedded shader header standalone (also run
# automatically by the CMake build below via a custom_command). Layer 5A
# added warcraft_vert.vert/warcraft_frag.frag alongside the existing
# tabletop_vert.vert/tabletop_frag.frag - see "Layer 5A" below.
ANDROID_NDK_HOME=/path/to/ndk platform/android/quest/scripts/build-shaders.sh /tmp/shader-out

# Full Gradle/CMake debug build (arm64-v8a only). Also compiles shaders,
# builds the Vulkan/OpenXR/passthrough renderer plus (layer 5A) the
# Warcraft III model renderer, and links everything into one .so.
JAVA_HOME=/path/to/temurin-17 make quest-assemble-debug

# The above, plus the forbidden-dependency/ABI/entry-point check (now also
# allow-lists libvulkan.so).
JAVA_HOME=/path/to/temurin-17 make quest-verify-native-lib

# Convenience target bundling everything that doesn't need physical
# hardware (source-sync test + full build + native-lib verification).
JAVA_HOME=/path/to/temurin-17 make quest

# Installs (does not launch) the built debug APK via adb - untested against
# real Quest hardware, see Current limitations.
JAVA_HOME=/path/to/temurin-17 make quest-install-debug
```

### Exact on-device acceptance procedure (requires a connected Quest 3/3S)

None of the following was run against physical hardware in this session (no
device was connected) — this is the exact procedure to run it, and the exact
log lines that indicate success at each stage:

```sh
# 1. Build + install.
JAVA_HOME=/path/to/temurin-17 make quest-assemble-debug
adb install -r platform/android/quest/app/build/outputs/apk/debug/app-debug.apk

# 2. Launch (NativeActivity's default launch intent) and tail this app's log tag.
adb shell am start -n org.openrealm.quest/android.app.NativeActivity
adb logcat -s OpenRealmQuest:V

# 3. Also watch for OpenXR loader/runtime broker errors under its own tags
#    (separate process, not this app's PID):
adb logcat -s OpenXR:V VrApi:V
```

Expected `OpenRealmQuest` log sequence on success, in order (each line's
literal text, from the exact `BZ_QUEST_LOGI`/`BZ_QUEST_LOGE` call sites in
`bz_quest_host.c`/`bz_quest_xr.c`/`bz_quest_vk.c`/`bz_quest_passthrough.c`):

1. `bz_quest_host: starting (layer 4: tabletop lifecycle/snapshot bridge)`
2. `APP_CMD_START`
3. `xrInitializeLoaderKHR succeeded`
4. `xrGetSystem succeeded: systemName=... vendorId=... passthroughCapabilities=0x...`
   (the `0x...` bitmask must have `XR_PASSTHROUGH_CAPABILITY_BIT_FB` set —
   see "Hardware-only acceptance gates" below for what happens if not)
5. `Vulkan API version bound: min=1.x max=1.x`
6. `swapchain[0]: WxH, N images` and `swapchain[1]: WxH, N images`
7. `passthrough object + reconstruction layer created`
8. `passthrough started`
9. `bz_quest_renderer_init succeeded`
10. Either `bz_quest_bridge_start succeeded (data dir '<path>')` (see
    "Data-path contract" below for exactly which path this is) **or**
    `bz_quest_bridge_start failed: <reason> - see docs/quest-tabletop.md's
    data-path contract; continuing to pump the Android event loop with no
    engine running` — a data/archive resolution failure here is a real,
    documented Quest-only failure mode, not a crash: OpenXR/Vulkan/
    passthrough keep running with the checkerboard test scene and the app
    stays fully responsive to Android teardown (see "Hardware/data-only
    acceptance procedure" below).
11. `APP_CMD_RESUME`
12. Repeated `XrEventDataSessionStateChanged: state=...` lines progressing
    `READY` -> `xrBeginSession succeeded` -> (eventually) `SYNCHRONIZED` ->
    `VISIBLE` -> `FOCUSED`.
13. If step 10 succeeded, exactly one `tabletop frame: status=... generation=0
    lifecycleState=...` line shortly after (the first-ever captured frame,
    `bz_quest_frame_should_log()`'s "status changed from nothing captured
    yet" case — see "Diagnostics: throttled log, never per-frame" below),
    then **no further** `tabletop frame: ...` lines unless `status`/
    `lifecycleState`/`lifecycleError` actually changes (there is no map
    loaded in this layer, so `generation` still advances once per client
    frame — see "Snapshot ownership and diagnostics" below — but a bare
    `generation` advance is never, by itself, a log trigger, so this line
    does not repeat merely because the engine is running).
14. No further `BZ_QUEST_LOGE` lines once `FOCUSED` is reached and frames are
    flowing (a healthy frame loop produces **no** per-frame log output at
    all — see "No busy loop / no per-frame logging" below).

Visual acceptance (must be confirmed by a human wearing the headset — no
automated check exists for this): the passthrough camera feed is visible as
the background, and a checkerboard tabletop with four colored cubes appears
roughly at waist height ~1m in front of the headset's `LOCAL` origin, with
correct stereo separation (looking left/right shows appropriate parallax)
and correct occlusion (cubes closer to the eye occlude the table behind
them, verifying the depth buffer is wired correctly).

## Testing

Every pure/host-testable module has a matching test file run by `make
test`, none of which need Gradle/NDK/Quest hardware:

| Module | Test file | What it proves |
|---|---|---|
| `bz_quest_pure.c` | `tests/test_bz_quest_pure.c` | Projection/view-matrix math, format/extension/passthrough-capability selection (unchanged from layer 3). |
| `bz_quest_scene.c` | `tests/test_bz_quest_scene.c` | Procedural test-scene generator (unchanged from layer 3). |
| `bz_quest_data.c` | `tests/test_bz_quest_data.c` | Default-dir construction, override read/validate (normal + every documented rejection: relative path, disallowed characters, oversized, empty), full resolve fallback order, and argv construction (normal + undersized-buffer/NULL-arg error paths) — 22 tests, pure, real temp dirs via `mkdtemp()`. |
| `bz_quest_frame.c` | `tests/test_bz_quest_frame.c` | Reset value, `bz_quest_frame_from_values()` field copy/truncation/ABI-mismatch detection, and every `bz_quest_frame_should_log()` cache-hit/cache-miss branch (identical frame never logs; status/lifecycle-state/lifecycle-error changes each log; a bare `generation` advance — in *any* status, including `OK` — never logs, including across ~190 simulated consecutive engine frames, guarding against the per-frame-log regression fixed in PR #19's review pass) — 15 tests, pure, no I/O. |
| `bz_quest_bridge.c` | `tests/test_bz_quest_bridge.c` | Valid-override start reaches `RUNNING`; missing-data start reaches `FAILED` with the engine's own error surfaced; invalid-override start reaches `FAILED` *before* any `bzTabletopLifecycle_t` exists; a second `start()` on an already-attempted instance is rejected; suspend/resume forward correctly (and are safe no-ops before a start or after a stop); `stop()` is idempotent and safe pre-start; `destroy()` then a fresh `start()` on the same storage succeeds; `is_terminal()` is correct for every bridge state — 10 tests, **linking the real** `bz_tabletop_lifecycle.c`/`common/bz_runtime.c` (not a stub), 67 assertions. |

`bz_quest_bridge.c`'s tests are the one suite in this table that needs
`make test-assets` (to produce a synthetic `build/tests/tests.mpq` data
directory) and the `shared`/`sheet` dynamic libs — wired automatically as
target dependencies of `make test-quest-bridge` (see "Build/run/log
commands" above), exactly mirroring `games/warcraft-3/game.mk`'s existing
`test-bz-tabletop-lifecycle` recipe.

No `+com_frame_limit`-bounded full engine run was additionally attempted on
this host beyond what `test_bz_quest_bridge.c` already exercises: this
machine has no full retail `War3.mpq`/`War3x.mpq` data, only the synthetic
`test-assets` fixture, and `bz_quest_bridge_stop()` (not a frame limit) is
what ends each bridge test's engine thread — see "Hardware/data-only
acceptance procedure" above for exactly what this does and does not prove.

## OpenXR session lifecycle (`bz_quest_xr.c`)

### Required instance extensions

Exactly three, enumerated via `xrEnumerateInstanceExtensionProperties` and
checked with `bz_quest_check_required_names()` (a host-tested pure helper)
before `xrCreateInstance` — a missing one is a hard startup failure, not a
silently-skipped feature:

| Extension | Why required |
|---|---|
| `XR_KHR_android_create_instance` | Mandatory on Android — `xrCreateInstance` needs the JavaVM/Activity `next`-chain entry. |
| `XR_KHR_vulkan_enable2` | This renderer's only supported graphics API — no OpenGL ES, no legacy `XR_KHR_vulkan_enable`. |
| `XR_FB_passthrough` | Quest 3/3S MR passthrough is a hard prototype requirement (see "Passthrough is mandatory" below), not optional. |

### Init sequence (`bz_quest_renderer_init()`, in `bz_quest_renderer.c`)

1. `bz_quest_xr_init_loader` — `xrInitializeLoaderKHR` via
   `XR_KHR_loader_init_android`, unchanged from layer 2, must run before any
   other OpenXR call.
2. `bz_quest_xr_create_instance` — enumerates + requires the three
   extensions above, then `xrCreateInstance`.
3. `bz_quest_xr_get_system` — `xrGetSystem(HEAD_MOUNTED_DISPLAY)`, then
   `xrGetSystemProperties` with an `XrSystemPassthroughProperties2FB`
   `next`-chain entry to read `passthroughCapabilities`. **Hard-fails if
   `XR_PASSTHROUGH_CAPABILITY_BIT_FB` is absent** (gate #1 of 2 — see
   "Passthrough is mandatory").
4. `bz_quest_xr_load_functions` — resolves every `XR_KHR_vulkan_enable2` and
   `XR_FB_passthrough` function pointer via `xrGetInstanceProcAddr`
   individually, each checked for `XR_SUCCESS`/non-NULL with the exact
   function name logged on failure (`bz_quest_xr_resolve()`).
5. `bz_quest_xr_get_vulkan_requirements` —
   `xrGetVulkanGraphicsRequirements2KHR`, giving the
   `minApiVersionSupported`/`maxApiVersionSupported` bit-packed `XrVersion`
   range the Vulkan instance's `VkApplicationInfo::apiVersion` must satisfy.
6. `bz_quest_vk_create_instance` / `bz_quest_vk_create_device` — see
   "Vulkan device creation via XR_KHR_vulkan_enable2" below.
7. `bz_quest_xr_select_blend_mode` — enumerates
   `xrEnumerateEnvironmentBlendModes` and **hard-fails if
   `XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND` is unsupported** (there is no
   fallback to `OPAQUE`/`ADDITIVE` — an opaque blend mode would silently
   defeat the whole point of an MR passthrough prototype).
8. `bz_quest_xr_enumerate_views` — `xrEnumerateViewConfigurationViews` for
   `XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO`, filling
   `xr->viewConfigs[BZ_QUEST_VIEW_COUNT]` (recommended/max swapchain
   width/height/sample count per eye).
9. `bz_quest_xr_create_session` — builds an `XrGraphicsBindingVulkanKHR`
   from the Vulkan instance/physicalDevice/device/queueFamilyIndex the
   previous steps created, then `xrCreateSession`.
10. `bz_quest_xr_create_space` — `xrCreateReferenceSpace` with
    `XR_REFERENCE_SPACE_TYPE_LOCAL` (seated/standing origin at startup
    position — not `STAGE`, since this prototype does not depend on a
    room-scale guardian boundary being configured).
11. `bz_quest_xr_create_swapchains` — for each eye, picks the first runtime-
    supported format from a caller-supplied preference list (see "Swapchain
    format selection" below) via `xrEnumerateSwapchainFormats` +
    `bz_quest_select_swapchain_format()` (host-tested pure helper), then
    `xrCreateSwapchain` + `xrEnumerateSwapchainImages`.
12. `bz_quest_vk_create_render_resources` / `bz_quest_vk_create_targets` —
    see "Vulkan render pass/pipeline/targets" below.
13. `bz_quest_passthrough_create` / `bz_quest_passthrough_start` — see
    "Passthrough lifecycle" below.

Any failed step tears down everything already created via
`bz_quest_renderer_shutdown()` before returning `false` — there is no
partially-initialized state a caller could observe or accidentally use.

### Swapchain format selection

`bz_quest_renderer_init()` passes a preference-ordered list of `VkFormat`
values to `bz_quest_xr_create_swapchains()`:
`VK_FORMAT_R8G8B8A8_UNORM`, `VK_FORMAT_B8G8R8A8_UNORM`,
`VK_FORMAT_R8G8B8A8_SRGB`, `VK_FORMAT_B8G8R8A8_SRGB` — non-sRGB first
because the fragment shader (`tabletop_frag.frag`) writes vertex colors
straight through with no gamma-correction step; sRGB variants are accepted
as a fallback rather than failing outright if that's all the runtime
offers. `bz_quest_select_swapchain_format()` (in `bz_quest_pure.c`, host-
tested) is the actual selection logic, taking plain `int64_t` format values
so it needs no Vulkan/OpenXR headers to unit test.

### Frame loop (`bz_quest_renderer_frame()`, called once per
`ALooper_pollOnce` iteration from `bz_quest_host.c`)

1. `bz_quest_xr_poll_events` — drains the `XrEventDataBuffer` queue,
   handling exactly one `XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED` case per
   state (`READY` -> `xrBeginSession`, `STOPPING` -> `xrEndSession`,
   `EXITING`/`LOSS_PENDING` -> sets `exitRequested`) plus
   `XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING`. See "OpenXR session state
   handling" below for why each transition has exactly one action instead
   of being inferred from a boolean.
2. If `exitRequested`, return `false` (host tears down and exits).
3. If the session isn't running yet (before `xrBeginSession` succeeds, or
   after `xrEndSession`), return `true` without calling any frame function —
   `xrWaitFrame`/`xrBeginFrame`/`xrEndFrame` must only be called while a
   session is actually running, per the spec.
4. `bz_quest_xr_wait_frame` (`xrWaitFrame`) -> `bz_quest_xr_begin_frame`
   (`xrBeginFrame`) — always paired even when nothing will render, per the
   spec's frame-loop-in-lockstep requirement.
5. If `frameState.shouldRender` and `bz_quest_xr_locate_views` reports valid
   tracking (`XR_VIEW_STATE_POSITION_VALID_BIT` /
   `XR_VIEW_STATE_ORIENTATION_VALID_BIT` both set — see "View validity
   checking" below), for each eye: acquire/wait the swapchain image, build
   that eye's MVP matrix (`bz_quest_pose_to_view_matrix` +
   `bz_quest_fov_projection_vk` + `bz_quest_mat4_multiply`, all host-tested
   pure helpers), call `bz_quest_vk_render_target`, release the swapchain
   image, and fill an `XrCompositionLayerProjectionView`.
6. If step 5 produced valid per-eye data, assemble exactly two composition
   layers in back-to-front order (`XrFrameEndInfo.layers` order is
   back-to-front per the spec's "Layer Ordering" section): the passthrough
   layer (`bz_quest_passthrough_build_layer`,
   `XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB`) first as the background,
   then the `XrCompositionLayerProjection` (the tabletop scene) second as
   the foreground.
7. `bz_quest_xr_end_frame` (`xrEndFrame`) with either the 2-layer array or
   `layerCount=0` if nothing was rendered this frame (tracking invalid,
   `shouldRender` false, or a swapchain/render step failed).

### No busy loop / no per-frame logging

`bz_quest_host.c`'s `android_main` loop chooses `ALooper_pollOnce`'s
timeout per iteration via the host-tested pure helper
`bz_quest_looper_timeout_millis(wantsXrEventPolling, xrSessionRunning)`
(`bz_quest_pure.h`/`.c`): it returns `-1` (block indefinitely, zero CPU
cost) only when **both** are false, and `0` (poll without blocking)
whenever **either** is true. `xrSessionRunning` alone is deliberately not
the only input: `xrPollEvent` is only reachable from inside
`bz_quest_renderer_frame` (via `bz_quest_xr_poll_events`), itself only
called after `ALooper_pollOnce` returns, so an app that has just resumed
from the background but whose session hasn't reached `RUNNING` yet (the
`READY` event that would trigger `xrBeginSession` hasn't been polled yet)
would otherwise have `ALooper_pollOnce` block indefinitely waiting for an
Android input event that may never arrive — a permanent hang after every
resume with no user input, closed by also polling non-blocking whenever
`wantsXrEventPolling` (`androidResumed && rendererReady`, tracked in
`bzQuestAppState_t`) is true. Gating on `rendererReady` too (not
`androidResumed` alone) avoids a pure busy-loop with nothing to poll if
renderer init ever failed. No code path in the frame loop logs anything on
a healthy per-frame basis — every `BZ_QUEST_LOGI`/`BZ_QUEST_LOGE` call site
in `bz_quest_xr.c`/`bz_quest_vk.c`/`bz_quest_passthrough.c` is inside a
one-time init/teardown function or an error branch, never inside
`bz_quest_xr_wait_frame`/`bz_quest_xr_begin_frame`/`bz_quest_xr_end_frame`/
`bz_quest_vk_render_target`'s success paths.

### OpenXR session state handling

`bz_quest_xr_handle_session_state_changed()` is the single function that
interprets `XrEventDataSessionStateChanged`, with one unambiguous branch per
relevant state (not a generic "if running, do X" inference):

- `XR_SESSION_STATE_READY` -> `xrBeginSession` (view configuration:
  `XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO`), sets `sessionRunning = true`
  on success.
- `XR_SESSION_STATE_STOPPING` -> `xrEndSession`, sets
  `sessionRunning = false` regardless of that call's result (the session is
  stopping either way; the result is only logged).
- `XR_SESSION_STATE_EXITING` / `XR_SESSION_STATE_LOSS_PENDING` -> sets
  `exitRequested = true`.
- `XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING` (a separate event type, not a
  session-state value) -> also sets `exitRequested = true`.

`bz_quest_xr_destroy()` only tears down swapchains/space/session/instance in
that reverse-dependency order, and documents that it must not be called on
a session still in `XR_SESSION_STATE_STOPPING` that hasn't received
`xrEndSession` yet — callers must drain events to completion first (which
`bz_quest_renderer_frame()`'s `bz_quest_xr_poll_events()` call does on every
iteration before shutdown is ever reached).

### View validity checking

`XrView`s returned by `xrLocateViews` can report invalid position/
orientation (e.g. a momentary tracking loss) via `XrViewState.viewStateFlags`
— `bz_quest_xr_locate_views()` returns `false` unless **both**
`XR_VIEW_STATE_POSITION_VALID_BIT` and `XR_VIEW_STATE_ORIENTATION_VALID_BIT`
are set for every view, in which case `bz_quest_renderer_frame()` submits an
empty frame (`layerCount=0`) rather than rendering with a stale/garbage
pose.

## Vulkan device creation via `XR_KHR_vulkan_enable2` (`bz_quest_vk.c`)

Per the `XR_KHR_vulkan_enable2` pattern (confirmed against
`KhronosGroup/OpenXR-SDK-Source`'s `hello_xr` Vulkan graphics plugin and the
OpenXR 1.0 spec's "XR_KHR_vulkan_enable2" section,
<https://registry.khronos.org/OpenXR/specs/1.0/html/xrspec.html#XR_KHR_vulkan_enable2>,
verified 2026-07-31): the app supplies its own `VkInstanceCreateInfo`/
`VkDeviceCreateInfo` via `XrVulkanInstanceCreateInfoKHR`/
`XrVulkanDeviceCreateInfoKHR`, and the runtime's `xrCreateVulkanInstanceKHR`/
`xrCreateVulkanDeviceKHR` construct the actual `VkInstance`/`VkDevice`,
merging in whatever extensions/layers *it* needs on top of what the app
requested. **This app does not enumerate or request its own Vulkan
instance/device extensions** — the runtime supplies everything required for
OpenXR interop; requesting extensions here would only risk requesting
something the runtime doesn't also request identically, per the spec's
merge semantics.

`VkApplicationInfo::apiVersion` is derived from
`XrGraphicsRequirementsVulkanKHR::minApiVersionSupported` via
`bz_quest_xr_version_to_vk_api_version()` (a host-tested pure helper) — see
"Quirk: XrVersion and VkVersion do not share a bit layout" below.

Physical device selection: `xrGetVulkanGraphicsDevice2KHR` — the runtime
picks the physical device, not the app (a Quest has exactly one, but the
API contract doesn't assume that).

## Vulkan render pass/pipeline/targets (`bz_quest_vk.c`)

- **Depth format**: `bz_quest_vk_select_depth_format()` queries
  `vkGetPhysicalDeviceFormatProperties` against a preference-ordered
  candidate list (`VK_FORMAT_D32_SFLOAT`, `VK_FORMAT_D24_UNORM_S8_UINT`,
  `VK_FORMAT_D32_SFLOAT_S8_UINT`, `VK_FORMAT_D16_UNORM`), checking
  `VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT` for optimal tiling — see
  "Quirk: no single depth format is universally guaranteed" below.
- **Render pass**: one color attachment (the XR swapchain image,
  `finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL` — not a
  windowing-system `PRESENT_SRC` layout, since XR swapchain images are
  never presented via a normal Vulkan swapchain; this matches `hello_xr`'s
  Vulkan graphics plugin exactly) plus one depth attachment.
- **Pipeline**: `VK_CULL_MODE_NONE` — deliberate, because
  `bz_quest_scene.c`'s procedural quad/cube emitter does not guarantee
  consistent triangle winding per face; a correct two-sided baseline was
  prioritized over speculative back-face-cull optimization the generator
  doesn't actually support yet. Dynamic viewport/scissor (set per-target at
  record time, since each eye's swapchain can differ in size). Push-
  constant MVP matrix (one `mat4`, see `tabletop_vert.vert`).
- **Vertex buffer**: host-visible/coherent memory, written directly with no
  staging buffer/copy — justified because the test scene
  (`BZ_QUEST_SCENE_VERTEX_COUNT` vertices, see "Test scene" below) is a few
  hundred vertices, not a performance-sensitive path. A real asset-rendering
  layer must not copy this pattern without re-justifying it.
- **Multiview / fixed foveation / MSAA**: **not implemented.** Per this
  layer's explicit scope ("Prefer Vulkan multiview, MSAA, and fixed
  foveation only when the exact required runtime/device extensions and
  feature bits are proven... A correct two-eye baseline is more important
  than speculative optimization"), this renderer draws each eye with its
  own render pass/framebuffer/command buffer instead of
  `VK_KHR_multiview`, uses no MSAA attachment, and requests no
  `XR_FB_foveation`/fixed-foveation extension. This is an explicit,
  documented seam for the physical-device pass, not an oversight — flipping
  any of these on requires verifying the exact device feature bits/
  extension support on real Quest 3/3S hardware first, which this
  environment cannot do.
- **Per-frame synchronization**: see "Quirk: `XR_KHR_vulkan_enable2` has no
  semaphore/fence handoff to `xrEndFrame`" below.

## Shader build pipeline

Rather than package SPIR-V as Android assets (which would need
`AAssetManager` plumbing inside `bz_quest_vk.c`, contaminating the module
with Android-specific APIs) or commit opaque `.spv` binaries to the repo,
shaders are compiled from committed GLSL source
(`shaders/tabletop_vert.vert`/`tabletop_frag.frag`) at build time:

1. `scripts/build-shaders.sh` resolves the NDK's bundled `glslc`
   (`$ANDROID_NDK_HOME/shader-tools/<host-tag>/glslc`), compiles each
   `.vert`/`.frag` to SPIR-V (`--target-env=vulkan1.0`).
2. It builds `scripts/bin2c.c` (a tiny standalone host tool, `cc -std=c11`)
   and runs it on each `.spv`, which **repacks the file's raw bytes into
   native 32-bit words (explicitly little-endian, matching every host
   `glslc` build and Android arm64 - see the file's endianness comment),
   verifies the little-endian SPIR-V magic number (`0x07230203`) against
   the first word before embedding, and writes an aligned
   `static const uint32_t <name>[]` header** (`<name>.spv.h`, array
   `g_bz_quest_<name>_spv`). Emitting real `uint32_t` words - not an
   `unsigned char[]` later reinterpret-cast to `uint32_t *` - is
   deliberate: `VkShaderModuleCreateInfo::pCode` requires
   `alignof(uint32_t)` alignment, which only an array whose element type is
   actually `uint32_t` is guaranteed by the C standard to provide (see
   `bin2c.c`'s top comment and the Vulkan 1.3 spec's `vkCreateShaderModule`
   section).
3. It concatenates both into one umbrella `bz_quest_shaders_generated.h`
   that `bz_quest_vk.c` `#include`s directly.

`CMakeLists.txt` wires this as an `add_custom_command` (`OUTPUT` the
umbrella header, `DEPENDS` the script/`bin2c.c`/both GLSL sources) so every
`cmake --build`/`gradle assembleDebug` regenerates it from source — **no
SPIR-V binary or generated header is ever committed to the repo**, only the
`.vert`/`.frag` sources and the build tooling. This satisfies the layer's
requirement to keep shaders reproducible with a deterministic build path
and a verification mechanism, without an opaque committed binary. Verified
end-to-end on this machine: `ANDROID_NDK_HOME=... scripts/build-shaders.sh
/tmp/shader-out` and the full `make quest-assemble-debug` build both produce
a valid header (magic-number-checked SPIR-V for both stages).

## Passthrough lifecycle (`bz_quest_passthrough.c`)

### Passthrough is mandatory (two independent capability gates)

Per this layer's explicit Quest 3/3S MR scope, **lacking passthrough
capability is a hard startup failure, never a silent opaque-scene
fallback** — enforced twice, independently:

1. **System-level gate**: `bz_quest_xr_get_system()` reads
   `XrSystemPassthroughProperties2FB::capabilities` and hard-fails if
   `XR_PASSTHROUGH_CAPABILITY_BIT_FB` is absent, before any Vulkan/session
   work even begins.
2. **Object-level gate**: `bz_quest_passthrough_create()` independently
   re-checks the same capability bit (via the same host-tested
   `bz_quest_passthrough_capable()` helper) before calling
   `xrCreatePassthroughFB`, per this task's explicit requirement that the
   passthrough object itself checks capability rather than relying solely
   on the earlier system-level check.

`bz_quest_xr_select_blend_mode()` adds a third, related hard requirement:
`XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND` must be enumerated, or startup
fails — see the OpenXR spec's environment blend mode section,
<https://registry.khronos.org/OpenXR/specs/1.0/html/xrspec.html#environment-blend-mode>.

### Create/start/pause/destroy ordering

- **Create** (`bz_quest_passthrough_create`): `xrCreatePassthroughFB` with
  `flags=0` (**not** running at creation —
  `XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB` is deliberately not used,
  matching the spec's recommended explicit-start pattern), then
  `xrCreatePassthroughLayerFB` with
  `XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB` (full-environment
  reconstruction — the only purpose relevant to this prototype; no
  per-surface/projected passthrough is implemented).
- **Start** (`bz_quest_passthrough_start`): `xrPassthroughStartFB`, called
  once from `bz_quest_renderer_init()` right after create, and again from
  `bz_quest_host.c`'s `APP_CMD_RESUME` handler (guarded by `!pt->started` so
  a resume immediately following the initial start is a no-op, not a
  redundant `xrPassthroughStartFB` call).
- **Pause** (`bz_quest_passthrough_pause`): `xrPassthroughPauseFB`, called
  from `bz_quest_host.c`'s `APP_CMD_PAUSE` handler so the passthrough camera
  feed is released while the app isn't foregrounded — this is a real
  resource (camera access), not a free no-op, per Meta's own passthrough
  sample guidance.
- **Destroy** (`bz_quest_passthrough_destroy`): reverse order — pauses if
  still started, destroys the layer, then the passthrough object. Safe to
  call on a partially-initialized `bzQuestPassthrough_t` (every handle
  checked against `XR_NULL_HANDLE`).

### Composition layer assembly

`bz_quest_passthrough_build_layer()` fills an
`XrCompositionLayerPassthroughFB` (`type`, `space`, `layerHandle` — no
`flags` beyond the struct's zero-initialized defaults). It is placed first
(background) in `bz_quest_renderer_frame()`'s `layers[]` array, with the
`XrCompositionLayerProjection` (the tabletop scene) second (foreground),
since `XrFrameEndInfo.layers` is composited back-to-front.

**The projection layer's `layerFlags` must request source-alpha
compositing, or it fully occludes the passthrough layer beneath it.**
`tabletop_frag.frag` writes `vec4(fragColor, 1.0)` (opaque geometry) while
`bz_quest_vk_create_render_resources()`'s render-pass clear value clears
untouched background pixels to alpha `0.0` (see "Vulkan render
pass/pipeline/targets" above) — this only lets passthrough show through
those untouched pixels if the compositor is told to honor the layer's
alpha channel at all, and to interpret it as straight (non-premultiplied)
alpha, since that's the form the shader writes (color channels are never
scaled by alpha). `bz_quest_renderer.c` sets
`projectionLayer.layerFlags = bz_quest_projection_layer_flags(/*unpremultipliedAlpha=*/true)`
(`bz_quest_pure.h`/`.c`), which ORs in both
`XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT` (without it, the
runtime ignores the layer's alpha channel entirely and treats every texel
as fully opaque, regardless of what the shader wrote) and
`XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT` (without it, the runtime
assumes the source is already premultiplied and double-darkens any
partially-transparent texel — invisible for this scene's fully-opaque
geometry texels, but wrong for the alpha=0 background, which is exactly
where this bug hid). `bz_quest_renderer.c` also `_Static_assert`s the pure
module's mirrored flag-bit literals (needed there because
`bz_quest_pure.c` must stay host-buildable without `openxr.h`) against the
real `XR_COMPOSITION_LAYER_*_BIT` constants at compile time, and
`test_bz_quest_pure.c` has host-buildable regression tests asserting both
bits are set for the arguments the renderer actually passes. See the OpenXR
spec citation in "Documented quirks" below.

## Tabletop engine lifecycle bridge (`bz_quest_bridge.c`)

### Engine/renderer threading boundary

Exactly one dedicated engine thread runs the whole tabletop engine, and the
XR/Vulkan render loop never calls an engine frame function directly:

- `platform/tabletop/bridge/bz_tabletop_lifecycle.c`'s `BZ_TabletopStart()`
  spawns that one thread internally (unchanged from layers 1-3 —
  `bz_quest_bridge_start()` only calls `BZ_TabletopCreate()`/
  `BZ_TabletopStart()`, never touches `BZ_RuntimeFrame()`/`BZ_RuntimeInit()`
  itself). That thread is the only caller of `BZ_RuntimeFrame()` for the
  lifetime of the app.
- `android_main()`'s loop (the Android main/UI thread, which is also the
  thread every `bz_quest_xr_*`/`bz_quest_vk_*` OpenXR/Vulkan call already
  runs on — see "OpenXR session lifecycle" above) calls
  `bz_quest_snapshot_capture()` and `bz_quest_renderer_frame()` once per
  loop iteration. Neither of those functions, nor anything transitively
  reachable from `bz_quest_renderer.c`, ever calls `BZ_RuntimeFrame()`,
  `BZ_TabletopStart()`, or any other engine-thread-only entry point — the
  only cross-thread contact point is the lock-free, ref-counted immutable
  snapshot described in "Snapshot ownership and diagnostics" below.
- `bz_quest_bridge_start()`/`_suspend()`/`_resume()`/`_stop()`/`_destroy()`
  (this file) are the only functions `bz_quest_host.c` calls to affect the
  engine thread's lifecycle — all of them just forward to the existing
  `BZ_TabletopStart()`/`BZ_TabletopSuspend()`/`BZ_TabletopResume()`/
  `BZ_TabletopStop()`/`BZ_TabletopDestroy()` API (see
  `platform/tabletop/bridge/bz_tabletop_lifecycle.h`), which already
  implements the actual thread-safe start/join/suspend/resume machinery —
  this bridge adds no new threading of its own, only Quest-specific
  argv/data-dir construction and Android-lifecycle-to-lifecycle-call
  mapping.

### Lifecycle-transition mapping

`bz_quest_handle_cmd()` (in `bz_quest_host.c`) maps exactly these Android
`onAppCmd` events to bridge calls (each cited at its exact call site):

| Android event | Bridge call | Notes |
|---|---|---|
| `APP_CMD_START` | `bz_quest_ensure_bridge_start()` → `bz_quest_bridge_start()` (once per process) | Runs after `bz_quest_ensure_renderer_init()`; blocks the calling thread until `BZ_RuntimeInit()` completes, mirroring the renderer init call's own synchronous convention (see `bz_quest_ensure_bridge_start()`'s header comment in `bz_quest_host.c`). Independent of renderer init success — the engine runs headless even if OpenXR/Vulkan init failed. |
| `APP_CMD_RESUME` | `bz_quest_bridge_resume()` | Forwards to `BZ_TabletopResume()`; a no-op if the bridge never reached RUNNING/SUSPENDED. |
| `APP_CMD_PAUSE` | `bz_quest_bridge_suspend()` | Forwards to `BZ_TabletopSuspend()`; same no-op safety. |
| `APP_CMD_STOP` | *(none — log only)* | Android may still resume from `STOP` without a full `DESTROY`; stopping the engine here would make `APP_CMD_RESUME` unable to cheaply resume it. |
| `APP_CMD_DESTROY` | *(none — log only)* | See "Teardown ownership" below — `android_main()` alone owns the stop+destroy call, not this callback. |

Repeated start/stop or a "map reload" restart a fresh
`bzTabletopLifecycle_t`, they do not resume a terminal one:
`bz_quest_bridge_start()` is single-shot per `bzQuestBridge_t` instance
(rejects — logs and returns `false` — a second call on an already-attempted
instance, exactly like the underlying `BZ_TabletopStart()`'s own single-shot
contract); `bz_quest_bridge_destroy()` fully zeroes the struct so the same
storage can run `bz_quest_bridge_start()` again for a fresh attempt. See
`bz_quest_bridge.h`'s header comment for the full ownership rationale.

### Teardown ownership (which owner triggers final shutdown)

`android_main()` is the sole, deterministic owner of final shutdown —
**not** `bz_quest_handle_cmd()`'s `APP_CMD_DESTROY` case, which only logs.
This matters because there are four independent triggers that can end the
app, and all four must fall through the same one teardown code path instead
of each doing its own (potentially differently-ordered, potentially
double-run) cleanup:

1. Android requests destroy (`app->destroyRequested` becomes true, checked
   right after each `ALooper_pollOnce()` and again at the top of the loop
   condition).
2. `bz_quest_renderer_frame()` returns `false` (OpenXR session loss/exit
   pending, or a fatal Vulkan/XR error).
3. The engine thread self-transitions to a terminal state
   (`BZ_QUEST_BRIDGE_FAILED`/`BZ_QUEST_BRIDGE_STOPPED`, checked every loop
   iteration via `bz_quest_bridge_is_terminal()`) — e.g. a future
   frame-limit or console `quit` reaching `Sys_Quit()`. Nothing about the
   render path forces this; the engine can end itself independent of any
   Android/OpenXR event.
4. Renderer init never even attempted/succeeded in the first place (the
   loop still runs to pump Android events and the bridge, just with
   `state.rendererReady` false throughout).

Every one of these `break`s out of `android_main()`'s `while` loop into the
same code immediately below it:
`if (state.bridge.startAttempted) bz_quest_bridge_destroy(&state.bridge);`
runs **before** `bz_quest_renderer_shutdown()`. Bridge-before-renderer is a
deliberate ordering choice, not arbitrary: an already-exited/terminal engine
thread has nothing left for a render-thread frame-capture to observe, so
tearing down the "business logic" side first, then the presentation side,
keeps the ordering simple and independent of which trigger actually fired
(see `android_main()`'s teardown-block comment in `bz_quest_host.c`).

## Data-path contract (`bz_quest_data.c`)

### Default data directory

`bz_quest_data_default_dir()` builds `"<base>/Warcraft III"`, where `<base>`
prefers `ANativeActivity::externalDataPath` (adb-push-accessible without
root, survives an app update, mirrors `Context.getExternalFilesDir(null)`)
and falls back to `::internalDataPath` only if external storage is
unavailable — the NDK's own `ANativeActivity` documentation states
`externalDataPath` may be `NULL` (e.g. no shared storage volume mounted),
while `internalDataPath` is always non-`NULL` for a real NativeActivity app.
This mirrors `platform/apple/visionos/tabletop`'s `"Resources/Warcraft III"`
bundled-data naming without literally reusing visionOS's bundle-relative
path, since Quest has no app bundle to look inside.

### Override mechanism

A **single, fixed, documented** file path —
`"<internalDataPath>/warcraft_data_path_override.txt"`
(`BZ_QUEST_DATA_OVERRIDE_FILENAME` in `bz_quest_data.h`) — lets a developer
point at a non-default location (e.g. an external SD card path staged by
`adb push` before an install-only-once ADB data-copy script lands in a
later layer). This is **not** a silent secondary search path:

- `internalDataPath` is always non-`NULL`, so this fixed location never
  itself depends on the value it might override — no chicken-and-egg
  resolution order.
- `bz_quest_data_read_override_file()` returns `false` (no override
  requested — the normal, common case) only when the file does not exist
  at all. If it exists but is empty or unreadable, that is surfaced as an
  empty candidate string that the validation step below explicitly
  rejects — a permission error on a file a developer deliberately staged
  must never be silently treated as "no override requested".
- `bz_quest_data_validate_override()` requires the candidate (after
  trimming exactly one trailing `\n`/`\r\n`) to be non-empty, absolute
  (start with `/`), fit within `BZ_QUEST_DATA_DIR_MAX` (512) bytes, and
  contain none of `"`, `\r`, `\n`, `;` — **mirroring
  `bz_tabletop_lifecycle.c`'s own `BZ_TabletopSubmitMap()` path validation
  exactly**, since this value flows into the same `"-data"` argv slot that
  eventually reaches a `map "<path>"`-style console command internally
  (see `bz_quest_data.h`'s header comment for the full rationale).
- An invalid override (wrong shape, disallowed character, empty, oversized)
  is a **hard configuration error surfaced verbatim** to the caller
  (`bz_quest_bridge_start()` sets `bridge->preLcError` and returns `false`
  without ever calling `BZ_TabletopCreate()`), never a silent fall-back to
  the default the developer was explicitly trying to avoid.

`bz_quest_data_resolve()` is the single entry point `bz_quest_bridge_start()`
calls: read+validate the override if present, else the default dir. It does
**not** check the resolved directory actually exists or contains valid MPQ
archives — that check happens inside the engine's own `BZ_RuntimeInit()`
(`FS_AddDataDirectory()`), whose failure the bridge surfaces separately (see
"Missing/invalid data behavior" below) rather than being duplicated here.

### Engine startup argv construction

`bz_quest_data_build_argv()` builds
`["openwarcraft3-quest", "-data", "<dir>"[, "+map", "<name>"]]` — reusing
the exact `"-data"`/`"+map"` convention
`platform/apple/visionos/tabletop/app/OpenRealmTabletopApp.swift`'s
`LiveTabletopTransport` already establishes for this same lifecycle core
(see `common/bz_runtime.h`'s `bzRuntimeArgs_t` and AGENTS.md's "Command
Conventions": `+` is a process/startup argument here, never an in-engine
console command) — no second startup-argument scheme was invented for this
platform. `mapName` is always `NULL` in this layer (see "Current
limitations" below); the 3-argument form (`-data` only) is what actually
runs. The whole call uses caller-provided fixed-size storage — no heap
allocation.

### Missing/invalid data behavior

If the resolved directory does not exist or has no valid archives,
`BZ_RuntimeInit()` (via `FS_AddDataDirectory()`) fails and logs the exact
path (`"Failed to add data directory: <path>"`), and
`bz_tabletop_lifecycle.c` moves the lifecycle to its real
`BZ_TABLETOP_STATE_FAILED` state with a descriptive
`BZ_TabletopLastError()` — this bridge does not invent a second failure
mode. `bz_quest_bridge_state()` then reports `BZ_QUEST_BRIDGE_FAILED`, and
`bz_quest_bridge_is_terminal()` returns `true`, so `android_main()`'s loop
funnels this into the same one teardown path as every other exit trigger
(see "Teardown ownership" above) — Android/OpenXR teardown stays fully
responsive; the app does not hang or crash, it just runs with no engine
thread active (confirmed by `test_bz_quest_bridge.c`'s
`test_start_with_missing_data_reaches_failed` — see "Testing" above).

## Snapshot ownership and diagnostics (`bz_quest_frame.c`/`bz_quest_snapshot.c`)

### Acquire/copy/release contract

`bz_quest_snapshot_capture()` (the one real caller of
`BZ_TT_Latest()`/`BZ_TTSnapshot_*()`/`BZ_TTSnapshot_Release()`, kept in its
own translation unit precisely so `bz_quest_frame.c` stays link-clean of
`platform/bridge/bz_tabletop_transport.c`'s heavy engine dependencies) is
called exactly once per `android_main()` loop iteration, on the XR/render
thread:

1. Acquires the latest immutable snapshot via `BZ_TT_Latest()` (may return
   `NULL` if nothing has been published yet — not an error).
2. If non-`NULL`, copies only the small diagnostic values
   `bz_quest_frame_from_values()` needs (ABI version, generation,
   connection state, map name/bounds, player number/team, selected/total/
   overflow entity counts, fog dimensions, config-string count, action-
   layout presence) into a plain `bzQuestFrameValues_t` on the stack.
3. Releases the snapshot via `BZ_TTSnapshot_Release()` — **on every
   branch**, including "no snapshot published yet" (nothing to release)
   and an ABI-mismatched snapshot (still released after being read, just
   flagged `BZ_QUEST_FRAME_ABI_MISMATCH` by `bz_quest_frame_from_values()`
   instead of `BZ_QUEST_FRAME_OK`). No engine pointer/handle from the
   snapshot ever outlives this one function call — see
   `bz_quest_snapshot.h`'s header comment and
   `platform/bridge/bz_tabletop_transport.h`'s retain/release contract.

This call site is pinned to the render thread deliberately (not merely
"wherever is safe" — `BZ_TT_Latest()` itself is thread-safe from any
thread): a future input/gameplay layer will need to read the *same*
generation's entity/selection data the renderer just drew, so both must
observe one consistent snapshot from a single call site rather than two
independently-timed `BZ_TT_Latest()` calls that could race a generation
change between them.

### Frame descriptor fields tracked

`bzQuestFrame_t` (see `bz_quest_frame.h`) tracks, in one plain-C struct:
snapshot generation/readiness (`status`: `NO_SNAPSHOT`/`ABI_MISMATCH`/`OK`,
`generation`), player/"camera" state (`playerValid`/`playerNumber`/
`playerTeam` — WC3's tabletop ABI has no separate camera struct; the XR
head pose is tracked independently by `bz_quest_xr.c`, out of scope for
this descriptor, so "camera state" per this layer's task scope means these
player fields), entity count (`entityCount`/`entitiesOverflowCount`/
`selectedEntityCount`), terrain/fog/selection/configstring availability
(`mapBoundsValid`, `fogPresent`/`fogWidth`/`fogHeight`,
`configStringCount`, `actionLayoutPresent`), and lifecycle errors
(`lifecycleState`, `lifecycleError`). **No ABI widening was needed or
performed** — `bzTTSnapshot_t` v3 already exposes every field this layer
requires (see `bz_quest_frame.h`'s comment on `bzQuestFrameValues_t`); this
layer consumes v3 as-is.

### Diagnostics: throttled log, never per-frame

`bz_quest_frame_should_log()` (pure, host-tested, no I/O) is the single gate
`android_main()` checks before emitting the `"tabletop frame: ..."` log
line — it returns `true` only when something actually changed since the
last capture: the snapshot `status` changed (first-ever capture, or an ABI
mismatch newly appearing/clearing), the `lifecycleState` changed, or the
`lifecycleError` text appeared/changed/cleared. It deliberately does **not**
return `true` merely because `generation` advanced, even while
`status == OK`: `BZ_TT_PublishSnapshotFromClient()` bumps `generation` on
every engine frame (tens of times per second — see
`platform/bridge/bz_tabletop_transport.c` and
`platform/tabletop/client/cl_scrn_tabletop_null.c`'s `SCR_UpdateScreen()`),
so treating a bare generation advance as a log trigger reproduces exactly
the per-frame logging AGENTS.md forbids the moment a bridge starts
successfully — this was a real defect caught in PR #19's review pass and
fixed by dropping the generation-advance trigger entirely (see
`test_should_log_generation_only_advance_never_logs_when_ok()` in
`tests/test_bz_quest_frame.c`, which simulates ~190 consecutive engine
frames of bare generation churn and asserts none of them log). The
snapshot's first-ever `OK` transition already proves generation is
advancing (there would be no `OK` status without a real, non-`NULL`
snapshot), and `generation` remains available in `bzQuestFrame_t` for
anyone reading the descriptor directly — only the automatic log trigger
excludes it. So a healthy, steady-state loop (map-less, generation
advancing every client frame, exactly as this layer expects — see
"Known limitations of this frame descriptor" below) produces **zero**
additional log lines after the first capture, matching AGENTS.md's "never
log per frame; log transitions/milestones only" convention and this
document's own "No busy loop / no per-frame logging" policy above.

### Integration into the test tabletop

This diagnostic frame descriptor (`bzQuestFrame_t`) itself is still never
translated into drawable geometry — `bz_quest_scene.c`'s procedural
checkerboard-and-cubes test scene (see "Test scene" below) is drawn
completely independently of it. Instead, `android_main()`'s throttled log
line (`"tabletop frame: status=... generation=... lifecycleState=... ..."`,
cited exactly at the log call site in `bz_quest_host.c`) remains the
diagnostic proof that snapshots advance while the engine thread is running.
Layer 5A (see
"[Layer 5A: static Warcraft III model rendering](#layer-5a-static-warcraft-iii-model-rendering-bz_quest_wc3_c-bz_quest_vk_wc3c)"
below) *does* now translate live snapshot entity data into drawable
Warcraft III model geometry — but via its own independent
`bz_quest_wc3_capture_frame()` call path (a separate `BZ_TT_Latest()`
acquire/release, not a read of this `bzQuestFrame_t` descriptor), and only
for static (non-animated) model geometry/materials, never terrain or any
`bzQuestFrame_t` field itself.

### Known limitations of this frame descriptor

- No map is ever loaded in this layer (`bz_quest_data_build_argv()` is
  always called with `mapName == NULL`), so `mapLoaded`/`mapBoundsValid`/
  `playerValid`/`fogPresent` will all read `false` and `entityCount` will
  read `0` in every real run — this is expected, not a bug; the tabletop
  transport still publishes an advancing-generation snapshot every client
  frame regardless of whether a map is loaded (see
  `platform/tabletop/client/cl_scrn_tabletop_null.c`'s
  `SCR_UpdateScreen()`), which is exactly what the throttled log proves.
- This descriptor's own fields are never drawn from directly — see
  "Integration into the test tabletop" above for how layer 5A's Warcraft
  rendering instead uses its own, separate capture path.

## Test scene (`bz_quest_scene.c`)

Purely procedural, no asset files: an `BZ_QUEST_SCENE_TABLE_TILES` x
`BZ_QUEST_SCENE_TABLE_TILES` (8x8) checkerboard table quad (2 alternating
colors) plus `BZ_QUEST_SCENE_CUBE_COUNT` (4) small colored proxy cubes
sitting on top, at different positions — `BZ_QUEST_SCENE_VERTEX_COUNT`
vertices total (table + 4 x 6-face x 2-triangle cubes), unindexed triangle
list. `bz_quest_vk_create_render_resources()` calls
`bz_quest_scene_generate(verts, 1.6f, -0.5f, 1.0f)`: a 1.6m-square table,
0.5m below the `LOCAL` reference space origin (roughly waist height below a
standing headset), 1m in front along -Z. This exists specifically to prove
per-eye stereo transforms, depth testing/occlusion, clipping, and real-world
scale on physical hardware — it is explicitly **not** Warcraft III asset
rendering (out of scope for this layer; see
[Current limitations](#current-limitations)).

## Layer 5A: static Warcraft III model rendering (`bz_quest_wc3_*.c`/`bz_quest_vk_wc3.c`)

Layer 5A's job: render **static** (non-animated) Warcraft III MDX model
geometry and materials at their authoritative snapshot transforms, on the
Quest XR/render thread, with correctly-owned Vulkan GPU resources. Explicitly
out of scope for this slice (each is a later layer's job, not an oversight):
terrain, skeletal/sequence animation, fog of war, selection decals,
particles/effects, command-card/HUD surfaces, gameplay input, audio, and
data staging.

### Module map and ownership

| File | Kind | Owns |
|------|------|------|
| [`bz_quest_wc3_render.h`/`.c`](../platform/android/quest/app/src/main/cpp/bz_quest_wc3_render.c) | pure, host-testable | Coordinate/scale math (engine space → target Y-up right-handed), render-item/render-list POD structs, render-list construction. Never touches an ABI handle or a Vulkan type. |
| [`bz_quest_wc3_cache.h`/`.c`](../platform/android/quest/app/src/main/cpp/bz_quest_wc3_cache.c) | pure, host-testable | Generic, fakeable identity-keyed cache bookkeeping (acquire/hit/miss/evict/shutdown) with injected create/destroy callbacks - no Vulkan/ABI dependency itself; instantiated twice by `bz_quest_vk_wc3.c` (models, textures). |
| [`bz_quest_wc3_capture.h`/`.c`](../platform/android/quest/app/src/main/cpp/bz_quest_wc3_capture.c) | impure, ABI-calling | The one translation unit that calls `BZ_TT_Latest()`/`BZ_TTSnapshot_EntityAt()`/`BZ_TTA_RegisterConfigString()`/`BZ_TTA_ResolveEntityMetadata()`/`BZ_TTA_RegisterModelTexture()`/`BZ_TTAsset_*()`. Retains/copies/releases every snapshot and asset handle on every branch - no bridge pointer survives past one `bz_quest_wc3_capture_frame()` call. Like `bz_quest_snapshot.c`, has no direct unit test (see "Testing" below); reviewed by inspection. |
| [`bz_quest_vk_wc3.h`/`.c`](../platform/android/quest/app/src/main/cpp/bz_quest_vk_wc3.c) | impure, Vulkan-calling | Owns its own descriptor set layout/pool/sampler/pipeline layout/shaders/staging buffer/upload command buffer/two GPU caches, entirely separate from `bz_quest_vk.h`'s procedural-scene pipeline/vertex buffer. Runs uploads and draws, both synchronously on the calling (Quest XR/render) thread. |
| [`shaders/warcraft_vert.vert`/`warcraft_frag.frag`](../platform/android/quest/app/src/main/cpp/shaders/warcraft_vert.vert) | GLSL, compiled by `build-shaders.sh` | Unlit textured geoset draw - see "Shader/pipeline" below. |

`bz_quest_renderer.c` wires it in: `bz_quest_vk_wc3_create()` runs once after
`bz_quest_vk_create_render_resources()` in `bz_quest_renderer_init()`;
`bz_quest_vk_wc3_capture_and_upload()` runs once per frame (not once per eye)
before the per-eye loop; each eye then calls
`bz_quest_vk_wc3_render_target()` when the captured render list is non-empty,
or falls back to the existing `bz_quest_vk_render_target()` procedural
diagnostic scene when it is empty (see "Diagnostic-scene fallback is
explicit, not silent" below); `bz_quest_vk_wc3_destroy()` runs in
`bz_quest_renderer_shutdown()` before `bz_quest_vk_destroy()`.

### Retain/copy/release discipline

`bz_quest_wc3_capture_frame()` mirrors `bz_quest_snapshot.c`'s discipline
exactly: it performs its own independent `BZ_TT_Latest()`/release pair
(not shared with `bz_quest_snapshot_capture()` - a one-generation skew
between the two is immaterial, both still observe a self-consistent
snapshot), walks entities, and for each one resolves category/footprint via
`BZ_TTA_ResolveEntityMetadata()` and its model config string via
`BZ_TTA_RegisterConfigString()`. Every retained model/texture asset handle
returned by `BZ_TTA_RegisterModelTexture()` is released
(`BZ_TTAsset_Release()`) on every branch — success, malformed data, or
overflow — before the function returns; the snapshot itself is released
last. Only plain-POD copies (the `bzQuestWc3EntityInput_t`/`bzQuestWc3Model_t`
structs declared in `bz_quest_wc3_render.h`) ever leave this function; no
bridge pointer/handle survives past one call, matching the task's
"no borrowed bridge pointer may survive the documented retain scope"
requirement.

### Coordinate/transform evidence (do not change without re-deriving)

Full citations live in `bz_quest_wc3_render.h`'s header comment; summarized
here:

- **Axis convention**: Warcraft III/MDX is Z-up (evidence:
  `games/warcraft-3/renderer/mdx/r_mdx_render.c:43` uses `{0,0,1}` as the
  billboard "up" vector). OpenXR/Vulkan (like the reviewed visionOS/
  RealityKit target) is Y-up right-handed. This module replicates the
  already-shipped, reviewed visionOS renderer's exact conversion rather than
  re-deriving new axis/winding rules:
  - Position: `(x, y, z)_engine -> (x, z, y)_target` -
    `LiveTabletopTransport.swift:56`.
  - Vertex/normal: `(x, y, z) -> (x, z, y)` -
    `WarcraftAssetAdapter.swift:379-380`.
  - Heading: negated, then applied as a rotation about the target Y axis -
    `TabletopAdapter.swift:54`, `RealityTabletopView.swift:268`.
  - Winding: indices reordered `[i0, i2, i1]` after the axis swap to restore
    correct front-face winding (the swap alone flips handedness) -
    `WarcraftAssetAdapter.swift:384-389`.
- **Scale**: deliberately not a raw MDX-unit passthrough. Two stages, both
  replicated exactly from the reviewed renderer's `.world` coordinate space:
  a per-category multiplier applied to `max(footprint, 0.25)` **independently
  per axis** — footprintX/width feeds X, footprintY/depth feeds Z, never a
  shared `max(footprintX, footprintY)` across both (a rectangular footprint
  must stay rectangular) — (`WarcraftRenderDescriptors.swift:375-391`,
  `LiveTabletopTransport.swift:686`), then a further world-space scale-down
  `(min(x,2)*0.06, y*0.08, min(z,2)*0.06)`
  (`WarcraftRenderMath.swift:498-514`). `bzTTEntity_t.scale` (the runtime
  unit-scale field, e.g. `SetUnitScale`) is never folded in — grepped, no
  reference to `entity.metadata.scale` in either reviewed Swift file's
  world-transform code, so this slice replicates that omission rather than
  guessing a use for it.
- `world = T(swapped origin) * R(-angle around Y) * S(category/footprint
  scale)`, column-major, matching `bz_quest_pure.h`'s
  `bz_quest_mat4_multiply()` layout.

### Vulkan GPU caches: keys, lifetime, hit/miss, eviction

Two `bzQuestWc3Cache_t` instances (from the shared, host-tested
`bz_quest_wc3_cache.h`), owned by `bzQuestVkWc3_t`:

- **Model cache** — key: model identity string (the MDX/config-string
  identity `BZ_TTAsset_Identity()` returns, stable per task-space asset).
  Value: `bzQuestVkWc3Model_t` (device-local vertex+index `VkBuffer`s plus
  `bzQuestWc3ModelMeta_t`'s geoset/layer metadata).
- **Texture cache** — key: texture identity string (same ABI identity
  contract, replaceable_id-0/direct textures only this slice — see
  "Supported vs. unsupported material behavior" below). Value:
  `bzQuestVkWc3Texture_t` (`VkImage`/`VkImageView`/`VkDescriptorSet`).
- Both caches are fixed-capacity (`BZ_QUEST_WC3_CACHE_CAPACITY` = 128, the
  same constant for both), FIFO-evicting the oldest entry once full (not an
  unbounded allocation) - see `bz_quest_wc3_cache.h`'s contract, host-tested
  by `test_acquire_evicts_oldest_entry_at_capacity`.
- **Create-before-evict headroom**: `bz_quest_wc3_cache_acquire()` calls the
  create-callback BEFORE evicting the oldest entry on a miss at capacity (by
  design — a failed create must never destroy a still-good cached entry).
  This means the *real* backing resource pool for a cache's create-callback
  must have room for `capacity + 1` simultaneously-live resources, not just
  `capacity` — otherwise the create-before-evict order deadlocks permanently
  once the pool is full (every subsequent distinct key repeats the identical
  allocation failure, since eviction never runs). The texture cache's Vulkan
  descriptor pool is therefore sized to
  `BZ_QUEST_VK_WC3_TEXTURE_DESCRIPTOR_POOL_CAPACITY` (=
  `BZ_QUEST_WC3_CACHE_CAPACITY + 1`, not the bare capacity) — see
  `bz_quest_vk_wc3.h`'s comment at that constant. Structurally verified by
  `test-wc3-descriptor-pool-headroom.sh` (checked into
  `test-quest-wc3-descriptor-pool-headroom`/`quest` Make targets) and proven
  at the pure-cache-logic level by
  `test_acquire_deadlocks_when_ceiling_matches_capacity_with_no_spare_slot`/
  `test_acquire_recovers_when_ceiling_has_one_spare_slot` in
  `test_bz_quest_wc3_cache.c`.
- **Hit path**: the render path (`bz_quest_vk_wc3_render_target()`) never
  triggers an upload — it uses a read-only `cache_find()` linear scan over
  the cache's public `slots[]` array; an item whose model/texture isn't yet
  resident is simply skipped for that frame (transient, not an error, not
  logged), matching this slice's "no placeholder-geometry" contract.
- **Miss path**: only `bz_quest_vk_wc3_capture_and_upload()`'s two
  callbacks (`model_ready_cb`/`texture_ready_cb`) may call
  `bz_quest_wc3_cache_acquire()`, which runs the real Vulkan
  create-callback (staged upload) on a miss.
- **Shutdown**: `bz_quest_vk_wc3_destroy()` calls `vkDeviceWaitIdle()` then
  shuts down both caches (destroying every still-cached model/texture via
  their injected destroy callbacks - host-tested by
  `test_shutdown_destroys_every_entry`), then destroys the staging
  buffer/sampler/descriptor pool+layout/pipeline layout/shaders/pipeline
  variants — safe on a partially-initialized `vk3` (every handle checked
  against `VK_NULL_HANDLE`, matching `bz_quest_vk_destroy()`'s own
  contract).

### Bounded, thread-pinned uploads

All Vulkan calls in this slice happen on the Quest XR/render thread only —
`bz_quest_wc3_capture.c` never touches Vulkan, and `bz_quest_vk_wc3.c` never
touches the tabletop asset ABI. Each model/texture upload uses one shared,
fixed 16MB host-visible/coherent staging `VkBuffer` and one pre-allocated,
reused `uploadCommandBuffer`: map staging memory, `memcpy`, submit a
one-time-submit command buffer, `vkQueueWaitIdle()` before returning — no
hidden worker thread, no async multi-frame-in-flight upload queue. Per-frame
new-upload work is explicitly bounded
(`BZ_QUEST_VK_WC3_MAX_NEW_MODEL_UPLOADS_PER_FRAME` = 4,
`BZ_QUEST_VK_WC3_MAX_NEW_TEXTURE_UPLOADS_PER_FRAME` = 8): a frame that
suddenly touches many never-before-seen assets (e.g. right after a map load)
cannot stall the render thread for an unbounded number of staging
round-trips — the remainder is simply captured and uploaded again on later
frames (that entity is skipped that frame only, never drawn corrupted).
Texture pixel uploads repack each row into a tightly-packed destination
buffer (BLP row padding vs. `vkCmdCopyBufferToImage`'s
`bufferRowLength=0` "tightly packed" default) rather than a single flat
`memcpy`.

### Shader/pipeline

`warcraft_vert.vert`/`warcraft_frag.frag` are deliberately **unlit** —
matching `tabletop_frag.frag`'s own established unlit-passthrough
convention for this Quest renderer, not an oversight. An earlier draft
computed a world-space normal via `mat3(mvp)` for lighting, which is
incorrect (`mvp` = `viewProj * world` includes the projection matrix, not a
valid normal transform space); rather than add a second world-only matrix
push constant (which would exceed Vulkan's guaranteed-minimum 128-byte
`maxPushConstantsSize` alongside the existing 64-byte `mvp` + 16-byte
material params), lighting was dropped entirely for this slice rather than
guessed. Push constants: `{VERTEX, offset 0, size 64}` = `mat4 mvp`;
`{FRAGMENT, offset 64, size 16}` = `vec4 materialParams` (`x` = layer alpha,
`y` = alpha-discard cutoff). `MODEL_GEO_UNSHADED` (0x1) is not distinguished
— no lighting model exists at all this slice, so there is nothing to
disable per-layer.

Blend/cull/depth mapping is derived from the desktop OpenGL MDX renderer
(`games/warcraft-3/renderer/mdx/r_mdx_geoset.c`'s `MDLX_SetBlendMode()`/
`MDLX_ApplyLayerFlags()`/`MDLX_IsBlendedLayer()`), mirrored (not
`#include`d — `bz_tabletop_assets.h`'s `bzTTBlendMode_t` values are
locally-mirrored `BZ_QUEST_TTA_BLEND_*` constants, cross-checked by a
`_Static_assert` in `bz_quest_wc3_capture.c`, the one file in this slice
that legitimately includes that header):

| `bzTTBlendMode_t` | Blend | Depth write default |
|---|---|---|
| `OPAQUE` (0) | none | on |
| `TRANSPARENT` (1) | none, alpha-discard < 0.5 (`renderer/r_shader.c:308`'s `uAlphaCutoff`) | on |
| `ALPHA` (2) | src=SRC_ALPHA, dst=ONE_MINUS_SRC_ALPHA | off |
| `ADDITIVE` (3) | src=ONE, dst=ONE | off |
| `ADD_ALPHA` (4) | src=SRC_ALPHA, dst=ONE | off |
| `MODULATE` (5) | src=DST_COLOR, dst=ZERO | off |
| `MODULATE_2X` (6) | src=DST_COLOR, dst=SRC_COLOR | off |

`MODEL_GEO_TWOSIDED` (0x10, `r_mdx.h:31`) → cull none, else back-cull.
`MODEL_GEO_NO_DEPTH_TEST` (0x40) → depth test off.
`MODEL_GEO_NO_DEPTH_SET` (0x80) → forces depth write off (only ever an
override toward *off*, never forces on over a blend mode's own off
default). Pipeline variants are lazily created and cached (keyed by blend
mode × two-sided × depth-test × depth-write), capped at
`BZ_QUEST_VK_WC3_MAX_PIPELINE_VARIANTS` (56 = 7 blend modes × 2 × 2 × 2); a
genuinely-exhausted cap logs once and reuses the opaque/back-cull/depth-on
variant rather than failing to draw.

### Supported vs. unsupported material behavior

- **Supported**: `replaceable_id` 0 (direct/non-team texture), 1 (team
  color), and 2 (team glow, layer 5C added 1/2 — see "Layer 5C: Warcraft
  III model animation" above), `BZ_TTA_PIXEL_RGBA8` format,
  `BZ_TTA_ORIGIN_TOP_LEFT` origin, up to `BZ_QUEST_WC3_MAX_TEXTURE_DIM`
  (2048px) per axis / `BZ_QUEST_WC3_MAX_TEXTURE_BYTES` (16MB) total.
- **Explicitly unsupported this slice** (each logged once per unique
  identity/detail via `bz_quest_wc3_capture.c`'s `LOG_ONCE`, the affected
  layer/geoset skipped — never silently demoted to another mode or
  substituted with a different texture):
  - Any `replaceable_id` outside `{0,1,2}` (a per-entity image override) —
    deferred to a later layer (`bz_quest_wc3_capture.h`'s "texture
    decode policy" comment).
  - Any texture in a pixel format other than RGBA8, or with an origin other
    than top-left.
  - A texture exceeding this slice's staging-buffer size/dimension caps.
  - A geoset whose vertex/index count exceeds this slice's per-model scratch
    capacity (`BZ_QUEST_WC3_MAX_VERTS_PER_MODEL`/`_INDICES_PER_MODEL`).
  - Missing `MaterialInfo`/`MaterialLayerInfo`/`ModelTextureInfo` for a
    geoset/layer.

### Draw ordering / transparency

Two-pass draw per eye: (1) all opaque/alpha-tested layers (`blendMode < 2`)
across every render-list item, arbitrary order (depth test/write already
guarantees correctness); (2) all blended layers (`blendMode >= 2` -
`MDLX_IsBlendedLayer()`'s exact threshold) collected into a scratch array
and sorted back-to-front by squared distance from the eye's tracked
world-space position to each render item's world-matrix translation
(`qsort`), then drawn in that order. **This sort is per-entity, not
per-triangle** — a documented, honest granularity limit (not full
per-triangle transparency sorting), consistent with this slice's
"favor correct ownership and draw ordering over speculative optimization"
priority. Batching/instancing: this slice does not batch at the
render-list-construction level (`bz_quest_wc3_build_render_list()` always
emits one `bzQuestWc3RenderItem_t` per entity); repeated models are instead
naturally deduplicated at the GPU level by the model/texture cache key (a
second entity referencing an already-uploaded model reuses the same
`VkBuffer`/`VkImage`, only a new per-instance push-constant draw call is
issued), which is the correctness-preserving form of reuse this slice
implements.

### Diagnostic-scene fallback is explicit, not silent

When the captured render list is empty (no snapshot yet, still connecting,
or no visible entity's model/texture has finished uploading), each eye
renders the existing procedural checkerboard-and-cubes test scene (see
"Test scene" above) — **not** a silent "asset unsupported" fallback, but the
same explicit, pre-existing diagnostic scene layer 3/4 already used for
"nothing valid to render yet". Procedural-scene and Warcraft-model Vulkan
resources are entirely separately owned (`bz_quest_vk.h` vs.
`bz_quest_vk_wc3.h`, two disjoint pipelines/vertex buffers/descriptor sets)
so a missing/malformed Warcraft asset can never corrupt the diagnostic
renderer, and vice versa.

## Documented quirks (with sources)

- **`XrVersion` and `VkVersion` do not share a bit layout.** `XrVersion` is
  a `uint64_t` with major/minor/patch in bits 63-48/47-32/31-0;
  `VkVersion`/`VK_MAKE_API_VERSION` packs variant/major/minor/patch into a
  32-bit value with major in bits 22-29, minor in bits 12-21. Converting one
  to the other by reinterpreting bits (or naively shifting) is wrong.
  `bz_quest_xr_version_to_vk_api_version()` (in `bz_quest_pure.c`, host-
  tested with both a normal case and a patch-truncation case) does the
  correct field-by-field extract-and-repack. Confirmed from
  `openxr.h`'s `XR_VERSION_MAJOR`/`MINOR`/`PATCH` macros and
  `vulkan_core.h`'s `VK_API_VERSION_MAJOR`/`MINOR`/`PATCH`/`VK_MAKE_API_VERSION`
  macros directly (both headers extracted and inspected on this machine,
  2026-07-31).
- **`XR_KHR_vulkan_enable2` has no semaphore/fence handoff to
  `xrEndFrame`.** Unlike a normal Vulkan swapchain (where a semaphore
  signals when rendering is done and `vkQueuePresentKHR` waits on it), the
  OpenXR spec's Vulkan graphics binding requires only that the app's
  rendering to the color image **complete** before `xrEndFrame` uses it —
  there is no OpenXR-side semaphore parameter to synchronize against.
  `bz_quest_vk_render_target()` handles this by blocking on the just-
  submitted fence (`vkWaitForFences`) before returning, which is correct
  but leaves no CPU/GPU overlap between eyes — an explicit, documented
  performance-vs-correctness tradeoff (see the function's comment) that a
  physical-device profiling pass should revisit (e.g. render both eyes'
  command buffers before waiting on either fence). Confirmed against the
  OpenXR 1.0 spec's Vulkan graphics binding section,
  <https://registry.khronos.org/OpenXR/specs/1.0/html/xrspec.html#XR_KHR_vulkan_enable2>,
  and `hello_xr`'s Vulkan graphics plugin, which does the same
  wait-before-return.
- **No single Vulkan depth format is universally guaranteed.** The Vulkan
  spec's "Required Format Support" table
  (<https://registry.khronos.org/vulkan/specs/1.3/html/vkspec.html#features-required-format-support>)
  only guarantees *at least one* of `{D32_SFLOAT, X8_D24_UNORM_PACK32}` and
  *at least one* of `{D24_UNORM_S8_UINT, D32_SFLOAT_S8_UINT}` support
  `VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT` for optimal tiling — not
  any single specific format. `bz_quest_vk_select_depth_format()` queries
  capability at runtime instead of assuming one.
  `VK_FORMAT_D16_UNORM` is included as this renderer's final fallback
  candidate (guaranteed depth-only format on essentially all
  implementations), after the two commonly-preferred combined/higher-
  precision formats.
- **Composition layer order is back-to-front.** OpenXR 1.0 spec, "Layer
  Ordering" section,
  <https://registry.khronos.org/OpenXR/specs/1.0/html/xrspec.html#compositing-layer-ordering>:
  `XrFrameEndInfo.layers[0]` is drawn first (furthest back), so passthrough
  must be listed before the projection layer, not after.
- **A composition layer's alpha channel is ignored unless
  `XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT` is set in
  `layerFlags`.** Without it, the runtime treats the layer as fully opaque
  regardless of what the shader wrote to alpha — this is what let an
  earlier draft's `XrCompositionLayerProjection` (`layerFlags` left at its
  zero-initialized default) fully occlude `XR_FB_passthrough`, even though
  the render pass correctly cleared background pixels to alpha `0.0`. A
  second, independent bit,
  `XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT`, must also be set
  whenever (as here) the source color is straight/non-premultiplied alpha,
  or the runtime composites as though it were already premultiplied,
  double-darkening partially-transparent texels. Confirmed against the
  OpenXR 1.1 spec's `XrCompositionLayerFlags`/`XrCompositionLayerBaseHeader`
  description,
  <https://registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html#XrCompositionLayerFlags>,
  and the exact bit values (`0x2`/`0x4`) cross-checked against the
  extracted `org.khronos.openxr:openxr_loader_for_android:1.1.49` `openxr.h`
  (`XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT` /
  `XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT`). See "Composition layer
  assembly" above and `bz_quest_pure.h`'s
  `bz_quest_projection_layer_flags()` comment.
- **`xrLocateViews` can report invalid tracking even mid-session.**
  `XrViewState.viewStateFlags` must be checked
  (`XR_VIEW_STATE_POSITION_VALID_BIT`/`ORIENTATION_VALID_BIT`) on every call
  — a momentary tracking loss does not change `XrSessionState`, so relying
  on session state alone to decide whether to render is insufficient. OpenXR
  1.0 spec, `xrLocateViews`/`XrViewState` reference pages.
- **`XR_TIMEOUT_EXPIRED` from `xrWaitSwapchainImage` is a qualified
  *success*, not a failure, and must not be treated as license to release or
  abandon the image.** `XR_TIMEOUT_EXPIRED = 1` and
  `XR_SUCCEEDED(result)` is `(result) >= 0`, so it is `XR_SUCCEEDED` despite
  the name. The OpenXR spec's Rendering chapter, "Swapchain Image
  Management" → `xrWaitSwapchainImage`
  (<https://github.com/KhronosGroup/OpenXR-Docs/blob/main/specification/sources/chapters/rendering.adoc>,
  mirrored at
  <https://registry.khronos.org/OpenXR/specs/1.0/html/xrspec.html#xrWaitSwapchainImage>)
  states: "If `xrWaitSwapchainImage` returns `XR_TIMEOUT_EXPIRED`, the next
  call to `xrWaitSwapchainImage` will wait on the same image index again
  until the function succeeds with `XR_SUCCESS`" and separately that
  `xrReleaseSwapchainImage` requires the image to have "been successfully
  waited on **without timeout** before it is released." `bz_quest_xr.c`'s
  `bz_quest_xr_wait_swapchain_image()` always requests `XR_INFINITE_DURATION`
  (so a conformant runtime should not return `XR_TIMEOUT_EXPIRED` there in
  practice — there is no requested timeout to expire against), but
  defensively retries (bounded by `BZ_QUEST_SWAPCHAIN_WAIT_MAX_RETRIES`,
  currently 64) rather than treating it as fatal, and only returns `false`
  on a genuine `XR_FAILED` result (a negative `XrResult`), which is the only
  case where the caller (`bz_quest_renderer.c`'s frame loop) correctly skips
  the subsequent `bz_quest_xr_release_swapchain_image()` call for that
  image. Confirmed against `hello_xr`'s reference implementation
  (`src/tests/hello_xr/check.h`'s `CheckXrResult`), which uses
  `XR_FAILED(res)` (negative-only) rather than `res != XR_SUCCESS` to decide
  whether an `XrResult` is fatal, for the same reason.
- **`VkShaderModuleCreateInfo::pCode` requires `uint32_t` alignment, which a
  `char`/`unsigned char` array does not guarantee.** SPIR-V is a stream of
  32-bit words (SPIR-V spec section 2.2.1, "Layout of a Module",
  <https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html>), and the
  Vulkan 1.3 spec's `vkCreateShaderModule` section requires `pCode` to
  point at an array of `codeSize/4` `uint32_t` values
  (<https://registry.khronos.org/vulkan/specs/1.3/html/vkspec.html#vkCreateShaderModule>).
  An earlier version of `scripts/bin2c.c` emitted `static const unsigned
  char[]` and `bz_quest_vk.c` reinterpret-cast its address to
  `const uint32_t *` — an array of `unsigned char` has no alignment
  requirement beyond 1, so that cast was not standard-guaranteed valid,
  even though it happened to work on every ABI this project currently
  targets. `bin2c.c` now repacks the SPIR-V bytes into a real
  `static const uint32_t[]` at generation time (explicitly assuming
  little-endian byte order, matching every host `glslc` build and the
  arm64 Android target — see that file's top comment for why a
  byte-swapped input is a hard failure, not something silently handled),
  so the generated array itself carries the correct compiler-guaranteed
  alignment and `bz_quest_vk.c` no longer needs to cast at all.
- **The Khronos Android OpenXR loader AAR's Prefab package name is
  `"OpenXR"`, not the Maven artifact name.** Carried over from layer 2,
  still true and still load-bearing for `CMakeLists.txt`'s
  `find_package(OpenXR REQUIRED CONFIG)` — see layer 2's original
  verification of `org.khronos.openxr:openxr_loader_for_android:1.1.49`.

## Layer 5B: Warcraft III terrain rendering (`bz_quest_wc3_terrain*.c`/`bz_quest_vk_wc3_terrain.c`)

Layer 5B adds only terrain: ground splat layers, cliffs, and water, all built
from `platform/bridge/bz_tabletop_assets.h`'s already-shipped terrain ABI. The
module split mirrors 5A exactly:

| File | Kind | Owns |
|------|------|------|
| [`bz_quest_wc3_terrain.h`/`.c`](../platform/android/quest/app/src/main/cpp/bz_quest_wc3_terrain.c) | pure, host-testable | Terrain scale validation, chunk grid/bounds math, surface-layer UV selection, water/cliff detection, chunk mesh emission, and stable chunk/texture keys. |
| [`bz_quest_wc3_terrain_capture.h`/`.c`](../platform/android/quest/app/src/main/cpp/bz_quest_wc3_terrain_capture.c) | impure, ABI-calling | The one retain/copy/release walk over `BZ_TTA_LatestTerrain()`/`BZ_TTTerrain_*()`/`BZ_TTA_RegisterTerrainTexture()`/`BZ_TTAsset_*()`. Detects same-generation terrain cheaply and skips *chunk-metadata* rebuild work there, but re-offers each referenced ground/cliff/water texture on every call **only while its own per-generation pending flag is still set** (see "Bounded texture upload budget spans multiple frames" below) — a same-generation short-circuit must never also skip a still-pending texture's emission, and a completed texture must never be re-registered/re-copied. |
| [`bz_quest_vk_wc3_terrain.h`/`.c`](../platform/android/quest/app/src/main/cpp/bz_quest_vk_wc3_terrain.c) | impure, Vulkan-calling | Owns terrain-only descriptor set layout/pool/sampler/pipeline layout/shaders/staging buffer/upload command buffer plus one chunk cache and one texture cache, fully separate from both `bz_quest_vk.c` and `bz_quest_vk_wc3.c`. |
| [`shaders/terrain_vert.vert`/`terrain_frag.frag`](../platform/android/quest/app/src/main/cpp/shaders/terrain_vert.vert) | GLSL, compiled by `build-shaders.sh` | Unlit textured terrain draw with per-vertex RGBA so water can preserve its authoritative corner alpha. |

`bz_quest_renderer.c` now captures models and terrain once per frame, then
records both into one shared per-eye render pass in this fixed order:

1. terrain opaque (base ground + cliffs)
2. model opaque
3. terrain blended (extra splat layers + water)
4. model blended

Terrain uses the same generic FIFO cache implementation as 5A, but its
generation-lifetime differs: when the terrain identity changes, the module
`vkDeviceWaitIdle()`s and resets both terrain caches wholesale instead of
trying to reuse old chunk/image entries across maps. That is deliberate: model
assets can repeat across frames and maps, but terrain chunk geometry cannot.

### Coplanar depth compare for blended splats/water

Ground splat layers beyond the base layer, and water, are drawn at the exact
same quad positions as the already depth-written opaque base pass (they are
the same cell quad re-drawn with different UVs, not offset geometry). The
opaque terrain pipeline keeps `VK_COMPARE_OP_LESS` (normal occlusion against
geometry drawn earlier), but the blended terrain pipeline uses
`VK_COMPARE_OP_LESS_OR_EQUAL` so a coplanar blended fragment is not rejected
against its own already-written depth value; `depthWriteEnable = VK_FALSE` is
unchanged for blended draws, so this never corrupts the depth buffer used for
water/model transparency occlusion. `test_terrain_pipeline_depth_compare_is_less_or_equal_for_blended_only`
structurally asserts both compare ops from the checked-in source text (no
host-testable Vulkan device is available to assert this dynamically).

### Bounded texture upload budget spans multiple frames

`bz_quest_vk_wc3_terrain.c`'s `ensure_texture_uploaded()` caps new texture
creates to `BZ_QUEST_VK_WC3_TERRAIN_MAX_NEW_TEXTURE_UPLOADS_PER_FRAME` per
frame, mirroring 5A's model-texture budget. A texture deferred by the budget
is **not** lost: `bz_quest_wc3_terrain_capture()` tracks a per-referenced-
texture "pending" flag (`s_groundPending`/`s_cliffPending`/`s_waterPending`),
all set on a NEW terrain generation. On every call (including same-generation
frames), only textures still marked pending are re-registered, decoded via
`BZ_TTAsset_CopyImagePixels`, and fed to `onTextureReady`; the callback
returns `true` once the Vulkan side actually consumed the texture (uploaded,
or already cache-hit), which clears its pending flag so every later
same-generation call does **zero** registration/decode/copy work for it - it
is not merely a "cheap cache-find no-op", it is skipped before any ABI call
happens. A callback returning `false` (deferred by the per-frame budget, or a
transient registration/copy failure) leaves the texture pending so it is
retried on the very next call, and a new terrain generation repopulates every
pending flag via `reset_pending_textures()`. This drains a large texture set
across multiple frames without permanently dropping anything past the first
frame's budget, and without re-decoding/re-copying pixels for a texture that
already finished uploading - unconditionally re-offering every referenced
texture every frame regardless of completion status would re-copy potentially
many large images per frame indefinitely on mobile hardware, which is exactly
the bug this pending-flag gate fixes.

A texture still pending past its first deferral logs once
(`log_deferred_once()`, keyed by identity) so it stays explicitly diagnosable
without per-frame log spam. That log-once set is scoped to the current
terrain **generation**, not the whole app session: `reset_deferred_log()`
clears it on every generation swap (`reset_generation_caches()`) and on
module teardown (`bz_quest_vk_wc3_terrain_destroy()`). Without this reset, the
bounded set accumulates identities across every map reload forever, which (a)
eventually fills the set so a brand-new deferred identity on a later map can
no longer be recorded and instead reverts to logging every single frame, and
(b) wrongly suppresses a legitimately-new deferral for an identity STRING
reused across maps (e.g. a shared texture path) because an earlier
generation's stale entry is still sitting in the set.

`test_texture_budget_*` in `test_bz_quest_wc3_terrain.c` covers >4-texture
eventual upload across frames, same-generation dedup, create-failure retry,
and generation-reset eviction, using the same fake-cache harness as the
existing hit/miss/eviction test. `test_pending_textures_*` composes that same
harness with a local mirror of the capture-side pending-flag policy to prove,
with copy counters: the initial copy happens; a >4-texture set eventually
drains its budget across frames while textures completed in an earlier frame
get **zero** additional copy calls in later frames; only pending/failed
identities retry; and a generation reset causes a genuinely fresh copy (not a
false zero-copy skip). `test_capture_gates_texture_reoffer_on_a_pending_flag`
and `test_deferred_log_is_reset_on_generation_swap_and_teardown` structurally
assert the real pending-flag gating and log-once-per-generation reset exist
at their exact call sites in the checked-in source (no host-testable
`bzTTTerrain_t` bridge or `VkDevice` is available to exercise either path
dynamically). `test_deferred_log_*` behaviorally proves the log-once-with-
reset policy itself: once-per-identity-per-generation, a reused identity
logging again after a reset, many resets never leaking state, and capacity
exhaustion recovering once reset restores headroom.

Texture uploads also re-pack into a tightly-packed staging buffer one row at
a time via the shared, host-testable `bz_quest_wc3_terrain_repack_texture_tight()`
helper (mirroring `bz_quest_vk_wc3.c`'s model-texture upload path exactly):
`vkCmdCopyBufferToImage`'s `bufferRowLength = 0` always means "tightly
packed" regardless of the source's own row stride, so a source row stride
wider than `width*4` (e.g. BLP row padding) must never be flat-copied as-is
into the staging buffer. A source `rowBytes` shorter than `width*4` is
rejected outright (it would read past a row's true end), never silently
truncated. `test_repack_texture_tight_*` covers tight input, padded input,
an undersized destination, and an invalid too-short stride.

`test_bz_quest_wc3_terrain.c` host-covers the pure chunk builder's critical
paths: scale rejection, 32x32 tail-chunk clamping, authoritative quad winding,
ground splat atlas selection, water opacity, cliff detection/material fallback,
stable cache-key generation, the blended-pipeline depth-compare structural
assertion, the texture upload budget/pending-retry/dedup/reset scenarios, the
per-generation deferred-log reset, and the row-padding-safe texture repack
helper (see the subsections above). The existing descriptor-pool headroom
script now structurally checks the terrain texture descriptor pool's required
`capacity + 1` spare slot too.

## Layer 5C: Warcraft III model animation (`bz_quest_wc3_anim*`/`bz_quest_wc3_capture.c`/`bz_quest_vk_wc3.c`)

Layer 5C adds authoritative **model animation** on top of 5A's static-model
rendering: skeletal/vertex/geoset hierarchy, sequence and global-sequence
pose sampling, GPU (vertex-shader) skinning, and the dynamic material state
an animated unit/building/doodad needs (geoset visibility/alpha, team-color/
glow layer selection carried over from 5A's per-entity resolution). Explicitly
out of scope for this slice (each a real, logged gap, not an oversight - see
"Supported vs. unsupported animated behavior" below): camera-facing billboard
node override, texture-coordinate animation (TXAN) and material texture-ID
keyframes (KMTF), particles/effects, fog of war, selection decals, command-
card/HUD surfaces, gameplay input, audio, and data staging.

### Authoritative animation-state flow

Pose is derived **only** from data the authoritative snapshot/asset ABI
already exposes for this entity/model this frame - never a Quest-invented
wall clock:

1. `bz_quest_wc3_capture.c`'s `build_model_anim()` decodes a model's
   **immutable** animation data (node hierarchy/pivots, translation/
   rotation/scale tracks, sequence `[start_msec,end_msec)` ranges, global-
   sequence durations, per-geoset alpha tracks) once per model identity, via
   a two-pass ABI walk (`BZ_TTAsset_NodeInfo()`/`BZ_TTAsset_NodeTrackInfo()`/
   `BZ_TTAsset_SequenceInfo()`/`BZ_TTAsset_GlobalSequenceInfo()`/
   `BZ_TTAsset_GeosetAnimInfo()` for sizing, then the matching `Copy*Keys()`
   calls into one arena) - this is the SAME retained `bzTTAsset_t` 5A already
   acquires for static geometry, so no extra asset acquire/release round trip
   is introduced.
2. Each frame, `bz_quest_vk_wc3.c`'s `build_frame_dynamic_material()` reads
   the entity's own authoritative `bzTTEntity_t.frame` (msec) - the same
   snapshot field 5A already threads through `bzQuestWc3RenderItem_t` -
   and finds the active sequence by `[start_msec,end_msec)` matching
   (`find_active_sequence()`), mirroring desktop's `R_FindSequenceAtTime`
   exactly (see `bz_quest_wc3_anim.h`'s evidence comment). If no sequence
   matches (a genuinely invalid/upstream data condition - a valid entity
   always has an active sequence), the geoset falls back to an identity
   palette and `staticAlpha`/1.0 rather than guessing a sequence, and this is
   logged once per model identity (`"no-active-sequence"`).
3. For a **global** sequence track (`bzTTTrackInfo_t.global_sequence !=
   UINT32_MAX`), sampling instead uses `bz_quest_wc3_render_clock_msec()` -
   a Quest-owned `CLOCK_MONOTONIC` render clock (never entity/sequence time,
   never `CLOCK_REALTIME`) - modulo the global sequence's own duration, per
   `bz_quest_wc3_anim.h`'s evidence comment (transcribed from
   `r_mdx_anim.c:34-41`'s `tr.viewDef.time`/`SDL_GetTicks()` convention:
   global sequences are a render-driven ambient loop, e.g. a banner
   flapping, not gameplay-authoritative state - not a Quest invention).
4. `bz_quest_wc3_build_pose()` (pure, `bz_quest_wc3_anim.c`) samples every
   node's translation/rotation/scale track at the resolved time, builds each
   node's pivot-relative local matrix, then composes global matrices down
   the parent chain (`global = parent_global * local`, root nodes have no
   parent). `bz_quest_wc3_build_bone_palette()` then maps each geoset's
   resolved bone-palette node-index list (already node-index-remapped by the
   ABI - see `BZ_TTAsset_CopyGeosetMatrixPalette`'s doc comment) into the
   final per-geoset matrix palette the GPU consumes.
5. **Coordinate-basis conversion (fixed post-review; was previously missing).**
   Every matrix produced by steps 2-4 above (`bz_quest_wc3_anim.c`'s pivots,
   tracks, and composed global/palette matrices) is deliberately kept in raw
   MDX space (Z-up, left as documented in `bz_quest_wc3_anim.h`'s own header
   comment, to stay byte-for-byte faithful to desktop's formulas) - but
   `decode_geoset_geometry()` in `bz_quest_wc3_capture.c` already Y/Z-swaps
   every vertex position and normal (and flips triangle winding) into the
   target Y-up right-handed space before it ever reaches the GPU. Uploading
   the raw Z-up palette directly, as an earlier revision of this slice did,
   is a basis mismatch: the vertex shader multiplies an already-Y-up vertex
   by a still-Z-up bone matrix, so animated translations/rotations land on
   the wrong target axes even though static (identity-palette) models look
   correct. The fix, `bz_quest_wc3_convert_matrix_zup_to_yup()`
   (`bz_quest_wc3_render.h`/`.c`), applies `M_yup = S * M_zup * S` to every
   resolved palette slot in `build_frame_dynamic_material()`
   (`bz_quest_vk_wc3.c`), where `S` is the same Y/Z-swap matrix used for
   vertices; `S` is an involution (`S*S=I`), so this is the exact conjugation
   `S * M * S^-1` a change of basis requires, reduced algebraically since
   `S^-1 == S`. Verified independently (Python/numpy, not by invoking
   production code): identity stays identity; a pure MDX +Z translation
   becomes a pure target +Y translation; an MDX Z-axis rotation becomes a
   rotation strictly about target Y matching this codebase's own `Ry(-theta)`
   convention in `bz_quest_wc3_build_world_matrix()`. Regression-tested in
   `test_bz_quest_wc3_render.c` (`test_convert_zup_to_yup_identity_stays_identity`,
   `test_convert_zup_to_yup_translation_z_becomes_translation_y`,
   `test_convert_zup_to_yup_rotation_about_z_becomes_rotation_about_y`,
   `test_convert_zup_to_yup_in_place_matches_separate_buffer`,
   `test_convert_zup_to_yup_preserves_hierarchy_composition` - the last one
   builds a real 2-node parent/child pose via `bz_quest_wc3_build_pose()` and
   proves the conversion commutes correctly with hierarchy composition:
   `S*(parent*local)*S == (S*parent*S)*(S*local*S)`).
6. The converted palette is written into the persistently-mapped bone-palette
   UBO and bound via a per-draw dynamic offset; the vertex shader does the
   actual skinning (see "GPU skinning" below) - the CPU never transforms a
   single vertex.

No step above samples wall-clock time for entity-driven (non-global)
animation; the only clock read is the Quest render clock, and only for the
global-sequence case the desktop renderer itself also drives from a
render-side clock, not simulation time.

### ABI decision: extended in place (v2 → v3), not tunneled

Before writing any code, the existing v2 ABI
(`platform/bridge/bz_tabletop_assets.h`/`.c`) was audited against every piece
of data this slice's scope needs. It already exposed static geometry/
materials (5A) and terrain (5B), but exposed **no** node hierarchy, keyframe
tracks, sequence/global-sequence timing, or per-geoset alpha animation data
at all - there was concrete evidence of a real gap, not a convenience
preference, so the smallest possible versioned extension was added
(`46e348f`, `BZ_TABLETOP_ASSETS_ABI_VERSION` 2u → 3u):

- `bzTTSequenceInfo_t`/`BZ_TTAsset_SequenceInfo()` - name, `[start_msec,
  end_msec)`, move speed, flags, rarity, sync point, bounds.
- `bzTTNodeInfo_t`/`BZ_TTAsset_NodeInfo()` - name, raw MDX `object_id`/
  `parent_id` (resolved to array indices by the ABI's own
  `node_index_for_object_id()` before this slice ever sees them), flags
  (unfiltered - see "billboarding" below), pivot, initial translation/
  rotation (quaternion)/scale.
- `bzTTTrackInfo_t`/`BZ_TTAsset_NodeTrackInfo()` + `BZ_TTAsset_CopyNode
  TranslationKeys()`/`CopyNodeRotationKeys()`/`CopyNodeScaleKeys()` - key
  count, interpolation type (`bzTTKeyInterp_t`, matches `MODELKEYTRACKTYPE`
  exactly), global-sequence index (`UINT32_MAX` = none), and the actual
  vec3/quaternion keyframe arrays (value + in/out tangents for HERMITE/
  BEZIER).
- `BZ_TTAsset_GlobalSequenceInfo()` - duration in msec, indexed by
  `bzTTTrackInfo_t.global_sequence`.
- `bzTTGeosetAnimInfo_t`/`BZ_TTAsset_GeosetAnimInfo()` +
  `BZ_TTAsset_CopyGeosetAlphaKeys()` - per-geoset static alpha (used
  verbatim when the geoset has no alpha track) or an animated float track,
  plus the geoset's already node-index-remapped bone-palette node-index
  list (`BZ_TTAsset_CopyGeosetMatrixPalette()`) and per-vertex skin weights
  (`bzTTVertexSkin_t`/`BZ_TTAsset_CopyGeosetVertexSkin()`).

Every accessor returns raw, POD, engine-agnostic data (msec/floats/quaternion
components/plain index arrays) - **no Vulkan handle, no engine pointer, no
Quest-specific type** crosses the ABI boundary, keeping the extension a true
shared contract rather than a tunnel. The **producer** side
(`games/warcraft-3/visionos/wc3_mdx_decode.c` - the single shared MDX decoder
linked into both the visionOS app and this Quest build, despite its
directory name; see `games/warcraft-3/game.mk`'s `test-bz-tabletop-assets`
schema, which links it directly) was updated so both platforms decode from
the same real MDX data with no divergent parsing. The version bump is
enforced by `BZ_TTA_ERR_ABI_VERSION` and covered by `games/warcraft-3/tests/
test_bz_tabletop_assets.c`'s compatibility/error tests against a purpose-
built synthetic `rigged_anim` MDX fixture (`tools/mdxgen.c`) with a real bone
hierarchy, keyframed tracks, a global sequence, and a geoset alpha track -
these tests pass unchanged, confirming visionOS's existing static-model
consumption path (which never reads the new node/track/geoset-anim
accessors - grepped `WarcraftAssetAdapter.swift`, no reference) stays green
against the extended ABI. **Quest's `bz_quest_wc3_capture.c` (this layer) is
the first and only real *consumer* of the new animation accessors** -
visionOS's own Swift rendering stack does not read them (its skeletal
animation support was already confirmed non-functional/stub before this
slice began, and fixing that is out of this Quest-focused task's scope); no
Swift file was touched by this slice.

### Immutable animation asset cache

`bzQuestWc3ModelAnim_t` (`bz_quest_wc3_render.h`) is the Quest-owned,
retained-handle cache of a model's immutable animation data: **one** heap
arena backs every pointer field (nodes/sequences/global-sequence durations/
geoset-anim tracks and every keyframe array they reference), sized to the
model's *real* data by a first Info()-only sizing pass and filled by a
second pass of real `Copy*()` calls straight into arena-relative addresses -
one `malloc()`, one `free()`, no fixed worst-case over-allocation.
`bz_quest_wc3_model_anim_free()` releases both in the correct order and is
NULL-safe (a genuinely static/non-animated model has `meta.anim == NULL` and
this is always a valid, cheap no-op).

**Ownership-transfer bug found and fixed this slice**: `decode_model()`
rebuilds a fresh `bzQuestWc3ModelAnim_t` arena on every call (it has no
"already decoded" check of its own - see `bz_quest_wc3_capture.h`'s
documented CPU-decode-recurs-every-frame trade-off, inherited from 5A), but
`bz_quest_vk_wc3.c`'s `model_ready_cb()` only *moves* that pointer into the
persistent GPU cache entry on a genuine cache **miss**; on every other path
(cache hit, upload-budget deferral, create failure) the freshly-decoded arena
was previously silently discarded with no `free()` at all - a real per-frame
leak for every model touched after its first frame. Fixed by calling
`bz_quest_wc3_model_anim_free(model->meta.anim)` on every one of those
non-success paths, and by `model_cache_destroy()` freeing the *cached*
entry's arena on eviction/shutdown. `platform/android/quest/scripts/
test-wc3-bone-palette-layout.sh` structurally guards this contract (no host-
buildable Vulkan cache is available to exercise it dynamically), and
`test_bz_quest_wc3_render.c`'s `test_model_anim_free_null_is_a_no_op`/
`test_model_anim_free_releases_arena_and_struct` host-cover the release
function itself (NULL-safety and the real single-allocation free path).

**Second, distinct NULL-callback ownership leak found and fixed this
slice**: `decode_model()` previously called `build_model_anim()`
unconditionally, but `model_ready_cb()` — the *only* code path anywhere
that frees or transfers ownership of `meta.anim` (the hit/miss/deferral
paths above) — is itself only invoked by `bz_quest_wc3_capture_frame()`
`if (callbacks && callbacks->onModelReady)`. With a NULL `onModelReady`,
the freshly-`malloc()`'d arena pointer was silently overwritten by the
next frame's `memset(&s_scratchModel, 0, ...)`, leaking it — a case the
header's own documented contract ("safe to call with `callbacks` fields
NULL — no-ops, decode is simply skipped") explicitly promised but did not
implement for this one field. Fixed by only building the arena
`if (callbacks && callbacks->onModelReady)`, leaving `meta.anim` `NULL`
(already zeroed) otherwise, so the documented "skip decode" behavior now
actually holds for the animation arena, matching every other resource this
policy already covers. There is exactly one production caller
(`bz_quest_wc3_capture_and_upload()`), and it always sets both callbacks
together, so this was a defensive-contract gap, not a reachable production
leak. `bz_quest_wc3_capture.c` calls 35 distinct `platform/bridge` ABI
functions that are only implemented by real, Android/NDK- or WC3-game-
linked provider libraries (see `games/warcraft-3/tests/
test_bz_tabletop_assets_stubs.c` for the one precedent, which links the
real WC3 asset provider and game-engine state — architecturally unrelated
to and far heavier than this Quest-only file could reasonably link on
host), so — consistent with this file's own pre-existing "no direct unit
test, reviewed by inspection" trade-off (identical to `bz_quest_snapshot.c`)
— no new fake-ABI host-test harness was built for this one boolean gate;
the closest legitimate automated coverage remains the existing
`bz_quest_wc3_model_anim_free()` tests above, and the fix is otherwise
compile- and behavior-verified via the real `make quest-assemble-debug`
NDK build.

### GPU skinning: vertex-shader, not CPU

Chosen because it is the **only** path the reviewed desktop renderer itself
uses (`renderer/r_shader.c`/`renderer/r_buffer.c` - there is no CPU-skinning
code path in this codebase to fall back to), and because every geoset -
animated or static - always has a *resolved* bone-palette node-index list
per the ABI's own guarantee (`BZ_TTAsset_CopyGeosetMatrixPalette`'s doc
comment), so a static model's geoset just resolves to an all-identity
palette: **one** shader/pipeline path draws both, with no separate static/
skinned pipeline variant to keep in sync.

- **Vertex attributes** (`bzQuestWc3Vertex_t`, `bz_quest_wc3_render.h`):
  `boneIndex[4]` (`VK_FORMAT_R8G8B8A8_UINT` - raw, unnormalized bone
  indices) and `boneWeight[4]` (`VK_FORMAT_R8G8B8A8_UNORM` - hardware-
  normalized 0..255 → 0.0..1.0), matching the exact GL attribute formats
  `renderer/r_buffer.c`'s `i_boneIndex1`/`i_boneWeight1` use
  (`GL_UNSIGNED_BYTE`/`GL_FALSE` for the index, `GL_UNSIGNED_BYTE`/`GL_TRUE`
  for the weight) - transcribed, not guessed.
- **Skinning formula** (`shaders/warcraft_vert.vert`): `skinned +=
  (uBonePalette.bones[inBoneIndex[i]] * pos4) * inBoneWeight[i]` for `i` in
  `0..3`, no lookup-index offset (desktop's `uFirstBoneLookupIndex` is
  always `0.0` for MDX geometry) - a per-vertex weighted sum of up to 4 bone
  transforms, exactly the desktop formula.
- **Bone-palette UBO** (`bz_quest_vk_wc3.c`'s `create_palette_resources()`):
  one `VkDescriptorSetLayout` (set 1, binding 0,
  `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC`, vertex stage only), one
  descriptor set allocated **once**, bound with a different dynamic offset
  per draw - not one set per skinned draw. Backing storage is a single
  `HOST_VISIBLE|HOST_COHERENT`, persistently-mapped `VkBuffer` (this data
  changes every frame; a device-local buffer plus a staging round trip
  would cost a second GPU copy for no benefit here, unlike the model/
  texture data 5A uploads once and reuses for many frames).
  `paletteSlotStride` is `BZ_QUEST_WC3_MAX_MATRIX_PALETTE` (128) mat4s
  (8192 bytes) **rounded up to the device's own**
  `VkPhysicalDeviceLimits::minUniformBufferOffsetAlignment` at `create()`
  time (queried via `vkGetPhysicalDeviceProperties()`, never hardcoded),
  and checked against `VkPhysicalDeviceLimits::maxUniformBufferRange` with
  a **hard `create()` failure** (never a silent clamp) if the aligned
  stride would exceed it. Slot 0 is permanently reserved as an all-identity
  palette, written once at `create()` time; slots 1..
  `BZ_QUEST_WC3_MAX_SKINNED_DRAWS_PER_FRAME` serve the frame's real skinned
  draws, reset to unused at the top of every
  `bz_quest_vk_wc3_capture_and_upload()` call. A geoset that overflows this
  per-frame budget binds slot 0 (identity) and is logged once per model
  identity (`"palette-budget-exceeded"`) - never silently frozen/dropped.
- `build_frame_dynamic_material()` runs **exactly once per frame** (inside
  `bz_quest_vk_wc3_capture_and_upload()`, after the model-cache upload pass
  so newly-uploaded models are already resolvable), never once per eye -
  a mistake made and corrected during this slice's implementation (see this
  file's own doc comment on the forward-declaration/call-site history).

`test-wc3-bone-palette-layout.sh` structurally guards every one of the
bullet points above against silent regression (device-limit derivation, the
"+1 identity slot" constant, the exact vertex attribute formats, the
descriptor type, and the anim-arena ownership contract) directly from the
checked-in source, since no NDK/Gradle/Vulkan device is available in this
development environment to exercise `create_palette_resources()` /
`build_frame_dynamic_material()` dynamically.

### Supported vs. unsupported animated behavior

| Behavior | Status |
|---|---|
| Skeletal hierarchy, translation/rotation/scale keyframes, all 4 interpolation modes (NONE/LINEAR/HERMITE/BEZIER) | Supported - `bz_quest_wc3_anim.c`, 26 host tests |
| Sequence selection from authoritative `bzTTEntity_t.frame` | Supported |
| Global sequences (render-clock-driven, wraps modulo duration+1) | Supported |
| Non-looping clamp / before-first-key / past-last-key wraparound, missing tracks, out-of-range/cyclic parent indices | Supported - degrades to identity/root rather than hanging or crashing |
| Per-geoset bone-palette resolution, static-model identity-palette fallback | Supported |
| Geoset alpha animation (`GEOA`/`KGAO`, multiplicative with layer alpha) | Supported |
| Team-color/glow texture selection per entity (carried over from 5A's per-entity resolved identities) | Supported |
| Camera-facing billboard nodes (`MDLXNODE_Billboarded`, `r_mdx.h:49`) | **Not implemented.** A billboarded node still poses correctly via its normal parent-chain hierarchy transform (everything this slice claims), it just does not additionally face the viewer each frame - that needs this frame's view orientation threaded into pose building, which no authoritative snapshot/ABI field exposes. Logged once per model identity (`"node-billboard-unsupported"`), never silently dropped. |
| Texture-coordinate animation (TXAN) / material texture-ID keyframes (KMTF) | **Not exposed by the ABI at all** - `platform/bridge/bz_tabletop_assets.h` has no TXAN/KMTF accessor of any kind, so there is nothing to detect or diagnose at runtime (unlike billboarding, where the raw flag *is* exposed but unused). This is a genuine ABI scope boundary, not a silently-ignored track: no evidence surfaced that any currently-scoped model requires it, so no ABI extension was added speculatively. |
| Replaceable_id 1/2 (team color/glow) and per-entity image overrides for **static** (non-animated) layers | Same 5A scope boundary, unchanged - see "Supported vs. unsupported material behavior" above. |

### Tests

- `platform/android/quest/tests/test_bz_quest_wc3_anim.c` - 26 host tests for
  the pure pose-math module: track sampling with/without a track present
  (translation/scale/alpha defaults), all 4 interpolation modes against
  hand-derived expected values, exact-key-hit, before-first-key clamp,
  single-key hold, past-last-key wraparound (and the boundary case where the
  first key sits exactly at the wrap interval start), quaternion slerp,
  global-sequence-uses-render-clock vs. non-global-uses-entity-frame, node
  local matrix (no tracks/translation-only/pivot-preserving rotation),
  hierarchy composition (child inherits parent, root-alone, cyclic-parent
  degrades to root rather than hanging, out-of-range parent treated as
  root), and bone-palette mapping (including out-of-range node index
  falling back to identity).
- `games/warcraft-3/tests/test_bz_tabletop_assets.c` - ABI v3
  compatibility/error tests against the `rigged_anim` synthetic MDX fixture
  (real bone hierarchy, keyframed tracks, a global sequence, a geoset alpha
  track), covering every new accessor's success and malformed/absent-data
  paths.
- `test_bz_quest_wc3_render.c`'s two new tests
  (`test_model_anim_free_null_is_a_no_op`/
  `test_model_anim_free_releases_arena_and_struct`) cover the anim-arena
  release function's NULL-safety and real single-allocation free path.
- `platform/android/quest/scripts/test-wc3-bone-palette-layout.sh`
  (new this layer, wired into `make test`/`make quest` as
  `test-quest-wc3-bone-palette-layout`) structurally guards the bone-palette
  UBO layout, vertex attribute formats, descriptor type, and anim-arena
  ownership contract described above - no host-buildable Vulkan device
  exists in this environment to exercise `bz_quest_vk_wc3.c`'s device-limit
  queries or descriptor/buffer creation dynamically, so (mirroring
  `test-wc3-descriptor-pool-headroom.sh`'s established technique) this
  script greps the real source for the specific properties those runtime
  calls depend on.
- Cache hit/miss, eviction, and shutdown/release for the *generic* GPU cache
  machinery the anim-bearing model cache entry rides on top of are already
  covered by `test_bz_quest_wc3_cache.c`'s 28 existing tests (unchanged this
  layer - `bz_quest_wc3_cache.c` itself has no animation-specific logic, it
  is a generic identity-keyed create/destroy cache reused as-is).
  `bz_quest_wc3_capture.c` (the one impure, ABI-calling translation unit
  that decodes animation data) has **no direct unit test**, matching the
  pre-existing, reviewed 5A precedent documented in its own header comment
  (`bz_quest_snapshot.c`'s same trade-off) - it is reviewed by inspection,
  not a fake-ABI test harness, and its call sites into the pure/tested
  `bz_quest_wc3_anim.c` module carry the actual sampling/hierarchy logic.
- `make test-quest-host-tests`: 4040/4040 assertions (up from 4038/4038 at
  the end of layer 5B; the delta is the two new
  `test_bz_quest_wc3_render.c` anim-free tests - `bz_quest_wc3_anim.c`'s own
  26 tests were added earlier in this same branch's development and are
  already included in the 4038 baseline).

### Acceptance gates (adds to "Hardware-only acceptance gates" above)

Everything in "Hardware-only acceptance gates" above still applies
unchanged; additionally, **none** of the following was verified against
real retail Warcraft III animated model data or a physical device this
session - do not report any of these as confirmed:

- Whether a real animated MDX model (skeleton, keyframed sequences, a
  global sequence, geoset alpha) actually decodes end to end through
  `bz_quest_wc3_capture.c`'s `build_model_anim()` and produces a visually
  correct pose on-device - only the synthetic `rigged_anim` mdxgen fixture
  (via `test_bz_tabletop_assets.c`) and hand-derived pure-math test cases
  (via `test_bz_quest_wc3_anim.c`) were exercised in this environment.
- Whether `create_palette_resources()`'s device-limit queries
  (`minUniformBufferOffsetAlignment`/`maxUniformBufferRange`) and the
  bone-palette UBO/descriptor set actually succeed and bind correctly
  against the real Adreno GPU driver Quest ships - only compiled
  (`make quest-assemble-debug`) and structurally checked
  (`test-wc3-bone-palette-layout.sh`), never executed against a runtime.
- Real visual correctness of any animated model's pose, sequence timing,
  team-color selection, or geoset alpha fade on real hardware - only
  provable by a human wearing the headset with a real map and real staged
  `War3.mpq`/`War3x.mpq` data loaded, neither of which was available here.
- Per-frame CPU cost of `build_frame_dynamic_material()`'s pose-math re-run
  for every touched model every frame (this slice does not cache poses
  across frames, matching 5A's own documented "CPU decode recurs every
  frame" trade-off for geometry) - untestable without a device to profile.

## Manifest requirements

Unchanged from layer 2: `AndroidManifest.xml`'s NativeActivity metadata,
`com.oculus.supportedDevices` scoped to `quest3|quest3s`, and headtracking
`uses-feature` were verified against Meta's mobile-native manifest guidance
(<https://developers.meta.com/horizon/documentation/native/android/mobile-native-manifest/>).
No `horizonos:uses-horizonos-sdk` element (Spatial SDK panel-app specific,
not applicable to a plain OpenXR NativeActivity app).

## Package/app identifiers

Unchanged from layer 2: `org.openrealm.quest` / `bz_quest_native` /
`openwarcraft3-*` are project-private placeholders for this sideloaded
debug prototype. Replace before any wider distribution.

## Current limitations

Everything below is explicitly out of scope for this layer; each has a
compile-time `#error`-guarded seam in `bz_quest_host.c`
(`BZ_QUEST_ENABLE_INPUT`, `BZ_QUEST_ENABLE_AUDIO`,
`BZ_QUEST_ENABLE_DATA_STAGING`) so a later layer flips exactly one on as its
real implementation lands, instead of a silent stub reporting fake success.
`BZ_QUEST_ENABLE_VULKAN_RENDERER`, `BZ_QUEST_ENABLE_ENGINE_START`,
`BZ_QUEST_ENABLE_BRIDGE_SNAPSHOTS`, and (new this layer)
`BZ_QUEST_ENABLE_WC3_RENDERER` are now all hard-required to `1` (a `#error`
fires if a build tries to set any of them to `0`) since this layer's
Warcraft renderer, procedural renderer, and tabletop bridge are no longer
optional:

- No OpenXR action/input polling, no audio output, no War3 MPQ data staging
  onto the device (no ADB data-copy script yet — a developer must stage
  data manually per "Data-path contract" above until that later layer
  lands).
- No map is ever loaded (`bz_quest_bridge_start()` always passes a `NULL`
  map name) — see "Known limitations of this frame descriptor" above. With
  no map loaded, `BZ_TT_Latest()` returns an entity-less snapshot, so layer
  5A's captured render list is always empty in this environment and every
  eye renders the procedural diagnostic scene (see "Diagnostic-scene
  fallback is explicit, not silent" above) — this was never observed
  rendering real Warcraft geometry on a live snapshot in this environment
  (see "Hardware/data-only acceptance procedure" below).
- No terrain (see "Layer 5B" instead), no fog of war, no selection decals,
  no particles/effects, no command-card/HUD surfaces — see "Layer 5A" and
  "Layer 5C" above for the exact, deliberate scope boundaries (skeletal/
  sequence animation IS now supported — see "Layer 5C" above).
- No lighting model at all (fully unlit shader) — see "Shader/pipeline"
  above for why this was a deliberate scope decision, not a bug.
- `replaceable_id 0` (direct/non-team), `1` (team color), and `2` (team
  glow) textures are all supported (team color/glow added in layer 5C —
  see "ABI decision"/"Supported vs. unsupported animated behavior" above);
  any *other* replaceable_id (a per-entity image override) is still logged
  once and skipped, not substituted — see "Supported vs. unsupported
  material behavior" above.
- Transparency ordering is per-entity (by world-translation distance to the
  eye), not per-triangle — see "Draw ordering / transparency" above.
- No gameplay controller input reaches the engine (no `BZ_TabletopSubmit*`
  command is ever called by this layer).
- No Vulkan multiview, MSAA, or fixed foveation (`XR_FB_foveation`) — see
  "Vulkan render pass/pipeline/targets" above for why this is an explicit,
  documented seam rather than an oversight.
- The tabletop asset ABI **was** widened once, in layer 5C: `bzTTAsset_t`
  went from v2 (layers 5A/5B's static geometry/materials + terrain) to v3
  (node hierarchy/keyframe tracks/sequences/global sequences/geoset alpha),
  after concrete evidence showed v2 exposed no animation data whatsoever —
  see "ABI decision: extended in place (v2 → v3), not tunneled" above. No
  further widening has occurred since; camera-facing billboarding and
  texture-coordinate/material-ID animation (TXAN/KMTF) remain outside the
  ABI's exposed surface — see "Layer 5C"'s "Supported vs. unsupported
  animated behavior" table above.

### Hardware-only acceptance gates

**No physical Meta Quest device was available in this development
environment.** Everything below requires real hardware and was **not**
verified this session — do not report session creation, passthrough
activation, stereo correctness, frame rate/timing, engine startup against
real staged data, or any visual result as confirmed until each is checked
against a real device:

- Whether `xrCreateSession`/`xrCreateSwapchain`/`xrBeginSession` actually
  succeed against a real Quest 3/3S OpenXR runtime (only compile-time/
  syntax-verified against the extracted OpenXR AAR headers on this
  machine — never executed against a runtime).
- Whether the system genuinely reports `XR_PASSTHROUGH_CAPABILITY_BIT_FB`
  and `XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND` support (both hard-fail
  startup if absent — see "Passthrough is mandatory" — but which specific
  Quest 3/3S runtime/OS version actually reports them was not checked here).
- Whether the negotiated Vulkan instance/device/queue actually create
  successfully against the real Adreno GPU driver Quest ships (only the
  `XR_KHR_vulkan_enable2` call sequence was verified against the spec/
  `hello_xr`, never executed).
- Real stereo correctness (per-eye separation, no swapped left/right eyes,
  correct FOV/aspect per eye) — only provable by a human wearing the
  headset.
- Real frame timing/rate, dropped-frame behavior under
  `XR_SESSION_STATE_SYNCHRONIZED`/backgrounding, and whether the
  fence-wait-per-eye synchronization strategy (see the
  `XR_KHR_vulkan_enable2` fence-handoff quirk above) is fast enough for a
  comfortable frame rate — untestable without a device to profile.
- Whether `ANativeActivity::internalDataPath`/`externalDataPath` actually
  resolve to the paths this document assumes on a real Quest 3/3S OS
  build, and whether `adb push`-staged real `War3.mpq`/`War3x.mpq` (or an
  override file) is actually discovered and loaded successfully by
  `BZ_RuntimeInit()` on-device — only the path-construction/validation
  logic itself was host-tested (see "Testing" above); the actual
  filesystem paths a real device reports were never observed.
- Whether the bridge/engine thread genuinely starts, advances snapshot
  generations, and tears down cleanly across real Android pause/resume/
  destroy and OpenXR session-loss events on physical hardware — only a
  synthetic, non-Quest `pthreads`-hosted lifecycle run (via
  `make test-quest-bridge`, see "Testing" above) was exercised this
  session, not a real Android `onAppCmd` sequence.
- Whether `adb install`/launch/logcat actually behave as described in
  "Exact on-device acceptance procedure" above — the procedure is written
  from the OpenXR/Android/NDK spec and Meta's own documentation, not from
  having run it.
- The Meta manifest guidance page and the OpenXR/Vulkan spec URLs cited
  throughout this document are versioned/updatable; re-fetch and diff
  before relying on them for a release build.
- **New this layer** — none of the following was verified against real
  retail Warcraft III data or a physical device; **do not report any of
  these as confirmed** until checked on a real Quest with real staged
  `War3.mpq`/`War3x.mpq` data:
  - Whether real MDX model geometry/materials actually decode correctly end
    to end through `bz_quest_wc3_capture.c` (only syntax/type-checked and
    compiled against the real headers/NDK Vulkan headers this session — see
    "What *was* verified this session" below — never run against a live
    snapshot with real entities, since no map was ever loaded in this
    environment).
  - Whether the Vulkan buffer/image/descriptor creation, staged uploads,
    and pipeline-variant selection in `bz_quest_vk_wc3.c` actually succeed
    against the real Adreno GPU driver Quest ships (only compiled, never
    executed against a runtime).
  - Real visual correctness of any rendered model — position/scale/
    orientation, texture appearance, blend mode appearance — is only
    provable by a human wearing the headset with a real map loaded; the
    coordinate/scale conversion math itself is host-tested (see "Testing"
    below) against synthetic inputs, not against a real MDX model's actual
    on-screen appearance.
  - Whether the bounded per-frame upload budget
    (`BZ_QUEST_VK_WC3_MAX_NEW_MODEL_UPLOADS_PER_FRAME`/
    `_MAX_NEW_TEXTURE_UPLOADS_PER_FRAME`) is well-tuned for a real map's
    worth of unique models/textures streaming in after a real map load —
    untestable without real data and a device to profile against.

### Hardware/data-only acceptance procedure

Two independent things must both be true before this layer can be called
"working" on a real device — physical Quest hardware, AND real staged WC3
data — and this document distinguishes exactly which each of the following
sub-procedures actually proves:

**A. Hardware-only (no real WC3 data staged, override left unset).** Proves
the bridge starts and fails *cleanly* — no crash, no hang, Android/OpenXR
teardown stays fully responsive:

1. Run steps 1-2 of "Exact on-device acceptance procedure" above (build,
   install, launch, tail `OpenRealmQuest:V`) with **no**
   `warcraft_data_path_override.txt` staged and no data pushed to
   `externalDataPath`/`internalDataPath`.
2. Expect log line 10 from that procedure to read
   `bz_quest_bridge_start failed: ...` (a `"Failed to add data directory:
   ..."` reason from the engine, surfaced through
   `bz_quest_bridge_last_error()`), **not** `succeeded`.
3. Expect the app to keep running (passthrough camera + checkerboard test
   scene) and to exit cleanly on `adb shell am force-stop
   org.openrealm.quest` or the Oculus button — confirms "surface the exact
   path/error and enter the lifecycle's real failed state while keeping
   Android/OpenXR teardown responsive" without needing any real game data.

**B. Data-only (no headset — a bounded non-Quest host build).** Proves the
*shared* lifecycle/transport core this bridge wraps genuinely starts,
advances snapshot generations, and stops cleanly, independent of any
Quest/Android/OpenXR code:

1. `make test-quest-bridge` — links the real
   `bz_tabletop_lifecycle.c`/`common/bz_runtime.c`, stages a valid
   synthetic data directory override (`build/tests/tests.mpq`, produced by
   `make test-assets`), and runs `BZ_TabletopCreate()`/`Start()`/
   `Suspend()`/`Resume()`/`Stop()`/`Destroy()` against it end to end on
   this host, with no Quest/Android/OpenXR code involved at all (see
   `test_bz_quest_bridge.c`'s `test_start_with_valid_override_reaches_running`).
2. This is the closest bounded, hardware-free proof available in this
   environment that "advancing snapshots" really works, given that no full
   retail `War3.mpq`/`War3x.mpq` data is present on this machine — only
   the synthetic `test-assets` fixture is (see "Testing" above for exactly
   why a `+com_frame_limit`-bounded *full* engine run was not additionally
   attempted here).

**Neither A nor B alone proves the Quest map boots or that snapshots
advance while wearing a headset** — that requires physical hardware *and*
real staged data together, which was not available in this environment;
do not report either as confirmed until both are combined on a real
device.


### What *was* verified this session

- All `bz_quest_*.c` files individually syntax-check clean
  (`-fsyntax-only -Wall -Wextra --target=aarch64-linux-android29`) against
  the real extracted OpenXR AAR headers and the NDK's Vulkan headers.
- The full Gradle/CMake `assembleDebug` build succeeds end to end (`JAVA_HOME`
  pointed at Temurin 17, NDK 27.2.12479018), producing `app-debug.apk` with
  `lib/arm64-v8a/libbz_quest_native.so` — now statically linking
  `bz_quest_data.c`/`bz_quest_frame.c`/`bz_quest_snapshot.c`/
  `bz_quest_bridge.c` alongside the existing renderer/scene sources.
- `scripts/verify-native-lib.sh` passes against that APK: exactly one
  packaged ABI (`arm64-v8a`); `DT_NEEDED` is exactly
  `libopenxr_loader.so libvulkan.so libandroid.so liblog.so libz.so libm.so
  libdl.so libc.so` (no SDL2/desktop-GL/Apple-ObjC/VrApi dependency, and no
  new dynamic dependency was introduced by this layer — the tabletop
  lifecycle/runtime is already statically linked via `openwarcraft3-bridge`/
  `openwarcraft3-engine`); no forbidden symbol referenced; no `main()`
  symbol linked; `ANativeActivity_onCreate` and `android_main` both present
  in the dynamic symbol table.
- The shader build pipeline produces valid SPIR-V (magic-number-checked)
  for both `tabletop_vert.vert`/`tabletop_frag.frag`, embedded into the
  linked `.so` with no committed binary.
- `make test-quest-source-sync`, `make test-quest-host-tests` (3349/3349
  assertions across `bz_quest_pure.c`'s math/selection helpers,
  `bz_quest_scene.c`'s procedural generator, and — new this layer —
  `bz_quest_data.c`'s data-dir/argv resolution and `bz_quest_frame.c`'s
  frame-descriptor/throttled-log logic), `make test-quest-bridge` (67/67
  assertions, new this layer, linking the real
  `bz_tabletop_lifecycle.c`/`bz_runtime.c` against a synthetic staged data
  directory — see "Hardware/data-only acceptance procedure" above), and the
  full repo-root `make test` all pass with no regressions.
- `git diff --check clancey-quest-vulkan-openxr-session...HEAD` reports no
  whitespace errors.

### What *was* verified this session (layer 5A)

- `bz_quest_wc3_render.c`/`bz_quest_wc3_cache.c` (pure, host-buildable) pass
  `make test-quest-host-tests` with full coverage of coordinate/scale
  conversion, render-list construction (skip-no-model, overflow reporting,
  stale-state rewrite), and GPU-cache bookkeeping (first-call miss,
  same-key hit, independent-key misses, create-failure-does-not-insert,
  FIFO eviction at capacity, shutdown destroys every entry, shutdown of an
  empty/zero-initialized cache is safe) — 3607/3607 assertions across the
  whole `test-quest-host-tests` binary (up from 3349/3349 at the end of
  layer 4; the delta is these two new modules' tests).
- The full Gradle/CMake `assembleDebug` build succeeds end to end
  (`JAVA_HOME` pointed at Temurin 17, NDK 27.2.12479018) with
  `bz_quest_wc3_render.c`/`bz_quest_wc3_cache.c`/`bz_quest_wc3_capture.c`/
  `bz_quest_vk_wc3.c` newly linked into `libbz_quest_native.so`, and both
  new shaders (`warcraft_vert.vert`/`warcraft_frag.frag`) compiling to valid
  embedded SPIR-V alongside the existing `tabletop_vert.vert`/
  `tabletop_frag.frag` — this caught and fixed one real build error (the
  Vulkan-side blend-mode dispatch initially referenced
  `bz_tabletop_assets.h`'s `BZ_TTA_BLEND_*` names directly, but
  `bz_quest_vk_wc3.c` deliberately does not include that header — fixed by
  mirroring the enum locally with a `_Static_assert` cross-check in
  `bz_quest_wc3_capture.c`, the one file that does include it).
- `scripts/verify-native-lib.sh` passes against that APK with **no change**
  to the allow-listed `DT_NEEDED` set (`libopenxr_loader.so libvulkan.so
  libandroid.so liblog.so libz.so libm.so libdl.so libc.so`) — this layer
  introduced zero new shared-library dependencies, confirming no SDL2/
  desktop-GL/Apple-ObjC/VrApi dependency entered the APK from the new
  Vulkan/model-rendering code.
- `make test-quest-source-sync`, `make test-quest-host-tests` (3607/3607
  assertions, see above), `make test-quest-bridge` (67/67 assertions,
  unchanged from layer 4), and the full repo-root `make test` all pass with
  no regressions.
- `git diff --check clancey-quest-tabletop-lifecycle-bridge...HEAD` reports
  no whitespace errors.
- **Not verified this session** (no real MDX/BLP data or physical device
  available) — see "Hardware-only acceptance gates" above for the exact
  list: real model decode correctness, on-device Vulkan resource creation,
  visual correctness of any rendered model, and per-frame upload-budget
  tuning against a real map's data volume.

### What *was* verified this session (layer 5C)

- The ABI v2 → v3 extension in `platform/bridge/bz_tabletop_assets.h`/`.c`
  and the shared MDX producer update in
  `games/warcraft-3/visionos/wc3_mdx_decode.c` are exercised by
  `test_bz_tabletop_assets.c`'s ABI compatibility tests (including the new
  `rigged_anim` mdxgen fixture — a real, synthetically-generated skeletal
  MDX with bones/keyframed translation-rotation-scale tracks, a global
  sequence, and geoset alpha animation), and the pure pose-math module
  `bz_quest_wc3_anim.c` passes 26 hand-derived host tests covering
  hierarchy composition, all three interpolation modes (none/linear/
  Hermite) and their endpoints, pivot transforms, global sequences,
  missing-track fallback to the node's bind pose, and non-looping-sequence
  clamping — all part of `make test-quest-host-tests`, now **4040/4040**
  assertions (up from 3607/3607 at the end of layer 5B/5A's own session;
  the delta is layer 5C's new/expanded modules).
- `test_bz_quest_wc3_render.c` gained two new tests for
  `bz_quest_wc3_model_anim_free()` (NULL-safety and real single-arena-
  allocation release), and the pre-existing 5A/5B cache-eviction and
  render-list tests continue to pass unchanged, confirming the animation
  cache's arena-ownership model does not regress the base model/texture
  GPU caches it shares eviction machinery with.
- A new structural test script,
  `platform/android/quest/scripts/test-wc3-bone-palette-layout.sh` (wired
  into `make test-quest-wc3-bone-palette-layout`, and into both the `test`
  and `quest` convenience targets in `build.mk`), greps the real
  `bz_quest_vk_wc3.c`/`.h` and `warcraft_vert.vert` source for: the
  device-limit-derived `paletteSlotStride` computation, the
  `BZ_QUEST_VK_WC3_PALETTE_SLOT_COUNT = ...MAX_SKINNED_DRAWS_PER_FRAME + 1`
  spare-slot constant (the same create-before-evict headroom pattern as
  `test-wc3-descriptor-pool-headroom.sh` guards for the model/texture
  caches), the exact bone-index (unnormalized `UINT`) and bone-weight
  (normalized `UNORM`) vertex attribute formats, the
  `UNIFORM_BUFFER_DYNAMIC` descriptor type used for the per-draw palette
  offset, and the anim-arena free-on-every-non-success-path contract in
  `model_ready_cb()`/`model_cache_destroy()`. This runs with no Vulkan
  device present, so it is a compile-/source-shape regression guard, not a
  substitute for on-device verification.
- The full Gradle/CMake `assembleDebug` build succeeds end to end
  (`JAVA_HOME` pointed at Temurin 17, NDK 27.2.12479018) with the modified
  `bz_quest_wc3_anim.c`/`.h`, `bz_quest_wc3_capture.c`/`.h`,
  `bz_quest_wc3_render.c`/`.h`, and `bz_quest_vk_wc3.c`/`.h`, and both
  shaders (`warcraft_vert.vert` gained the bone-index/weight inputs and
  palette UBO, `warcraft_frag.frag` gained team-color/glow blending)
  compiling to valid embedded SPIR-V.
- `scripts/verify-native-lib.sh` passes against that APK with **no change**
  to the allow-listed `DT_NEEDED` set — layer 5C introduced zero new
  shared-library dependencies (no new SDL2/desktop-GL/Apple-ObjC/VrApi
  dependency entered the APK from the new animation/skinning code).
- `make test-quest-source-sync`, `make test-quest-host-tests` (4040/4040,
  see above), `make test-quest-bridge` (67/67, unchanged), and the full
  repo-root `make test` all pass with no regressions (`421/421` in the
  final UI test gate, and every other suite reporting its own unchanged
  count — no `FAIL` anywhere in the full run).
- `git diff --check clancey-clancey-quest-renderer-terrain...HEAD` reports
  no whitespace errors.
- **Not verified this session** — identical caveats to layer 5A/5B above,
  plus the animation-specific gates listed in Layer 5C's own "Acceptance
  gates" subsection: no real retail MDX skeletal data, no physical Quest
  device, no on-device pose/team-color/geoset-alpha visual confirmation,
  and no profiling of `build_frame_dynamic_material()`'s per-frame CPU
  cost. Do not report any of these as confirmed until checked against real
  hardware and real staged `War3.mpq`/`War3x.mpq` data.

## Related documents

- [visionos-tabletop.md](visionos-tabletop.md) — the shared
  `platform/tabletop/` extraction this layer links unmodified, and the
  origin of the `"-data"`/`"+map"` engine startup argv convention this
  layer's `bz_quest_data_build_argv()` reuses
  (`platform/apple/visionos/tabletop/app/OpenRealmTabletopApp.swift`'s
  `LiveTabletopTransport`).
- `platform/tabletop/bridge/bz_tabletop_lifecycle.h` — the portable
  create/start/suspend/resume/stop/destroy engine-thread state machine
  `bz_quest_bridge.c` wraps unmodified.
- `platform/bridge/bz_tabletop_transport.h` — the immutable, ref-counted
  snapshot ABI (`bzTTSnapshot_t`, v3) `bz_quest_snapshot.c` reads via
  `BZ_TT_Latest()`/`BZ_TTSnapshot_*()`/`BZ_TTSnapshot_Release()`, consumed
  as-is with no ABI widening in this layer.
- `platform/bridge/bz_tabletop_assets.h` — the ref-counted, POD-array
  Warcraft III asset ABI (`bzTTAsset_t`, geosets/vertices/indices/textures/
  materials) `bz_quest_wc3_capture.c` reads via `BZ_TTA_Acquire()`/
  `BZ_TTA_Release()`. Layer 5A consumed the ABI as-is (v2, no widening —
  see "Layer 5A: static Warcraft III model rendering" above); layer 5C
  extended it in place to v3 to expose node/track/sequence/global-sequence/
  geoset-anim data — see "Layer 5C: Warcraft III model animation"'s "ABI
  decision" subsection above for the exact accessor list and why tunneling
  a Vulkan-specific/engine pointer through the ABI was rejected instead.
- `games/warcraft-3/visionos/wc3_mdx_decode.c` — the single shared MDX
  decoder (despite living under the `visionos/` directory, it is linked by
  both the visionOS and Quest builds — see `games/warcraft-3/game.mk`) that
  layer 5C updated to populate the new ABI v3 fields from real MDX chunk
  data. VisionOS's own Swift consumer
  (`platform/apple/visionos/tabletop/app/WarcraftAssetAdapter.swift`) was
  **not** changed this layer and does not read any of the new accessors —
  `bz_quest_wc3_capture.c` is the first and only real consumer of them;
  visionOS animation rendering remains the pre-existing non-functional stub
  it was before this layer, which is unchanged and out of this Quest-
  focused task's scope to fix.
- `WarcraftAssetAdapter.swift`, `WarcraftRenderDescriptors.swift`, and
  `WarcraftRenderMath.swift` (under
  `platform/apple/visionos/tabletop/app/`) — the reviewed visionOS
  reference implementation this layer's coordinate/scale conversion in
  `bz_quest_wc3_render.c` matches; exact file+line citations are given in
  "Layer 5A: static Warcraft III model rendering" above.
- Khronos OpenXR Android loader:
  <https://github.com/KhronosGroup/OpenXR-SDK-Source> (`src/tests/hello_xr`
  for the reference loader-init/instance/session/Vulkan-interop sequence),
  Maven Central artifact `org.khronos.openxr:openxr_loader_for_android`.
- OpenXR 1.0 specification: <https://registry.khronos.org/OpenXR/specs/1.0/html/xrspec.html>
  (`XR_KHR_vulkan_enable2`, `XR_FB_passthrough`, environment blend modes,
  composition layer ordering, `xrLocateViews`/`XrViewState` sections cited
  above).
- Vulkan 1.3 specification: <https://registry.khronos.org/vulkan/specs/1.3/html/vkspec.html>
  ("Required Format Support" table, cited above for the depth-format
  quirk).
- Meta Quest native manifest requirements:
  <https://developers.meta.com/horizon/documentation/native/android/mobile-native-manifest/>
- Meta's `XR_FB_passthrough` sample/guidance (start/pause on app
  resume/pause pattern):
  <https://developers.meta.com/horizon/documentation/native/android/mobile-passthrough/>

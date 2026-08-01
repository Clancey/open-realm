# Meta Quest (Android/NDK + OpenXR) tabletop shell — Layers 5A/5B/5C/5D/5E/6/7/8

This document tracks layers 5A/5B/5C/5D/5E/6/7/8 of a stacked Meta Quest port:

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
- **Layer 5C (`clancey-quest-model-animation`)**: adds
  authoritative Warcraft III **model animation** on top of 5A's static
  models — skeletal/vertex hierarchy, sequence/global-sequence pose
  sampling, GPU (vertex-shader) skinning, and the dynamic material state
  (geoset alpha, team color/glow layer selection) an animated unit/building/
  doodad needs. See
  "[Layer 5C: Warcraft III model animation](#layer-5c-warcraft-iii-model-animation-bz_quest_wc3_anim-bz_quest_wc3_capturec-bz_quest_vk_wc3c)"
  below for full scope, ABI decision, ownership, and evidence.
- **Layer 5D (`clancey-quest-fog-selection-overlay`)**: adds
  authoritative Warcraft III **fog-of-war visibility/exploration** compositing
  plus **selection marker overlays** on top of 5B/5C's terrain+model pass,
  reusing only snapshot state already present in `bz_tabletop_transport.h` and
  asset metadata already present in `bz_tabletop_assets.h`. See
  "[Layer 5D: Warcraft III fog-of-war + selection overlays](#layer-5d-warcraft-iii-fog-of-war--selection-overlays-bz_quest_wc3_fogh-bz_quest_vk_wc3_fogc)"
  below for full scope, ownership, and evidence.
- **Layer 5E (this layer, `clancey-quest-hud-command-card-5e`)**: adds
  authoritative Warcraft III **status/command-card HUD** presentation on top
  of 5D's terrain+model+fog+selection pass: a pure, host-testable layout/
  hit-region contract (`bz_quest_wc3_hud.h`/`.c`) built from the same
  `bzTTActionLayout_t`/`bzTTPlayer_t` transport fields the command-card/
  status UI has always exposed, a project-owned deterministic bitmap-font
  atlas (`bz_quest_wc3_hud_font.h`/`.c`), and Quest-local Vulkan GPU ownership
  for the two small pipelines that render it
  (`bz_quest_vk_wc3_hud.h`/`.c`). Presentation only - see
  "[Layer 5E: Warcraft III status/command-card HUD](#layer-5e-warcraft-iii-statuscommand-card-hud-bz_quest_wc3_hudh-bz_quest_vk_wc3_hudc)"
  below for full scope, ownership, and evidence.
- **Layer 6 (this layer, `clancey-quest-touch-controls`)**: adds the first
  playable **Meta Quest Touch-controller input** layer on top of 5A-5E: an
  OpenXR action set with aim/grip poses, trigger/squeeze/thumbstick/face-button
  actions and per-hand haptics (`bz_quest_xr_actions.h`/`.c`, with the pure
  binding tables in `bz_quest_xr_bindings.h`/`.c`), a pure host-testable
  interaction state machine + deterministic ray-hit priority + board
  pan/rotate/zoom transform + typed-command mapping
  (`bz_quest_input_state.h`/`.c`), Quest-local Vulkan ray/reticle rendering
  (`bz_quest_vk_wc3_pointer.h`/`.c`, reusing the layer-5D marker shaders), and
  a per-frame interaction capture (`bz_quest_wc3_capture_interaction()`). Every
  gameplay command is posted **only** through the existing typed
  `BZ_TT_Post*` transport - never by mutating local state - see
  "[Layer 6: Meta Quest Touch controller input](#layer-6-meta-quest-touch-controller-input-bz_quest_xr_actionsc-bz_quest_input_statec-bz_quest_vk_wc3_pointerc)"
  below for full scope, the action map, the state machine, the ray-hit
  priority list, the command-mapping table (incl. the documented target-entity
  ABI gap), and the board-transform clamp ranges.
- **Layer 7 (`clancey-quest-data-staging-audio`)**: adds the
  reproducible developer workflow that stages a user's own local Warcraft
  III ROC/TFT archives onto the device via `run-as`
  (`platform/android/quest/scripts/stage-wc3-data.sh`), and a real Android
  **AAudio** output sink (`bz_quest_audio.h`/`.c`) that dequeues from the
  existing bounded `BZ_TTAudio_*` queue, parses only explicitly supported
  RIFF/WAVE PCM layouts (`bz_quest_wav.h`/`.c`), converts/mixes a bounded
  set of concurrent voices off the real-time thread
  (`bz_quest_audio_mixer.h`/`.c`), and drives stream lifecycle in step with
  Android/OpenXR app lifecycle (`bz_quest_audio_lifecycle.h`/`.c`). Neither
  piece changes any existing shared bridge/transport/asset ABI. See
  "[Layer 7: Warcraft III data staging + native AAudio output](#layer-7-warcraft-iii-data-staging--native-aaudio-output)"
  below for full scope, the staging safety/idempotency design, the ROC/TFT
  archive rules (and the one documented runtime `-tft` gap), and the AAudio
  parser/mixer/lifecycle design.
- **Layer 8 (this layer, `clancey-add-quest-hand-tracking`)**: adds
  native Meta Quest **hand-tracking** input parity alongside (never
  replacing) layer 6's Touch controllers: optional, explicitly-negotiated
  `XR_EXT_hand_tracking` + `XR_FB_hand_tracking_aim` OpenXR capability
  (`bz_quest_xr.h`/`.c`'s `bzQuestXrHandCapability_t`), one `XrHandTrackerEXT`
  per hand with strict session-scoped lifecycle (`bz_quest_xr_hands.h`/`.c`),
  a pure hand-gesture builder that turns raw joints or Meta's standardized
  aim/pinch state into the SAME per-hand sample type the Touch path already
  produces (`bz_quest_hand_input.h`/`.c`), and deterministic,
  hysteresis-debounced controller-vs-hand source arbitration folded directly
  into the existing layer-6 state machine (`bz_quest_input_state.h`/`.c`'s
  `bz_quest_input_arbitrate_source()`). No parallel command mapper, no hand
  mesh rendering, no widened bridge/transport ABI. See
  "[Layer 8: Meta Quest hand tracking](#layer-8-meta-quest-hand-tracking-bz_quest_xr_handsc-bz_quest_hand_inputc-bz_quest_input_statec)"
  below for the exact extension matrix, capability-negotiation contract,
  gesture/arbitration state machine, and the honest Quest-vs-visionOS
  differences.

**These layers now render fog of war, per-entity selection markers, and a
status/command-card HUD, (layer 6) read Touch controllers to select
units, activate command-card buttons, issue smart/target orders, cancel, and
pan/rotate/zoom the board, (layer 7) can stage a developer's own local
Warcraft III data onto the device and play its non-spatial audio through
AAudio, and (layer 8) can read Quest hand tracking as a second, arbitrated
input source for the exact same select/smart/cancel/board-pan commands when
Touch controllers are unsupported/inactive — all posted through the
authoritative typed tabletop transport, or (audio) dequeued from its
existing bounded audio queue. They still do not render particles/effects,
do not render a hand mesh, and do not implement multiplayer, Store asset
delivery, the Meta Audio SDK, or Platform services.**
See
[Current limitations](#current-limitations) and
`bz_quest_host.c`'s compile-time seams (`BZ_QUEST_ENABLE_*`, each guarded by
a `#error` until its real implementation lands) — `BZ_QUEST_ENABLE_ENGINE_START`
and `BZ_QUEST_ENABLE_BRIDGE_SNAPSHOTS` were layer 4's two seams;
`BZ_QUEST_ENABLE_WC3_RENDERER` is the one seam *layer 5A* replaces with a
real implementation (layer 5E's HUD renders under this same seam - it adds no
new one); `BZ_QUEST_ENABLE_INPUT` is the seam *layer 6* replaces with the real
OpenXR Touch-controller input layer; `BZ_QUEST_ENABLE_AUDIO` is the seam
*layer 7* replaces with the real AAudio sink (now hard-required at `1`, an
`#error` if ever turned off); `BZ_QUEST_ENABLE_DATA_STAGING` remains
permanently `#error`-gated at its default `0` — data staging is an external
ADB dev workflow, not an in-app runtime feature, so no real "on" value for
this seam will ever exist. **Layer 8 adds no new compile-time seam at all**
— hand-tracking capability is negotiated at OpenXR runtime (instance-
extension enumeration + system-property query), never at compile time, so
`bz_quest_xr_hands_create()`/`_sync()` are always built and simply no-op into
"Touch-only" on a runtime/device/account without the capability - see
"[Layer 8](#layer-8-meta-quest-hand-tracking-bz_quest_xr_handsc-bz_quest_hand_inputc-bz_quest_input_statec)"'s
"Capability negotiation" below.


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
      bz_quest_vk_wc3_fog.h/.c  # Vulkan fog-mask image + selection-marker
                                 # mesh/pipelines/descriptors, owned once per
                                 # renderer instance
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
    test-wc3-fog-selection-layout.sh # structural layer-5D shader/layout/order check
    build-shaders.sh             # GLSL -> SPIR-V -> embedded-C-header pipeline
    bin2c.c                      # host tool: SPIR-V binary -> aligned C uint32_t array
  tests/
    test_bz_quest_pure.c         # bz_quest_pure.c unit tests (host-buildable)
    test_bz_quest_scene.c        # bz_quest_scene.c unit tests (host-buildable)
    test_bz_quest_data.c         # bz_quest_data.c unit tests (layer 4, host-buildable)
    test_bz_quest_frame.c        # bz_quest_frame.c unit tests (layer 4, host-buildable)
    test_bz_quest_wc3_fog.c      # layer-5D pure fog/selection math tests
    test_bz_quest_pure_main.c    # runs the pure Quest host-test suites, wired into `make test`
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
# frame descriptor + throttled-log decision), PLUS the layer-5A/5B/5C/5D
# pure Warcraft helpers (render/world-matrix, generic cache, terrain math,
# animation sampling, and fog packing/cell/marker math). Runs as part of
# `make test`.
make test-quest-host-tests

# Layer 5D structural grep test for the fog/selection shader interface,
# one-byte fog image format, explicit row-length upload path, and shared
# eye-pass draw ordering. Runs as part of `make test` and `make quest`.
make test-quest-wc3-fog-selection-layout

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

# --- Layer 7: data staging + audio -----------------------------------------

# Fake-adb/run-as/pm/df host harness (no NDK/Gradle/device required) for the
# developer data-staging script below. Runs as part of `make test`.
make test-quest-stage-wc3-data

# Structural (no-NDK/no-device) check that the AAudio real-time data
# callback and the mixer render function it calls never allocate, lock,
# log, or touch files/bridge APIs. Runs as part of `make test`.
make test-quest-audio-rt-callback-safety

# Stages a developer's own local, user-owned Warcraft III ROC (and,
# optionally, TFT-over-ROC) data directory onto a connected/sideloaded
# Quest device via the app's own run-as identity (works under Android
# scoped storage - no root, no shell access to another app's private
# storage). Requires the debug APK already installed
# (`make quest-install-debug`) and a real adb-visible device. Never
# bundles/copies any Warcraft III data into git or the APK.
make quest-stage-wc3-data BZ_QUEST_WC3_DATA=/path/to/your/wc3/data

# Reports what is currently staged on the device (files, sizes, sha256, and
# the override file's contents) without transferring anything.
make quest-verify-wc3-data

# Removes only the app's private staged Warcraft III data subdirectory and
# override file (never a broader/recursive delete).
make quest-clean-wc3-data

# Launches the installed app, and tails its logcat tag, on the connected/
# sideloaded device.
make quest-run
make quest-log
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
| `bz_quest_pure.c` | `tests/test_bz_quest_pure.c` | Projection/view-matrix math, format/extension/passthrough-capability selection (unchanged from layer 3), plus (layer 8) `bz_quest_quat_forward()` - the shared aim-ray quaternion rotation controllers and hand tracking both use, cross-checked against `bz_quest_pose_to_view_matrix()`'s own rotation matrix. |
| `bz_quest_scene.c` | `tests/test_bz_quest_scene.c` | Procedural test-scene generator (unchanged from layer 3). |
| `bz_quest_data.c` | `tests/test_bz_quest_data.c` | Default-dir construction, override read/validate (normal + every documented rejection: relative path, disallowed characters, oversized, empty), full resolve fallback order, edition detection (`bz_quest_data_detect_edition()`: ROC-only, TFT-over-ROC, case-insensitive `War3x` match, non-`.mpq` false positives ignored, missing-directory/NULL-arg rejection), and argv construction (normal + `-tft` emission/ordering + undersized-buffer/NULL-arg error paths) — 32 tests, pure, real temp dirs via `mkdtemp()`. |
| `bz_quest_frame.c` | `tests/test_bz_quest_frame.c` | Reset value, `bz_quest_frame_from_values()` field copy/truncation/ABI-mismatch detection, and every `bz_quest_frame_should_log()` cache-hit/cache-miss branch (identical frame never logs; status/lifecycle-state/lifecycle-error changes each log; a bare `generation` advance — in *any* status, including `OK` — never logs, including across ~190 simulated consecutive engine frames, guarding against the per-frame-log regression fixed in PR #19's review pass) — 15 tests, pure, no I/O. |
| `bz_quest_wc3_fog.c` | `tests/test_bz_quest_wc3_fog.c` | Three-state fog classification, row-major cell indexing, world<->cell conversion (including rectangular/non-chunk-multiple grids), 0/128/255 texture packing with and without row padding, content-based dirty-check hit/miss, per-axis selection-marker matrix/tint generation (with and without the shared world transform), independent per-axis nonpositive-scale rejection, and zero-dimension rejection paths. |
| `bz_quest_wc3_render.c` (world transform) | `tests/test_bz_quest_wc3_world_transform.c` | Cross-subsystem integration for the shared world/tabletop transform: entity-at-terrain-corner/center coordinate match against `bz_quest_wc3_terrain_build_chunk()`'s real vertex output, fog-cell/entity raw-space agreement, selection-marker/entity translation+scale sharing, rectangular bounds + nonzero origin, map-reload producing an independent transform, and a guard against double-applying the transform. |
| `bz_quest_bridge.c` | `tests/test_bz_quest_bridge.c` | Valid-override start reaches `RUNNING`; missing-data start reaches `FAILED` with the engine's own error surfaced; invalid-override start reaches `FAILED` *before* any `bzTabletopLifecycle_t` exists; a second `start()` on an already-attempted instance is rejected; suspend/resume forward correctly (and are safe no-ops before a start or after a stop); `stop()` is idempotent and safe pre-start; `destroy()` then a fresh `start()` on the same storage succeeds; `is_terminal()` is correct for every bridge state — 10 tests, **linking the real** `bz_tabletop_lifecycle.c`/`common/bz_runtime.c` (not a stub), 67 assertions. |
| `bz_quest_input_state.c` (layer 6, extended layer 8) | `tests/test_bz_quest_input_state.c` | The pure Touch-controller interaction state machine: button edge detection (idle→press→hold→release→idle produces exactly one edge; two cycles produce two; a controller going inactive mid-hold clears the latch without firing a phantom release), deterministic ray-hit priority (HUD action > HUD cancel > HUD disabled/stale/hidden consumed-but-no-command > nearest-entity sphere > terrain plane > no-hit; boundary hits exactly on a region edge; entity-vs-terrain precedence), world↔composed coordinate inversion round-trip (forward `bz_quest_wc3_world_transform_point` then the new inverse, plus the degenerate no-bounds passthrough), board transform compose/inverse round-trip and scale/translate/pitch clamping at both ends, pan/rotate/zoom math, target/cancel transitions incl. the `ENTITY`/`ENTITY_OR_POINT` ABI-gap decision (target-point at the entity's ground origin), command-mapping-table completeness (every command type reachable, queue-full/stale-generation rejection paths surface a rejected haptic and leave no latched state), and idempotent transient-clear on focus-loss/controller-loss/generation-bump/map-epoch-change (clears once on entry, never re-fires while the condition persists); **layer 8 adds** `bz_quest_input_arbitrate_source()`'s controller-vs-hand state machine (instant reclaim, debounced handoff with real per-frame `dt` accumulation, no-handoff/fallback/no-redebounce paths) plus full `bz_quest_input_state_update()` integration coverage (hand-sourced select/smart-entity/board-pan, controller-preferred-while-active, exactly-once clear-on-switch with the precise float-accumulation boundary verified, fresh-edge-then-no-repeat across a switch, board-drag dropped by a switch, hands unable to rotate/zoom/height the board, cancel reachable via a hand pinch without a secondary button, and an explicit controller-only regression guard) - 16 new tests, every pre-existing layer 6/7 test in this file passes completely unmodified. |
| `bz_quest_hand_input.c` (layer 8) | `tests/test_bz_quest_hand_input.c` | The pure hand-tracking gesture builder: both hands independently; capability NONE/EXT_ONLY/FB_AIM; tracker active/inactive; each of the four tracked joints individually valid/invalid; a degenerate (zero-length) ray-direction rejection; the FB-aim tier trusting the runtime's index/middle `*Pinching` status bits directly (never re-deriving from strength), aim-invalid producing no output, aim pose passthrough; the EXT-only tier's joint-position-only ray basis, pinch engage/release/no-chatter hysteresis at and around both thresholds, thumb-validity gating, and `squeezeDown` always false; tracker loss/reacquisition requiring a fresh ENGAGE crossing (never resuming from mid-hysteresis); and the intentionally-always-zero unsupported fields (`primaryDown`/`secondaryDown`/`thumbstick`) - 22 tests. |
| `bz_quest_xr_bindings.c` (layer 6) | `tests/test_bz_quest_xr_bindings.c` | The pure OpenXR binding tables: both profile path strings are legal OpenXR paths; every action/component path is lowercase-legal (no spaces/uppercase, no leading/trailing/double slash); `simple_controller` is a strict subset of `touch_controller` (same aim/grip/select/menu/haptic semantic actions bound, thumbstick/squeeze/face-buttons only on Touch); the reserved Oculus/system button is never bound; menu is left-hand-only; face buttons differ by hand (X/Y left, A/B right); no duplicate component path per side; and the path validator rejects malformed inputs. |
| `bz_quest_wav.c` (layer 7) | `tests/test_bz_quest_wav.c` | The pure RIFF/WAVE parser: valid mono/stereo, 8-bit/16-bit PCM parses; every documented rejection (bad RIFF/WAVE magic, missing `fmt `/`data` chunk, `data` before `fmt `, non-PCM `audioFormat`, unsupported channel count/bit depth, `blockAlign`/`byteRate` inconsistent with the header's own fields, zero/oversized/out-of-bounds/misaligned `data` chunk size, truncated chunk header); odd-sized-chunk padding is honored when walking past a non-`fmt `/`data` chunk — 19 tests. |
| `bz_quest_audio_mixer.c` (layer 7) | `tests/test_bz_quest_audio_mixer.c` | The bounded RT-safe voice pool + format conversion: `init` zeroes every slot; `convert` widens 8-bit to 16-bit, duplicates mono to stereo, resamples up/down deterministically (exact expected sample values, not just "some output"), rejects a zero target rate and an oversized computed output-frame count without allocating; `submit`/`render`/`reap` full lifecycle (submit into the first free slot, render mixes/clamps/advances cursor, a voice reaching its end goes inactive mid-`render` — not a separate step — and is only freed by a later `reap`); pool-full rejection when all `BZ_QUEST_AUDIO_MAX_VOICES` slots are active; two overlapping voices mix (sum, not overwrite) and clip-saturate at the int16 boundary instead of wrapping — 14 tests. |
| `bz_quest_audio_lifecycle.c` (layer 7) | `tests/test_bz_quest_audio_lifecycle.c` | The pure AAudio lifecycle state machine: every legal `can_start`/`can_pause`/`can_resume`/`can_stop` transition and its inverse (illegal transitions from every other state); `FAILED` is reachable from a failed start attempt and itself allows a fresh `can_start` retry (never auto-retried by this module); `should_restart` is true only when a disconnect was flagged while `RUNNING`/`PAUSED` (never while `STOPPED`/`FAILED`); a successful restart always lands in `RUNNING` regardless of the pre-disconnect state; a failed restart lands in `FAILED`; `counter_changed` fires exactly once per distinct value (including a decrease) and never on a repeat — 14 tests. |

`bz_quest_wc3_render.c`'s world-transform integration test
(`tests/test_bz_quest_wc3_world_transform.c`) was also extended in layer 6
to cover `bz_quest_wc3_world_transform_point_inverse()`'s exact round-trip
against the forward transform (see [Layer 6](#layer-6-meta-quest-touch-controller-input-bz_quest_xr_actionsc-bz_quest_input_statec-bz_quest_vk_wc3_pointerc)).

`bz_quest_bridge.c`'s tests are the one suite in this table that needs
`make test-assets` (to produce a synthetic `build/tests/tests.mpq` data
directory) and the `shared`/`sheet` dynamic libs — wired automatically as
target dependencies of `make test-quest-bridge` (see "Build/run/log
commands" above), exactly mirroring `games/warcraft-3/game.mk`'s existing
`test-bz-tabletop-lifecycle` recipe.

Two more layer-7 checks are their own shell-script targets, like
`test-quest-wc3-fog-selection-layout` above, rather than rows in the pure
host-test binary table:

- `make test-quest-stage-wc3-data`
  (`scripts/test-stage-wc3-data.sh`) — a fake `adb`/`run-as`/`pm`/`df`/
  `sha256sum`/`cp` filesystem harness for
  `scripts/stage-wc3-data.sh` (no real device/NDK/Gradle needed): 20 cases
  covering ROC-only, TFT-over-ROC, incomplete-mixed-layout rejection,
  TFT-without-ROC rejection, filenames with spaces/mixed case preserved
  exactly, unchanged-file skip, changed-file atomic replace (temp name +
  rename, no leftovers), device-side corruption detected (prior valid file
  left intact), an interrupted transfer leaving the prior valid file
  intact, no-device/wrong-package/non-debuggable-app/insufficient-space
  all rejected with an actionable message, a near-full device rejected via
  the transient bounce+stage peak estimate (not just final size) with the
  prior file preserved, an idempotent re-stage never blocked by that same
  peak estimate on a near-full device, multiple attached devices without
  `--serial` rejected, `--serial` routing every adb invocation to the
  correct device (checked via an invocation log), an unknown `--serial`
  rejected, an offline `--serial` device rejected, and `clean` refusing
  without `--yes` then safely removing only the resolved app-data
  subdirectory.
- `make test-quest-audio-rt-callback-safety`
  (`scripts/test-quest-audio-rt-callback-safety.sh`) — greps the real
  `bz_quest_audio_data_callback()`/`bz_quest_audio_mixer_render()` function
  bodies (not the whole file, which legitimately allocates/logs elsewhere
  on the control thread) for any `malloc`/`calloc`/`realloc`/`free`/
  `pthread_mutex_`/log/`fopen`/`fread`/bridge (`BZ_TT_`) call, and confirms
  `bz_quest_audio_data_callback` is registered exactly once as the AAudio
  data callback — a future edit that reintroduces a forbidden call into
  the real-time path fails loudly here instead of only as an on-device
  glitch/dropout.

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
`adb push` before launch, or — as of layer 7 — written automatically by
`platform/android/quest/scripts/stage-wc3-data.sh stage <dir>`, see
[Layer 7](#layer-7-warcraft-iii-data-staging--native-aaudio-output) below).
This is **not** a silent secondary search path:

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
`["openwarcraft3-quest", "-data", "<dir>"[, "-tft"][, "+map", "<name>"]]` —
reusing the exact `"-data"`/`"+map"` convention
`platform/apple/visionos/tabletop/app/OpenRealmTabletopApp.swift`'s
`LiveTabletopTransport` already establishes for this same lifecycle core
(see `common/bz_runtime.h`'s `bzRuntimeArgs_t` and AGENTS.md's "Command
Conventions": `+` is a process/startup argument here, never an in-engine
console command) — no second startup-argument scheme was invented for this
platform. `mapName` is always `NULL` in this layer (see "Current
limitations" below); the 3- or 4-argument form (`-data` [+ `-tft`]) is what
actually runs. The whole call uses caller-provided fixed-size storage — no
heap allocation; `BZ_QUEST_DATA_ARGV_MAX` was raised from 5 to 6 to make
room for `-tft` alongside the existing `"+map" "<name>"` pair.

**Edition detection and the `-tft` dash-flag (layer 7 fix).**
`bz_quest_data_detect_edition()` re-scans the already-`bz_quest_data_resolve()`d
directory using the *exact same* signal `FS_AddArchiveScanEntry()`
(`common/common.c`) uses to decide whether to skip an archive: a `.mpq`
extension and a case-insensitive `"War3x"` 5-character basename prefix. If
any such file is present the edition is `BZ_QUEST_DATA_EDITION_TFT`;
otherwise `BZ_QUEST_DATA_EDITION_ROC`. `bz_quest_bridge_start()` calls this
right after a successful resolve, defaults to ROC on any detection failure
(never silently promotes to TFT), and threads the result into
`bz_quest_data_build_argv()`, which appends the bare dash-flag `"-tft"`
**before** `"+map"` when (and only when) the edition is TFT.

This must be a plain argv dash-flag, never a late `Cbuf_AddText("fs_expansion
1")`/`+fs_expansion 1` queued command: `Cvar_ApplyCommandLine()` (which
parses `-tft`/`-roc` into `fs_expansion`) runs inside `Com_Init()`, which
completes **before** `BZ_RuntimeInit()` ever calls `FS_AddDataDirectory()` —
by the time any queued `Cbuf` command would run, the archive scan (and its
skip-or-mount decision) has already happened. A late toggle would only ever
affect the *next* `FS_AddDataDirectory()` call, and this bridge makes
exactly one.

**Expected log evidence.** `bz_quest_bridge_start()` logs
`bz_quest_bridge_start: resolved data dir '<dir>', edition=roc` or
`edition=tft` right after detection, before argv construction; when TFT is
detected, the engine's own `common/common.c` archive-mount log
(`Added archive '<dir>/War3x.mpq'` — printed once per file, in the
alphabetical mount-order described below) confirms the flag actually took
effect, not just that the cvar was set. `test_bz_quest_bridge.c`'s
`test_tft_over_roc_data_enables_and_mounts_expansion` and
`test_case_insensitive_war3x_name_still_mounts_via_bridge` assert
`Cvar_Integer("fs_expansion", -1) == 1` **and** `FS_FileExists()` on a
marker file packed only inside the synthetic `War3x.mpq` fixture, proving
genuine mounting rather than cvar-state alone;
`test_roc_only_data_leaves_expansion_disabled` asserts the inverse
(`fs_expansion == 0`, marker file absent) for a ROC-only directory.

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

## Layer 5D: Warcraft III fog-of-war + selection overlays (`bz_quest_wc3_fog.h`/`bz_quest_vk_wc3_fog.c`)

Layer 5D adds only two presentation features on top of 5B/5C's existing
terrain+model renderer: authoritative fog-of-war visibility/exploration
compositing, and authoritative per-entity selection markers. It does **not**
add particles/effects, command-card/HUD surfaces, gameplay/controller input,
audio, data staging, hand tracking, billboarding, TXAN, or KMTF.

### Authoritative fog/selection state flow

1. `bz_quest_wc3_capture.c`'s new `bz_quest_wc3_capture_fog()` does its own
   independent `BZ_TT_Latest()` / `BZ_TTSnapshotRelease()` retain/copy/release
   cycle, matching `bz_quest_wc3_capture_frame()`'s pre-existing "each call site
   owns one snapshot copy" rule rather than threading a borrowed snapshot
   pointer across subsystems.
2. From that snapshot, it reads only already-existing transport fields:
   `BZ_TTSnapshot_FogDimensions()`, `BZ_TTSnapshot_FogVisible()`,
   `BZ_TTSnapshot_FogExplored()`, `BZ_TTSnapshot_MapBounds()`, and
   `BZ_TTSnapshot_Player(0)`'s `target` mode. No transport ABI field or version
   changed for this layer.
3. Per-entity selection/tint data stays in the existing frame-capture path:
   `bzTTEntity_t.selected` is copied into the Quest-local
   `bzQuestWc3EntityInput_t` / `bzQuestWc3RenderItem_t`, alongside
   `bzTTAssetMetadata_t.tint_r/g/b/a` from
   `BZ_TTA_ResolveEntityMetadata()`. `bzTTEntity_t.radius` is **not** copied or
   used anywhere in this layer (see "Selection overlay design" below for why
   and what replaces it). No hardcoded team-color table exists on the Quest
   side; the asset ABI's resolved tint stays authoritative.
4. `bz_quest_wc3_fog.c` is the pure, host-testable layer. It classifies each
   cell into exactly the desktop client's existing three-state model:
   VISIBLE (`visible!=0`), EXPLORED_NOT_VISIBLE (`visible==0 && explored!=0`),
   or UNSEEN (`visible==0 && explored==0`), and packs those states to the same
   `client/cl_parse.c` byte convention the desktop client already uses:
   `255`, `128`, and `0` respectively.
5. The same pure module owns cell/world conversion using the Warcraft III game
   constant `FOW_CELL_SIZE = TILE_SIZE / FOW_CELLS_PER_TILE_SIDE = 64.0f`
   (`common/common.h`, `games/warcraft-3/common/mapinfo.h`). Cell centers map
   to `bounds.min + (cell + 0.5) * 64`, matching `games/warcraft-3/game/g_fow.c`'s
   floor-based world->cell rule.
6. `bz_quest_vk_wc3_fog.c` owns the one persistent GPU fog image, the one
   persistent procedural marker mesh, and the fog/marker pipelines. It calls
   `bz_quest_wc3_capture_fog()` once per renderable frame, recreates the fog
   image only when dimensions change, and re-uploads pixel data only when a
   content compare against its own last-uploaded bytes says the packed fog mask
   actually changed. `BZ_TTSnapshot_Generation()` is intentionally **not** used
   as the upload gate because the transport increments it every client frame,
   even when fog bytes are identical.

### Shared world/tabletop position transform (fixes the inherited coordinate mismatch)

An earlier version of this layer left fog/entity/marker positions in raw
engine world units while terrain used a separately-scaled, centered "diorama"
box, and documented the resulting mismatch as an accepted limitation. That was
rejected in review: **the renderer cannot spatially align terrain, entities,
fog, and selection markers unless they share one transform**, and
documentation is not a substitute for actually applying it. This is now fixed
with one shared, Quest-owned transform applied exactly once to every
world-space position:

- `bzQuestWc3WorldTransform_t` (`bz_quest_wc3_render.h`) holds `{scale,
  centerX, centerZ}`. `bz_quest_wc3_world_transform_measure(minX, minZ, maxX,
  maxZ, &out)` derives it from the authoritative map bounds using the exact
  same formula `bz_quest_wc3_terrain_measure()` has always used for terrain
  (`scale = 1.08 / max(spanX, spanZ)`, `center = bounds midpoint` per axis) -
  `bz_quest_wc3_terrain_measure()` now calls this shared function internally
  instead of duplicating the `1.08` literal, so terrain and every other
  consumer are provably driven by the same formula, not two copies of it.
  `bz_quest_wc3_world_transform_point(transform, x, y, z, out)` applies it:
  `out = ((x-centerX)*scale, y*scale, (z-centerZ)*scale)` - height is scaled
  but never re-centered, matching the reviewed visionOS reference
  (`WarcraftWorldTransform` in `WarcraftAssetAdapter.swift`). A `NULL`
  transform is raw passthrough (no valid map bounds this frame), never a
  fabricated identity scale.
- `bz_quest_wc3_capture.c` computes this transform **once per frame** from
  `BZ_TTSnapshot_MapBounds()` and threads it into both
  `bz_quest_wc3_build_render_list()` (entity translation) and
  `bz_quest_wc3_capture_fog()`'s stored `bzQuestWc3FogCapture_t.transform`
  (fog GPU placement). Missing/degenerate bounds log once
  (`entity-transform-bounds-missing` / `fog-bounds-degenerate`) and fall back
  to `NULL`/failed capture respectively - never a silent guess.
- `bz_quest_wc3_build_world_matrix()` applies the transform to the entity's
  translation **only** (`outWorld[12/13/14]`); the model-local rotation/scale
  block (`outWorld[0..10]`, built from
  `bz_quest_wc3_entity_footprint_scale()`) is completely unaffected - model
  geometry scale and world-position scaling are two separate concerns and
  must never be mixed into one number.
- The Vulkan fog vertex shader (`warcraft_fog_vert.vert`) receives the same
  `{centerX, centerZ, scale}` via a push-constant `vec4 transform` and applies
  it only to the position fed into `gl_Position`; the fragment shader's fog
  cell-index math (`fragWorld`) intentionally stays in raw world space
  end-to-end, matching `bzQuestWc3FogBounds_t`'s own raw-unit convention - the
  transform only needs to touch on-screen placement, not fog-cell math.
- Selection markers get the same treatment: `bz_quest_vk_wc3_fog.c`'s
  production path reuses the render item's already-transformed
  `item->world[12/13/14]` directly (no second application), and the pure
  `bz_quest_wc3_selection_marker_from_entity()` test helper mirrors
  `bz_quest_wc3_build_world_matrix()`'s swap+transform exactly for host
  testing.
- Cross-subsystem integration tests in
  `platform/android/quest/tests/test_bz_quest_wc3_world_transform.c` prove an
  entity placed at a known terrain corner/center lands on the exact same
  on-screen position `bz_quest_wc3_terrain_build_chunk()` produces for that
  corner, that a fog cell and an entity agree on the same raw-space meaning
  of a world coordinate, that a selection marker shares the model's own
  translation and per-axis scale, that rectangular bounds and nonzero map
  origins work, that reloading the map with new bounds produces an
  independent transform (no stale/global state), and that applying the
  transform twice would produce a visibly different (wrong) result - guarding
  against a future double-conversion regression.

### GPU representation and draw order

- Fog uses one `VK_FORMAT_R8_UNORM` sampled 2D image, not RGBA8: the source
  data is already exactly one desktop-parity byte per cell (`0/128/255`), so a
  four-channel texture would waste 3 bytes/texel and add no information.
- Upload goes through one persistent host-visible staging buffer plus
  `VkBufferImageCopy.bufferRowLength`, so padded-row uploads stay correct when
  the image width is not already naturally aligned.
- The shared eye render pass order is now: terrain opaque -> model opaque ->
  terrain blended -> model blended -> fog overlay -> selection markers.
  Fog is recorded **after** both blended passes so water and transparent
  doodads are composited before fog darkens the final opaque+blended result -
  recording it earlier (as an initial version of this layer did) let
  transparent content draw fully visible on top of the fog mask afterward,
  silently un-fogging it. Selection markers draw last so they remain
  readable, but their pipeline still depth-tests against the existing opaque
  depth buffer and uses a tiny world-space lift epsilon (scaled by the shared
  transform's `scale`, so it stays a consistent fraction of world size instead
  of becoming oversized once positions are diorama-scaled) to avoid floor
  z-fight.
- When `BZ_TTSnapshot_FogDimensions()` reports false, the fog image is torn
  down and no overlay draws that frame; the renderer never leaves a stale
  "last map's fog" texture bound after unload/reset.

### Selection overlay design

Markers are Quest-owned procedural annulus geometry (32 segments, fixed inner
radius ratio) built once at renderer init, then transformed per selected
entity. The pure helper builds the marker's world matrix from the render
item's already-computed per-axis `footprintScaleX/Y/Z`
(`bz_quest_wc3_entity_footprint_scale()`) - the **same** category/footprint
formula `bz_quest_wc3_build_world_matrix()` uses for the selected entity's own
mesh scale - and its already-transformed `world[12/13/14]` translation, not
`bzTTEntity_t.radius`. An earlier version of this layer sized markers directly
from `radius`, but that field is tens of raw WC3 world units while the model
itself is placed with the compressed diorama footprint scale, so a
radius-sized ring never matched the selected model's own footprint; `radius`
has no other proven role and has been removed from the Quest-local entity/
render-item structs entirely. Because the scale is per-axis (not a single
uniform radius), a rectangular building's marker stays rectangular and
correctly oriented, matching its footprint. The marker shader is flat-tinted
from the per-entity `tint_r/g/b/a` metadata.

### Supported vs. unsupported fog/selection behavior

| Behavior | Status |
|---|---|
| Visibility/exploration compositing from `BZ_TTSnapshot_FogVisible()` + `FogExplored()` | Supported |
| Desktop-parity three-state bytes (`255` visible, `128` explored-not-visible, `0` unseen) | Supported |
| Rectangular/non-square fog grids and padded-row GPU uploads | Supported |
| Content-based dirty check (identical fog bytes do not re-upload) | Supported |
| Fog recorded after opaque+blended terrain/model passes so transparent content is correctly darkened | Supported |
| Per-entity selection rings from `bzTTEntity_t.selected`, sized from the same footprint/category scale the model uses (not raw `radius`) | Supported |
| Rectangular building footprints produce correctly oriented, non-square selection markers | Supported |
| Per-entity tint from `bzTTAssetMetadata_t.tint_*` | Supported |
| Shared world/tabletop position transform applied once to terrain, entity translations, fog bounds/cells, and selection markers | Supported |
| Player `target` mode rendering | **Not implemented.** `bzTTPlayer_t.target` is only a mode enum; the transport ABI exposes no authoritative point or entity payload to draw, so this layer logs once and deliberately renders nothing rather than fabricating a target location. Live controller/hand targeting is an input-layer concern and out of scope for this renderer-only slice. |

### Tests and build wiring

- `platform/android/quest/tests/test_bz_quest_wc3_fog.c` adds host coverage for
  all new pure branches: the three cell states, row-major indexing, first/last
  edge-cell round trips, rectangular grids, non-chunk-multiple grids, padded
  and non-padded packing, dirty-check hit/miss and length mismatch, marker
  transform/tint generation for multiple per-axis scale inputs (with and
  without a shared transform), independent per-axis nonpositive-scale
  rejection, and zero-dimension rejection.
- `platform/android/quest/tests/test_bz_quest_wc3_render.c` adds coverage for
  the shared `bz_quest_wc3_world_transform_measure/point` functions
  (valid/degenerate bounds, `NULL` passthrough), a test proving
  `bz_quest_wc3_build_world_matrix()`'s transform changes only the
  translation and never the rotation/scale block, and render-list coverage
  that each item's `footprintScaleX/Y/Z` (not `radius`, which no longer
  exists on either struct) exactly matches the mesh scale
  `bz_quest_wc3_build_world_matrix()` computes for the same entity, including
  a rectangular (non-square) footprint.
- `platform/android/quest/tests/test_bz_quest_wc3_world_transform.c` (new)
  holds the cross-subsystem integration tests described above - it
  deliberately calls the same production entry points terrain
  (`bz_quest_wc3_terrain_build_chunk`), entities
  (`bz_quest_wc3_build_world_matrix`/`build_render_list`), fog
  (`bz_quest_wc3_fog_world_to_cell`), and selection markers
  (`bz_quest_wc3_selection_marker_from_translation`) already use in
  production, never a hand-duplicated copy of the transform formula.
- `platform/android/quest/scripts/test-wc3-fog-selection-layout.sh` (wired into
  `make test` and `make quest` as `test-quest-wc3-fog-selection-layout`) guards
  the layer-5D shader/source registration, `VK_FORMAT_R8_UNORM`, explicit
  `bufferRowLength` upload path, the corrected fog-after-blended-passes render
  order, and the transform push-constant field on both the C and shader sides.
- `platform/android/quest/build.mk`'s `test-quest-host-tests` target now builds
  `bz_quest_wc3_fog.c` / `test_bz_quest_wc3_fog.c` and the new
  `test_bz_quest_wc3_world_transform.c` alongside the earlier pure Quest
  modules, and the shader build pipeline now regenerates the four new
  `warcraft_fog_*` / `warcraft_marker_*` SPIR-V headers automatically.

### Acceptance gates

This layer was **not** verified on a physical Quest device and was **not**
verified with a real loaded Warcraft III map in this environment:

- `bz_quest_host.c` still calls `bz_quest_bridge_start(..., NULL)`, so no map is
  selected at process start; the transport can therefore remain in the
  "no fog buffer yet / no entities yet" state here even though the renderer path
  now exists.
- No physical Quest device was connected, so there was no human visual check of
  real fog darkening, selection-ring placement, or depth interaction against
  real terrain/buildings.
- Verification for this slice is therefore limited to host tests, structural
  source checks, and the real native APK build/dependency inspection - **not**
  an on-device visual acceptance run, and **not** proof against retail ROC/TFT
  map data.

## Layer 5E: Warcraft III status/command-card HUD (`bz_quest_wc3_hud.h`/`bz_quest_vk_wc3_hud.c`)

Layer 5E adds one presentation feature on top of 5B/5C/5D's existing
terrain+model+fog+selection renderer: an authoritative status/command-card
HUD panel. It does **not** consume OpenXR controller/hand input, does **not**
post any tabletop command, and does **not** add particles/effects, audio,
data staging, or hand tracking - the exported hit-test contract exists for a
later input layer to call, but this layer never calls it itself.

### Authority: traced field-by-field against the ABI and visionOS, not assumed

The task asked for "selected unit info, health/mana/portrait/icon, resources/
status, target mode, enabled/disabled/hidden states, hotkeys/tooltips, and
loading/error state." Every one of those was traced concretely against
`platform/bridge/bz_tabletop_transport.h` and the reviewed visionOS reference
(`RealityTabletopView.swift`/`TabletopControls.swift`) - see
`bz_quest_wc3_hud.h`'s header comment for the full per-field trace. In short:

- **Health/mana/portrait: NOT IMPLEMENTED.** `bzTTEntity_t` has no health/
  mana field of any kind (every field checked), and there is no ABI portrait
  accessor - portraits are a desktop-local 3D render with no transport
  representation. This is not a gap this layer left; there is nothing in the
  ABI to trace.
- **Icon art: rendered as a flat, state-tinted placeholder, never a
  fabricated texture.** `bzTTActionButton_t.image_index` is `frame->tex.index`
  (`bz_tabletop_transport.c`'s `BuildActionButton()`, confirmed via
  `git blame`) - a desktop-client-local UI texture-registry handle, not a
  configstring/catalog index the asset ABI can resolve to pixel data. The
  visionOS reference itself never renders this field as an image either
  (only tooltip text/cooldown) - matching that precedent and layer 5D's
  "target mode has no location, so nothing is drawn" treatment of an
  equally unresolvable field.
- **Resources/status: fully supported**, going slightly beyond visionOS's own
  rendered surface (which decodes but never actually displays gold/lumber/
  food) while staying strictly inside the ABI's real, authoritative
  `bzTTPlayer_t` fields - not fabrication, just rendering data visionOS
  itself leaves undisplayed.
- **Selected unit info**: the ABI carries no per-entity name/type to show;
  the status bar shows the selection **count** from
  `BZ_TTSnapshot_SelectedEntityIds()`, nothing more.
- **Target mode, enabled/disabled/hidden, hotkeys/tooltips**: fully present
  on `bzTTActionLayout_t`/`bzTTActionButton_t` and fully implemented.
- **Loading/error state**: no player snapshot this frame renders a distinct
  "no player data" status line; a non-`NONE` `game_result` renders a distinct
  end-of-game line - both real ABI-derived states, not placeholders.

No transport ABI field or version changed for this layer - `bz_tabletop_transport.h`
stays exactly as it was after layer 5D.

### Authoritative HUD state flow

1. `bz_quest_wc3_capture.c`'s new `bz_quest_wc3_capture_hud()` does its own
   independent `BZ_TT_Latest()`/retain/copy/release cycle (matching every
   other capture function in this file), copies `frameId` from
   `BZ_TTSnapshot_Generation()`, the player resource/status fields, the
   selected-entity count, and the action layout's buttons (clamped to
   `BZ_QUEST_HUD_MAX_BUTTONS = 12`, matching `BZ_TT_MAX_COMMAND_BUTTONS`
   exactly - cross-checked with four `_Static_assert`s against the real ABI
   enums/constants). It logs once per unique `image_index` that icon pixel
   data cannot be resolved (see above) - never silently drops or substitutes
   a different action.
2. `bz_quest_wc3_hud.c` is the pure, host-testable layout module (no
   transport/Vulkan/Android/OpenXR type anywhere in it). `bz_quest_wc3_hud_build()`
   builds a `bzQuestHudFrame_t` deterministically from a `bzQuestHudInput_t`
   POD copy: a fixed panel transform, status/command-card background quads,
   one flat state-tinted quad per visible button slot, text runs for every
   label/cooldown/status line, and one hit region per non-hidden button plus
   a synthetic Cancel region whenever `currentTarget != NONE` - mirroring
   `RealityTabletopView.swift:837-878`'s exact rules (hidden buttons filtered
   entirely, row-major `(gridY, gridX)` sort into a fixed
   `BZ_QUEST_HUD_GRID_COLUMNS = 4` grid, an unconditional Cancel slot bound
   to a stable `CANCEL` action identity, not tied to any `buttons[]` slot).
3. `bz_quest_wc3_hud_hit_test(frame, currentFrameId, rayOrigin, rayDir, &outAction)`
   projects a world-space ray onto the panel's plane, converts to panel-local
   `(x,y)`, and returns the first hit region containing that point -
   rejecting stale `frameId`s, parallel rays, and behind-origin
   intersections. It reports a disabled hit's action identity too (hit-
   testing and command validation are deliberately separate, matching
   `TabletopControls.swift`'s own split) but is never called by this layer
   itself - it exists purely as the contract a later input layer consumes.
4. `bz_quest_vk_wc3_hud.c` is the impure Vulkan module: it calls
   `bz_quest_wc3_capture_hud()` + `bz_quest_wc3_hud_build()` once per
   renderable frame, expands the resulting quads/text runs into GPU vertex/
   index arrays (text runs go through `bz_quest_wc3_hud_font_layout_text()`
   at this step, not inside the pure layout module - see "Font atlas" below),
   and re-uploads only the buffers whose bytes actually changed (a direct
   `memcmp` against the persistently-mapped GPU buffer's own current
   content, not a separate host shadow array).

### Panel placement, scale, and readability

`bz_quest_wc3_hud_panel_transform()` returns a **fixed, deterministic,
map-size-independent** placement: anchored just outside the diorama's
conservative maximum half-extent (`BZ_QUEST_WC3_WORLD_TARGET_SPAN_F / 2`,
`bz_quest_wc3_render.h` - exposed from `bz_quest_wc3_render.c`'s private
`#define` specifically so this module never duplicates the `1.08` literal),
on the +X side, at a small height above the table, tilted back slightly for
overhead readability - the same translate+rotate composition style
`bz_quest_wc3_build_world_matrix()` uses for entities, but with a **fixed**
scale (never map→diorama scaled), matching this task's "stable scale/
readability" requirement. It never depends on a per-frame world transform
succeeding, so the panel has a well-defined position even in a loading/error/
no-map-loaded state.

### Font atlas: reused, project-owned, deterministic (no opaque binary)

`share/fonts/fixed_8x13.h` is an existing, source-committed, public-domain
(X11 misc-fixed `8x13.bdf`) bitmap font already vendored for the desktop
sysfont. `bz_quest_wc3_hud_font.c` packs that same font's 128 ASCII glyphs
into one small `VK_FORMAT_R8_UNORM` atlas via a pure, host-testable function
- reusing an existing project asset rather than shipping a second opaque
generated binary or taking on a platform text-rendering library dependency
(which would violate the UI module boundary - see AGENTS.md). The LSB-left
bit convention was verified concretely against the asymmetric `'F'` glyph
(not assumed) - see that file's header comment. Any byte outside 7-bit ASCII
remaps to `'?'` (a visibly-present "unsupported character" glyph, never
invisible blank space); `bz_quest_wc3_hud_font_glyph_uv()`/`_layout_text()`
themselves are pure and never log (see their header comments), so
`bz_quest_vk_wc3_hud.c`'s `build_text_vertices()` - the one production
caller of both - is what actually implements the "once per unique
unsupported byte" and "once per truncated text run" diagnostics, via its
own file-local `VK_WC3_HUD_LOG_ONCE` dedup helper (the same one-dedup-
table-per-translation-unit convention as `bz_quest_vk_wc3.c`'s).

### GPU representation, ownership, and draw order

- Two pipelines, matching layer 5D's fog/marker split: "panel" (flat-tint
  quads: status/command backgrounds, per-button placeholder slots) and
  "text" (glyph-textured quads sampling the one font atlas). Both use
  `VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST` (indexed quads, not the fog overlay's
  strip), `VK_CULL_MODE_NONE` (the panel's right/down/normal basis has no
  guaranteed winding), and are depth-tested but depth-write-disabled -
  readable over the board without ever occluding a later draw, the same
  rationale as layer 5D's selection markers.
- Both pipelines take a single vertex-stage push constant,
  `bzQuestVkWc3HudPushConsts_t{mat4 mvp}` (`viewProj * panelWorldMatrix`,
  built once per `bz_quest_vk_wc3_hud_record()` call and shared by both draw
  calls) - vertex positions stay in simple panel-local `(x,y,0)`, with the
  world transform applied entirely in the vertex shader, mirroring layer
  5D's `MarkerPushConsts_t.mvp` precedent.
- The font atlas texture is created once and, on failure, retried every
  frame via an idempotent guard (`if (vkHud->haveFont) return true;`) rather
  than being treated as fatal - satisfying "pending resources retry without
  repeated ABI pixel copies or log spam" (the retry only re-runs the cheap,
  deterministic atlas-pixel build, and failure is logged once via
  `BZ_QUEST_LOGE`, not per-frame). `create_font_atlas()` writes directly
  into `vkHud->fontImage`/`fontImageMemory`/`fontImageView` (one owner, no
  shadow locals), so every failure path past `vkCreateImage` calls
  `destroy_font_image()` before returning - tearing down exactly whatever
  got created and resetting every handle to `VK_NULL_HANDLE` - so a
  mid-sequence failure (e.g. out-of-memory at `vkAllocateMemory`) can never
  leak a live image/view across retries; only a fully successful sequence
  sets `haveFont = true`.
- Both dynamic vertex/index buffers (panel and text) are
  `HOST_VISIBLE|HOST_COHERENT` and mapped exactly once for the module's whole
  lifetime - the same single-buffered-per-frame discipline
  `bz_quest_vk_wc3.c`'s bone-palette UBO already relies on (this project's
  existing single in-flight-frame model; `bz_quest_renderer_render_warcraft_target()`
  waits on the previous frame's fence before this module's
  `capture_and_upload()` runs again, so there is never a write into a buffer
  the GPU may still be reading).
- The shared eye render pass order is now: terrain opaque → model opaque →
  terrain blended → model blended → fog overlay → selection markers → **HUD**.
  The HUD renders **last of all** world/overlay draws so the bridge-
  authored status/command-card panel is always legible on top of the fogged,
  selection-marked board - matching visionOS's own overlay-panel placement.
- The renderer's "is there anything to draw this frame" gate
  (`bz_quest_renderer.c`'s per-eye branch) now also checks
  `bz_quest_vk_wc3_hud_has_frame()`, so the HUD still renders on a frame with
  a connected bridge snapshot but no selection/models yet (an empty-board,
  no-selection state) instead of silently falling back to the procedural
  diagnostic scene only because no other WC3 content exists yet.

### Supported vs. unsupported HUD behavior

| Behavior | Status |
|---|---|
| Status bar: player name, gold/lumber/food used/cap, hero tokens | Supported. `BZ_QUEST_HUD_MAX_STATUS_TEXT` (88) is sized to the worst-case `UINT32_MAX`-valued resource line (78 chars) and max-length-name selected line (52 chars) with headroom - a prior too-small value (40) silently cut numeric fields mid-digit; `bzQuestHudFrame_t.statusTextTruncated` is a defensive runtime detector (set from each `snprintf()`'s own return value) that should be unreachable given today's field sizes, logged once by `bz_quest_vk_wc3_hud.c` if it ever fires. |
| Selected-entity count | Supported (count only - no per-entity name/health/mana; see "Authority" above) |
| Game-result (victory/defeat/draw) status line | Supported |
| Loading/no-player-snapshot status line | Supported |
| Command-card grid: row-major `(gridY, gridX)` sort, fixed 4-column layout, hidden slots filtered entirely | Supported |
| Disabled buttons: shown (distinct tint), not enabled, hit-testable but not actionable | Supported |
| Unsupported/`UNSUPPORTED` semantic buttons: treated as disabled, never fabricated as a real command | Supported |
| Synthetic Cancel region when `currentTarget != NONE` | Supported |
| Hotkey/tooltip/cooldown text | Supported |
| Icon art (`image_index`) | **Not implemented.** No ABI path resolves this desktop-local UI texture handle to pixel data - rendered as a flat, state-tinted placeholder quad instead of a fabricated icon (see "Authority" above). |
| Health/mana/portrait | **Not implemented.** No ABI field/accessor exists for either (see "Authority" above). |
| Panel world placement, stable fixed scale independent of map size | Supported |
| Ray/plane hit-test with stable action identity (`gridX,gridY,semantic,actionCode`) | Supported (exported contract only - never invoked by this layer) |
| Posting a command / consuming OpenXR controller-hand input | **Out of scope for this layer** - a later input layer's job. |

### Tests and build wiring

- `platform/android/quest/tests/test_bz_quest_wc3_hud_font.c` covers the
  verified glyph bit convention, atlas-build undersized-buffer rejection,
  UV lookup for a known glyph, the `'?'` unsupported-byte fallback, and
  text-layout truncation/space-advance/null-argument-rejection behavior.
- `platform/android/quest/tests/test_bz_quest_wc3_hud.c` covers the panel
  transform's determinism, no-player/loading status, resource/status-line
  formatting, game-result lines, command-card presence/absence rules,
  row-major sort, hidden-slot exclusion, disabled/unsupported-semantic
  handling, overlapping-grid-slot distinctness, cancel-region presence/
  absence, statelessness across successive different-input calls (map
  reload), hit-test coverage (stale frame ID rejection, center/edge/outside
  hits, parallel-ray and behind-origin rejection, and disabled-slot hits
  still being reported), and `UINT32_MAX`-valued resources/selected-count
  plus a maximum-length player name rendering completely untruncated
  (`statusTextTruncated == false`, every numeric field's full value present
  in the rendered text) - the regression test for the too-small
  `BZ_QUEST_HUD_MAX_STATUS_TEXT` defect a PR #24 review pass found.
- `platform/android/quest/scripts/test-wc3-hud-layout.sh` (wired into
  `make test`/`make quest` as `test-quest-wc3-hud-layout`) structurally
  guards the four new shaders' build-shaders.sh/CMakeLists.txt wiring, the
  font atlas's `VK_FORMAT_R8_UNORM` image/view format, the text shader's
  `set 0 binding 0` sampler, both pipelines' depth-test/depth-write/cull-
  mode/topology flags, the single vertex-only `mvp` push constant on both
  pipelines, the fog overlay → selection markers → HUD render-pass
  ordering in `bz_quest_renderer.c`, `create_font_atlas()`'s exactly-7
  `destroy_font_image(vkHud)` cleanup call sites (one per failure point
  past `vkCreateImage`, plus exactly one more in `bz_quest_vk_wc3_hud_destroy()`
  = 8 total) with `destroy_font_image()` defined before it's called, and
  `build_text_vertices()`'s truncation-return capture / unsupported-byte
  scan / `VK_WC3_HUD_LOG_ONCE`-backed diagnostics for text-run truncation,
  unsupported bytes, and `statusTextTruncated` - the structural regression
  coverage for the font-atlas leak and missing-diagnostics defects a PR #24
  review pass found (host-unit-untestable since the actual GPU calls and
  `fprintf` logging require a device/impure I/O).
- `platform/android/quest/build.mk`'s `test-quest-host-tests` target now
  builds `bz_quest_wc3_hud_font.c`/`bz_quest_wc3_hud.c` and their test files
  alongside the earlier pure Quest modules - **4472/4472 assertions pass**.
- The shader build pipeline (`build-shaders.sh`/`CMakeLists.txt`) now
  regenerates the four new `warcraft_hud_panel_*`/`warcraft_hud_text_*`
  SPIR-V headers automatically, and `CMakeLists.txt`'s `bz_quest_native`
  source list includes the three new `.c` files.

### Acceptance gates

This layer was **not** verified on a physical Quest device and was **not**
verified with a real loaded Warcraft III map in this environment:

- `bz_quest_host.c` still calls `bz_quest_bridge_start(..., NULL)`, so no map
  is selected at process start; the transport can therefore remain in the
  "no player snapshot yet" state here even though the renderer path now
  exists (this layer's own "no player data" loading status line is exactly
  the codepath exercised in that state).
- No physical Quest device was connected, so there was no human visual check
  of real HUD placement/readability, text legibility, or hit-test alignment
  against a real command-card layout.
- A real arm64-v8a debug APK **was** assembled in this environment via the
  project's Gradle/CMake build (`make quest-assemble-debug`) - confirmed via
  `quest-verify-native-lib` (arm64-v8a-only, no SDL2/desktop-GL/Apple-ObjC/
  VrApi dependency, no desktop `main()`, NativeActivity entry points intact)
  and by inspecting the built `.so` for this layer's new exported symbols
  (`bz_quest_vk_wc3_hud_create`/`capture_and_upload`/`record`/`destroy`,
  present and linked). This proves the new Vulkan module and shaders compile
  and link for the real target - it is **not** a substitute for an on-device
  visual acceptance run, and **not** proof against retail ROC/TFT map data.
- Verification for this slice therefore combines host tests, structural
  source checks, and the real native APK build/dependency inspection above -
  never claimed as an on-device visual success.

## Layer 6: Meta Quest Touch controller input (`bz_quest_xr_actions.c`, `bz_quest_input_state.c`, `bz_quest_vk_wc3_pointer.c`)

Layer 6 is the first layer that **reads the controllers and posts gameplay
commands**. Everything before it was render-only (the layer-5E HUD hit-test
was an exported-but-never-invoked contract - see "Supported vs. unsupported
HUD behavior" above). This layer wires the OpenXR Touch controllers into the
existing per-frame loop, runs a pure interaction state machine over the
captured world, and posts the decided command through the **existing typed
tabletop transport only** (`platform/bridge/bz_tabletop_transport.h`'s
`BZ_TT_Post*`), never by mutating local player/entity/selection state - the
server is authoritative and any effect appears only in a *later* snapshot.

### Module map and ownership

| File | Kind | Owns |
|---|---|---|
| `bz_quest_xr_bindings.h`/`.c` | **Pure** (plain `cc`, no OpenXR link) | The interaction-profile/action/component-path tables and an OpenXR path-syntax validator. No `Xr*` types - just strings and small enums, so the binding tables are host-testable. |
| `bz_quest_xr_actions.h`/`.c` | Impure (owns `XrAction`/`XrActionSet`/`XrSpace`) | One `XrActionSet`, the semantic `XrAction`s, per-hand aim/grip `XrSpace`s, `xrSuggestInteractionProfileBindings` per profile, `xrAttachSessionActionSets`, the once-per-frame `xrSyncActions` + `xrGetActionState*`/`xrLocateSpace` reads (unpacked into plain POD), and `xrApplyHapticFeedback`. |
| `bz_quest_input_state.h`/`.c` | **Pure** | The interaction phase enum, per-hand edge latches, the board pan/rotate/zoom transform (+ its inverse), the deterministic ray-hit priority, the command-mapping table, the haptic-pulse decision, and the idempotent transient-clear bookkeeping. This is what `tests/test_bz_quest_input_state.c` exercises with no OpenXR/Vulkan/engine link. |
| `bz_quest_wc3_capture.c` (`bz_quest_wc3_capture_interaction()`) | Impure (bridge snapshot reader) | Copies one snapshot generation's world transform, entity hit-spheres (composed center + footprint-scale radius), target mode, and selection set into plain POD, plus an FNV-1a map-name epoch, for the pure state machine. |
| `bz_quest_vk_wc3_pointer.h`/`.c` | Impure (Vulkan) | Procedural per-hand ray-beam + reticle geometry, drawn with the **reused** layer-5D `warcraft_marker` shaders (no new shader). |

The pure/impure split mirrors layer 5E's `bz_quest_wc3_hud.h` (POD-only) vs.
`bz_quest_vk_wc3_hud.c` (Vulkan) discipline: no `XrAction`/`XrSpace`/`XrPath`
ever appears in a host-testable header.

### Action map

One `XrActionSet` (`"tabletop"`) with these semantic actions, bound for both
`/interaction_profiles/oculus/touch_controller` and (as a reduced fallback)
`/interaction_profiles/khr/simple_controller`:

| Semantic action | Type | Touch binding (L / R) | simple_controller binding | Purpose |
|---|---|---|---|---|
| aim pose | pose | `input/aim/pose` (both) | `input/aim/pose` (both) | ray origin/direction |
| grip pose | pose | `input/grip/pose` (both) | `input/grip/pose` (both) | grip-drag anchor |
| select | bool | `input/trigger/value` (both) | `input/select/click` (both) | tap → select / smart-point / target-point / HUD button |
| squeeze | bool | `input/squeeze/value` (both) | *(unbound)* | grip → smart-entity order; left grip-drag pans the board |
| thumbstick | vec2 | `input/thumbstick` (both) | *(unbound)* | right X = board yaw, right Y = board zoom, left Y = board height |
| thumbstick click | bool | `input/thumbstick/click` (both) | *(unbound)* | reserved (no command mapped this layer) |
| primary (A/X) | bool | `input/x/click` (L) `input/a/click` (R) | *(unbound)* | additive-selection modifier |
| secondary (B/Y) | bool | `input/y/click` (L) `input/b/click` (R) | *(unbound)* | **cancel** (`BZ_TT_PostCancel`) |
| menu | bool | `input/menu/click` (**L only**) | `input/menu/click` (**L only**) | board reset to default transform |
| haptic | vibration output | `output/haptic` (both) | `output/haptic` (both) | accept/reject feedback |

Reserved-button rule: the Meta Quest system/Oculus button is **never** bound;
menu is left-hand-only because the profile only exposes a menu click on the
left controller (`bz_quest_xr_bindings.c`, verified by
`tests/test_bz_quest_xr_bindings.c`'s `test_no_reserved_system_binding` /
`test_menu_left_only_binding`).

**simple_controller degradation.** On the fallback profile only aim/grip/
select/menu/haptic are bound. The state machine degrades gracefully: no
thumbstick means no yaw/zoom/height board manipulation and no grip squeeze
means no smart-entity order; the menu click still resets the board, select
still drives select/smart-point/target-point/HUD-button, and cancel is
unavailable (the secondary button doesn't exist on that profile) - documented
here as an explicit reduced-capability path, not a silent no-op.

### OpenXR lifecycle integration (adds to, never duplicates, `bz_quest_xr.c`)

- `bz_quest_xr_actions_create()` is called once from
  `bz_quest_renderer_init()` after the session/spaces exist: it creates the
  action set + actions, suggests both profiles' bindings (each
  `xrSuggestInteractionProfileBindings` hard-checked - logs + returns false on
  failure, matching `bz_quest_xr.c`'s `bool`-return convention), creates the
  per-hand aim/grip action spaces, and calls `xrAttachSessionActionSets` once.
- `bz_quest_xr_actions_sync()` is called once per frame from
  `bz_quest_renderer_frame()` **only while the session is running and
  `XR_SESSION_STATE_FOCUSED`** (syncing while unfocused is wasteful/undefined
  per the spec); when not focused it leaves every hand inactive and
  `focused=false`, which drives the state machine's idempotent transient
  clear. It edge-reads each boolean, reads the thumbstick vec2, and locates
  the aim/grip poses at the frame's predicted display time (the same `XrTime`
  used to locate the eye views).
- Every state read honors `isActive`: an inactive action is never treated as
  "still pressed"/"still held". A controller transitioning active→inactive
  (device loss/reconnect) clears its latched edge state exactly once, as does
  a focus loss - both proven by `test_bz_quest_input_state.c`'s
  reconnect/focus-loss idempotent-clear tests.
- `bz_quest_xr_actions_destroy()` destroys the action spaces then the action
  set (which destroys its child actions), `XR_NULL_HANDLE`-guarded, mirroring
  `bz_quest_xr_destroy()`'s ordering discipline.

### Interaction state machine

`bz_quest_input_state.h`'s `bzQuestInputPhase_t` is a single enum (never
parallel booleans, per AGENTS.md): `IDLE_RAY` (ray pointing, no gesture owns
input), `BOARD_MANIPULATE` (a pan/rotate/zoom gesture owns input this frame),
and `TARGET_POINT_MODE` (the server's `current_target != NONE` owns input for
gameplay targeting). Board manipulation and a gameplay/target gesture are
mutually exclusive owners of input each frame - mirroring
`TabletopControls.swift`'s gesture-ownership exclusivity: a board grip-drag/
thumbstick gesture cannot begin while the server is in a target mode, and a
tap is suppressed on the same hand/frame a grip-drag or thumbstick gesture is
active so a manipulation never doubles as a tap. Ownership of the left grip
is additionally **hand-scoped, not just phase-scoped**: left squeeze is the
board-pan gesture's exclusive input and never maps to `PostSmartEntity`/
`PostSmartPoint`/`PostTargetPoint` regardless of `phase` or the server's
target mode (`bz_hand_owns_smart_trigger()`) - only the right hand's squeeze
rising edge is a valid smart-command trigger. This closes a fixed race where
the anchor frame of a left-grip drag reported `active=false` for one frame
(phase stayed `IDLE_RAY`), letting that same squeeze rising edge fall through
to the gameplay command loop and post a smart order a frame before
`BOARD_MANIPULATE` took ownership; `bz_update_board()` now reports `active`
starting on the anchor frame itself, and the hand-scoped trigger check is a
second, timing-independent guard for the target-mode case (where
`bz_update_board()` never runs at all, so phase can never reach
`BOARD_MANIPULATE` to suppress it that way).

Edge semantics: every button command fires only on the false→true transition
(`bz_quest_edge_update()` tracks previous-frame state per action) - never
re-fired while held, never on a bare "changed since last sync". Press-hold-
release yields exactly one command; two press/release cycles yield two
(`test_bz_quest_input_state.c`).

### Ray-hit priority (deterministic, evaluated every frame in this order)

The ray is first transformed **into composed space** with
`bz_quest_board_transform_inverse_ray()` (the same board transform the world/
HUD is drawn through), then `bz_quest_input_hit_test()` evaluates, in order:

1. **HUD action/cancel region** via `bz_quest_wc3_hud_hit_test()` against the
   *same* `bzQuestHudFrame_t` the renderer draws (`bz_quest_vk_wc3_hud_frame()`).
   A hidden slot yields no region; a disabled/stale-frame-id slot is reported
   as `HUD_DISABLED` - it *consumes* the ray (posts nothing, buzzes "reject")
   so the ray never passes through the panel onto a unit behind it. The frame's
   `generation` is used for both the HUD staleness check and the later post's
   `observed_generation` (same value, never divergent).
2. **Nearest entity sphere** (smallest positive ray-`t` wins). Entity hit
   spheres are built in `bz_quest_wc3_capture_interaction()` from the *same*
   `bz_quest_wc3_world_transform_point()` composed center and
   `bz_quest_wc3_entity_footprint_scale()` radius the rendered mesh/selection
   marker uses - never a separately-drifting bound, never `bzTTEntity_t.radius`.
3. **Terrain/world plane** (ray-plane at composed `y = planeY = 0`, the diorama
   base). No cheap per-point height query exists on
   `bz_quest_wc3_terrain.h`, so this is a flat-plane fallback at the diorama
   base - documented gap; the hit is converted back to authoritative engine
   world coordinates **exactly once** via the new
   `bz_quest_wc3_world_transform_point_inverse()` (added in
   `bz_quest_wc3_render.h`/`.c` alongside the existing forward transform,
   round-trip-tested in `test_bz_quest_wc3_world_transform.c`).
4. **No hit** → no command.

### Command mapping (table-driven)

`bz_quest_input_state.c`'s `kTargetTable` maps `(server target mode, hit kind)`
to a typed command; no if/else ladder:

| Hit | target = NONE | target = POINT | target = ENTITY | target = ENTITY_OR_POINT |
|---|---|---|---|---|
| HUD button | `PostButton(actionCode)` | (same) | (same) | (same) |
| HUD cancel region / secondary button | `PostCancel` | (same) | (same) | (same) |
| HUD disabled/stale | consume, reject-buzz, no post | (same) | (same) | (same) |
| Entity (trigger tap) | `PostSelect` (additive if A/X held, else replace) | `PostTargetPoint(entity origin)` | `PostTargetPoint(entity origin)` | `PostTargetPoint(entity origin)` |
| Entity (grip squeeze) | `PostSmartEntity(entity)` | `PostTargetPoint(entity origin)` | `PostTargetPoint(entity origin)` | `PostTargetPoint(entity origin)` |
| Terrain point | `PostSmartPoint(x,y)` | `PostTargetPoint(x,y)` | **reject** (no valid entity) | `PostTargetPoint(x,y)` |

**Documented ABI gap (target-entity).** `platform/bridge/bz_tabletop_transport.h`
exposes **no** `BZ_TT_PostTargetEntity` - only `BZ_TT_PostTargetPoint` (confirm
with `grep BZ_TT_Post` in that header: the only posts are Select, SmartEntity,
SmartPoint, Button, Cancel, TargetPoint). So when the server is in
`BZ_TT_ACTION_TARGET_ENTITY`/`ENTITY_OR_POINT` and the ray hits an entity,
this layer uses **workaround (a)**: post `BZ_TT_PostTargetPoint` at the
entity's own real engine ground origin (`entityEngineX,entityEngineNorth`,
captured from the authoritative snapshot origin - never fabricated). This
preserves server authority (the server still resolves what's at that point)
and fabricates no data beyond projecting the unit's own position to a ground
point. It is **not** silently dropped: an `ENTITY`-only mode terrain tap (no
valid entity) is explicitly rejected with the reject haptic. This is the least-
fabricating option given the ABI; if a `PostTargetEntity` is ever added, the
`kTargetTable` `onEntity` column is the single place to switch.

Every `BZ_TT_Post*` returns a `bzTTResult_t`; the impure driver
(`bz_quest_renderer_post_command()`) turns `BZ_TT_OK` into the "accept" haptic
and any failure (queue-full/stale-generation/invalid-argument) into the
"reject" haptic, leaving no latched success state. Haptic constants:
accept = 0.6 amplitude / 20 ms (crisp tap), reject = 0.35 amplitude / 90 ms
(soft buzz) - distinct by design so the player feels received-but-refused
input (`bz_quest_input_state.c:173-176`).

### Board transform (composes with, never replaces, the bounds transform)

`bzQuestBoardTransform_t` is a Quest-user-owned translation + yaw + uniform
scale that is **folded into** every eye's view*projection as
`mvpBoard = mvp * board` (`bz_quest_renderer.c`), applied consistently to
terrain, models, fog, selection markers, HUD panel placement, and the ray/
reticle endpoints, plus the inverse hit-test path - it does not replace the
existing bounds-derived `bzQuestWc3WorldTransform_t`, it stacks on top of it
(the same "one shared transform applied everywhere" pattern layer 5D
established). Composing the board transform then its inverse round-trips to the
original point within float tolerance (`test_bz_quest_input_state.c`).

Input mapping and clamp ranges (`bz_quest_input_state.h:84-104`):

| Gesture | Effect | Range/rate |
|---|---|---|
| Left grip-drag | pan (tx/tz) | translate clamped to ±3.0 m |
| Right thumbstick X | yaw rotate | 2.0 rad/s at full deflection |
| Right thumbstick Y | zoom (uniform scale) | 1.5 scale-units/s, clamped **[0.30, 3.00]** |
| Left thumbstick Y | board height (ty) | 0.6 m/s, clamped **[-2.0, 1.0]** m |
| Left menu click | reset board to default | tx=0, ty=-0.40, tz=-0.60, yaw=0, scale=1 |
| (thumbstick deadzone) | — | 0.2 |

The clamps mirror the "diorama box" reasoning in `bz_quest_wc3_render.h`: no
unbounded zoom-to-zero/infinity, board stays within arm's reach, height stays
above the floor and below head height. Every clamp is bounded at both ends,
proven in `test_bz_quest_input_state.c`.

### Rendering (ray/reticle)

`bz_quest_vk_wc3_pointer.c` builds procedural per-hand geometry each frame (a
thin crossed-quad beam, half-width 0.004 m, from the aim origin to the reticle
hit point, plus a 16-segment reticle disc, radius 0.02 m) into a host-visible
dynamic vertex buffer, written once per frame before the eye loop (hazard-free
because `render_warcraft_target` waits on the per-eye fence after each eye
submit). It **reuses the layer-5D `warcraft_marker` shader pair** (position-
only vertex, `mat4 mvp` + `vec4 tint` push constant) - layer 6 adds **no new
shader**. The pipeline matches the marker/HUD passthrough conventions: depth-
test on, depth-write off (a beam is occluded by nearer board geometry but never
occludes later draws), `VK_CULL_MODE_NONE`, `TRIANGLE_LIST`, straight-alpha
`SRC_ALPHA/ONE_MINUS_SRC_ALPHA` blend over passthrough. The pointer is recorded
**last** (after the HUD) in the *plain* per-eye view*projection - the physical
controllers live in tracking space, not the board-folded composed space; the
reticle endpoints were already mapped out of composed space by
`bz_quest_renderer_process_input()`. The reticle/beam tint encodes the hit
kind (amber HUD, red disabled/refused, green entity, cyan terrain, dim white
no-hit). No per-frame logging (matches "No busy loop / no per-frame logging"
above).

### Lifecycle / edge cases (each clears transient state exactly once)

`bz_quest_input_state_update()` idempotently clears all transient interaction
state (edge latches, active pan-drag) exactly once on entry to each of: focus
loss, controller `isActive` loss, snapshot-generation change, and map-epoch
change (a real `BZ_TTSnapshot_MapName()` change, tracked via the FNV-1a epoch -
never per-generation, which advances every frame), plus a board reset on map
change. `bz_quest_renderer_shutdown()` clears the state and destroys the
pointer + action modules. Each clear is one-shot: staying in the condition
across N frames does not re-fire (both directions tested).

### Supported vs. unsupported behavior

| Behavior | Status |
|---|---|
| Select (replace/additive), smart-entity, smart-point, cancel, HUD button, target-point orders posted via typed transport | Supported |
| Board pan/rotate/zoom/height + reset, folded consistently into world/HUD/fog/selection/pointer | Supported |
| Deterministic ray-hit priority with the real HUD hit-test + real entity footprints + terrain plane | Supported |
| Per-hand aim rays + hit reticles with hit-kind tint | Supported |
| Accept/reject haptics distinct by amplitude/duration | Supported |
| Target-**entity** submission | ABI gap: no `BZ_TT_PostTargetEntity`; resolved as target-point at the entity's ground origin (workaround (a) above), never silently dropped. |
| Terrain **height** under the ray | Flat diorama-base plane only - no per-point heightfield query exists on `bz_quest_wc3_terrain.h` (documented gap). |
| Hand tracking, audio, data staging, particles/effects, multiplayer, Meta Platform services | **Out of scope for this layer** - separate later layers, not started. |

### Tests and build wiring

- `platform/android/quest/tests/test_bz_quest_input_state.c` and
  `tests/test_bz_quest_xr_bindings.c` are the two new pure suites (see the
  [Testing](#testing) table for exactly what each proves), plus the extended
  `tests/test_bz_quest_wc3_world_transform.c` inverse-transform round-trip.
  All run under `make test-quest-host-tests` - **4880/4880 assertions pass**.
- `platform/android/quest/scripts/test-wc3-pointer-layout.sh` (wired into
  `make test`/`make quest` as `test-quest-wc3-pointer-layout`) structurally
  guards the pointer module's shader **reuse** (no new pointer shader), its
  depth/cull/topology/blend pipeline flags, the "pointer recorded after HUD in
  the plain mvp" render order, the `mvpBoard = mvp * board` fold, and the
  action module's exactly-once focus-gated `xrSyncActions`.
- `CMakeLists.txt`'s `bz_quest_native` source list includes the four new
  `.c` files (`bz_quest_input_state.c`, `bz_quest_xr_bindings.c`,
  `bz_quest_xr_actions.c`, `bz_quest_vk_wc3_pointer.c`) and defines
  `BZ_QUEST_ENABLE_INPUT=1`; `bz_quest_host.c`'s `#error` seam for that flag is
  now the "layer 6 must define this to 1" guard (replacing the earlier
  "later layer" gate).

### Acceptance gates

This layer's **pure** logic (state machine, hit priority, command mapping,
board transform, binding tables, transform inverse) is fully host-verified by
the tests above. The **impure** OpenXR/Vulkan glue was compiled/linked for the
real arm64-v8a target via the project's Gradle/CMake build and checked by
`quest-verify-native-lib`, but was **not** run on a physical device:

- No physical Quest 3/3S was connected, so there was **no** human check of real
  controller ray alignment, reticle placement, haptic feel, board-manipulation
  ergonomics, or that a posted command actually round-trips through the server
  into a later snapshot. These are **hardware-only** gates - see the exact
  on-device procedure below.
- `bz_quest_host.c` still starts the bridge with `NULL` (no map selected), so
  even on-device the transport can sit in the "no player snapshot" state; a
  real command round-trip additionally requires real loaded ROC/TFT map data,
  which this host does not have.

### Exact on-device acceptance procedure (requires a connected Quest 3/3S)

```sh
# 1. Build + install (see "Build/run/log commands" above for env setup).
make quest-assemble-debug
adb install -r platform/android/quest/app/build/outputs/apk/debug/app-debug.apk
# 2. Launch and tail this app's log tag while wearing the headset.
adb shell am start -n <package>/android.app.NativeActivity
adb logcat -s bz_quest_native:V
# 3. With a real map loaded, verify by hand (each is hardware-only, unproven here):
#    - both controller rays render and track; the reticle tints by hit kind
#      (amber over the HUD, green over a unit, cyan over terrain);
#    - trigger over a unit selects it (selection marker appears in a LATER
#      frame - never instantly/locally); A/X + trigger adds to selection;
#    - grip over a unit issues a smart order; trigger over terrain issues a
#      smart-point order; a command-card button posts its action; B/Y cancels;
#    - in a target mode, trigger over terrain/unit issues the target-point
#      order (unit resolves to its ground origin - the ABI-gap path);
#    - left grip-drag pans, right stick rotates/zooms, left stick raises/lowers,
#      left menu resets - all move terrain+models+fog+selection+HUD together;
#    - a crisp buzz on an accepted action, a distinct softer buzz on a refused
#      one (disabled HUD slot, queue-full, stale generation);
#    - dropping a controller (set it down) stops its ray without a phantom
#      press; taking the headset off (focus loss) and back on resumes cleanly.
```

## Layer 7: Warcraft III data staging + native AAudio output

Two independent pieces, explicitly scoped together as this stacked layer's
deliverable and nothing more: (1) a reproducible **developer ADB workflow**
that stages a user-owned Warcraft III ROC/TFT data directory into the exact
app-accessible path/override-file contract "Data-path contract" above
already defines, and (2) an **Android AAudio sink** that consumes the
existing shared bounded tabletop audio queue
(`platform/bridge/bz_tabletop_audio.h`) and actually plays sound on-device.
Explicitly out of scope (see "Current limitations" below for the honest
accounting): hand tracking, particles/effects, multiplayer, Store asset
delivery, Meta's spatial Audio SDK, and Platform services.

### Why data staging is a developer script, not an in-app feature

Warcraft III's ROC/TFT archives are the user's own purchased game data —
this project never bundles, downloads, or ships them (see AGENTS.md and
`platform/apple/visionos/scripts/wc3_data.sh`'s identical stance for the
visionOS port). The app itself only ever *reads* whatever directory
`bz_quest_data_resolve()` resolves to (see "Data-path contract" above); it
has no in-app "pick/copy my data" UI or code path, and none is added here
(`BZ_QUEST_ENABLE_DATA_STAGING` in `bz_quest_host.c` stays permanently at
its default `0` — flipping it to `1` is a build-time `#error`, since there
is no runtime feature it could gate). Getting the developer's own files
from a host machine onto the device is a **build/dev-time** problem, solved
entirely by `platform/android/quest/scripts/stage-wc3-data.sh` plus the
`make quest-*-wc3-data` targets in "Build/run/log commands" above.

### Why `run-as` + a `/data/local/tmp` bounce, not a direct `adb push`

Android's scoped storage model (API 29+) blocks other processes — including
an `adb shell` session — from writing directly into another app's private
storage sandbox
(<https://developer.android.com/training/data-storage/app-specific>: "the
system prevents other apps from accessing [app-specific] directory[ies]").
That rules out assuming shell access to `/sdcard/Android/data/<pkg>/...`,
which the task explicitly calls out as the wrong assumption to make.

The documented, supported dev-time mechanism instead is `run-as
<package>`, which lets an ADB shell assume a **debuggable** app's own UID —
"you can use the `run-as` command to invoke commands as that app"
(<https://developer.android.com/studio/debug>, "Use run-as: run commands as
a debuggable app"). Two consequences this script relies on, confirmed
against that same official page:

- `run-as <pkg> pwd` — the page's own example capability-check command —
  is exactly how `stage-wc3-data.sh` verifies (a) the app is installed,
  and (b) it is debuggable (a release/non-debug build makes `run-as` itself
  fail, which the script surfaces as its own explicit
  "app is not debuggable" error, not a generic ADB failure).
- `run-as`'s default working directory is the package's own private data
  root — so every remote path the script issues *inside* a `run-as`
  invocation (e.g. `files/Warcraft III/War3.mpq`) is deliberately relative,
  never a second, independently-computed absolute guess at where
  `Context.getFilesDir()` lives on a given OS build.

`run-as` itself has no way to receive bytes from the host machine directly
(it only lets you *run a command* as the app, not stream a local file in).
The standard workaround — and what this script does — is a two-hop bounce
through `/data/local/tmp`: a world-writable, shell-owned location that is
explicitly **outside** the scoped-storage sandbox model, so a plain `adb
push` can write to it, and `run-as`'s own `cp` can then read from it into
the app's private storage. `stage-wc3-data.sh` always deletes its own
bounce file (`trap ... EXIT INT TERM`) whether the transfer succeeds or
fails, and each bounce filename is unique per invocation (`bz_quest_stage.
$$.<name>`) so two concurrent staging runs, or a stale leftover from a
killed previous run, can never collide with or be mistaken for the current
transfer.

`ANativeActivity::internalDataPath` — the base `bz_quest_data.h`'s override
file lives under — maps to `Context.getFilesDir()` per the NDK's own
`ANativeActivity` struct reference
(<https://developer.android.com/ndk/reference/struct/a-native-activity>),
which is exactly `run-as`'s own default working directory root; this is why
the script can write `files/warcraft_data_path_override.txt` (relative,
inside `run-as`) and have it land at precisely the path `bz_quest_data.c`
reads at startup, with no separate path-translation step to keep in sync.

### ROC/TFT archive rules the script enforces (traced from `common/common.c`)

`FS_AddDataDirectory()` scans the resolved directory recursively for any
`.mpq` file, skips any whose basename starts with `"War3x"` (case-
insensitive) unless `fs_expansion` is `1` (set by `-tft`, unset — the
default — by no flag or `-roc`; see `common/cvar.c`), sorts every
surviving archive alphabetically (case-insensitive `strcasecmp` on
basename) before mounting, and — because `FS_OpenFile()` searches mounted
archives in **reverse** mount order (highest index first, first match
wins) — the alphabetically-*last* archive name wins any file-name
collision. Since `War3x.mpq` sorts after `War3.mpq` (`.` < `x` in ASCII),
this is exactly "TFT overrides ROC" without any special-cased merge logic
anywhere in the engine — the script's staging behavior only needs to
preserve that same ordering-by-filename on disk, not reimplement any
override logic itself.

`stage-wc3-data.sh` requires:

- **ROC-only**: exactly `War3.mpq` present. Supported and the minimum
  valid layout.
- **TFT-over-ROC**: `War3.mpq` **and** both `War3x.mpq` and
  `War3xlocal.mpq` present. All three are required together — a directory
  with `War3x.mpq` but missing `War3.mpq`, or with `War3x.mpq` but missing
  `War3xlocal.mpq`, is an **incomplete mixed layout** and is rejected with
  an actionable error naming exactly which required file is missing,
  never silently staged as ROC-only or silently ignoring the partial TFT
  files.
- Filenames are matched case-insensitively (Windows-sourced installs vary
  case) but staged using the **source directory's own on-disk casing and
  spacing verbatim** — never renamed/normalized — since `FS_AddDataDirectory`'s
  own basename comparisons are already case-insensitive, so altering case
  on-device would achieve nothing and would only make `verify`/manual
  inspection confusing.

**Runtime TFT mounting is now wired end-to-end (previously a known gap,
now fixed):** the layer 7 follow-up added `bz_quest_data_detect_edition()`
and threaded its result into `bz_quest_data_build_argv()`'s new `"-tft"`
dash-flag emission (see "Engine startup argv construction" above for the
full detection/argv/log-evidence design and why it must be a startup
dash-flag rather than a late `+fs_expansion 1`). A correctly-staged TFT
layout is now both discoverable-and-correctly-ordered on disk **and**
actually mounted at runtime: `fs_expansion` is set to `1` before
`FS_AddDataDirectory()` runs, so `FS_AddArchiveScanEntry` no longer skips
`War3x*` archives when TFT was detected. `bz_quest_data.h`/`bz_quest_bridge.c`
remain the same previously-reviewed layer-4 API surface — only the
already-planned edition parameter (visible in the header before this fix
landed) is now actually wired through, no other shared contract changed.

### Staging safety properties

- **Every remote command is a single pre-quoted string** before it ever
  reaches `adb shell`/`run-as` — POSIX single-quote escaping (`'` →
  `'\''`) is applied at each shell-boundary transition, so a host path,
  package name, or serial containing spaces or shell metacharacters is
  never re-tokenized or allowed to break out of its argument. No `eval`,
  no unquoted variable expansion in a command position.
- **No wildcard destructive deletion.** The only two things this script
  ever deletes are (a) one file's own temp-name family
  (`<name>.stage.*`, scoped by concatenating a quoted known prefix with an
  unquoted glob suffix — never a directory-wide `rm -rf *`), and (b) —
  only under `clean --yes` — the app's private `files/Warcraft III`
  subdirectory and the override file, nothing else, and only after an
  explicit `--yes` confirmation (the bare `clean` command refuses to run).
- **Atomic replace, never a partial file at the final name.** Each archive
  is pushed to a unique bounce name, copied device-side into a `.stage.$$`
  temp name inside the destination directory, sha256-verified against the
  locally-computed hash, and only then `mv`'d over the final name. A
  device-side `cp` failure, a `sha256sum` mismatch, or an interrupted
  transfer (killed before the temp file lands) all leave whatever was
  previously staged at the final name completely untouched — verified by
  `test-stage-wc3-data.sh`'s corruption/interruption cases.
- **Idempotent, without trusting stale files.** Before transferring
  anything, the script hashes the local source file and — if a file of the
  same name already exists at the final destination — its sha256 too
  (never its size/mtime alone, which a corrupt or truncated prior transfer
  could still match); an identical hash skips the transfer entirely
  ("unchanged, skipping"), while any difference re-runs the full
  push/copy/verify/atomic-replace sequence — this never assumes a
  previously-staged file is still good without recomputing its hash.
- **Explicit, distinct failures for every precondition** — no device
  attached, more than one attached device without `--serial`, a `--serial`
  that matches no attached device, a matched device that is `offline`/
  `unauthorized`, package not installed, package not debuggable (`run-as`
  itself fails), and insufficient free space on-device each produce their
  own actionable error message and a non-zero exit; none of these fall
  through to a generic/silent failure.
- **`--serial` is threaded through one choke point.** Every single `adb`
  invocation the script makes goes through one `adb_()` wrapper that
  prepends `-s "$serial"` whenever `--serial` was given — there is no
  second code path that could accidentally issue an un-suffixed call once a
  serial has been selected. `require_device()` also rejects an ambiguous
  bare invocation outright: with more than one attached device and no
  `--serial`, or with a `--serial` that names no attached device, or whose
  device is `offline`, the script fails before making any push/shell call
  that could otherwise silently land on the wrong headset.
  `test-stage-wc3-data.sh`'s fake-adb harness simulates multiple attached
  devices (a `DEVICES="serial:state ..."` list, each routed to its own
  per-serial fake device root) and an invocation log to assert every call
  in a `--serial`-scoped run consistently carries that same serial.
- **Free-space preflight accounts for the transient bounce+stage peak, not
  just final archive size.** Each changed file transiently exists as up to
  three copies at once mid-transfer — the host source, the `/data/local/tmp`
  bounce copy (freed by the script's own `trap` right after `run-as`'s `cp`
  succeeds), and the app-private `.stage.$$` temp copy (freed by the
  atomic `mv` to its final name) — so checking only the sum of final
  archive sizes can pass a preflight that later fails mid-transfer with no
  bytes recoverable. `cmd_stage()` instead computes, from each file's
  already-fetched remote hash, the total bytes that actually need transfer
  (`transfer_bytes`) and the largest single file among them
  (`max_transfer_bytes`), and requires `transfer_bytes + max_transfer_bytes`
  free — a safe (if not always maximally tight) upper bound for the
  worst-case sequential transient peak. Unchanged (already-verified) files
  contribute zero bytes to this figure, so a fully-idempotent re-stage
  never gets blocked by a peak sized for a fresh transfer even on a
  near-full device; `test-stage-wc3-data.sh` covers both the near-full
  rejection (with the prior valid file preserved, no `.stage.*` leftover)
  and the idempotent-not-blocked case.

### AAudio sink design

`bz_quest_audio.h`/`.c` is the **only** translation unit in this project
that includes `<aaudio/AAudio.h>` or touches an `AAudioStream` — every
other Android/AAudio type stays out of `platform/bridge`/
`platform/tabletop` headers, per this task's explicit requirement. It
consumes the existing, **unchanged** shared queue
(`platform/bridge/bz_tabletop_audio.h`: `BZ_TTAudio_Configure()`/
`_Dequeue()`/`_DroppedCount()`, `BZ_TT_AUDIO_QUEUE_CAPACITY = 32`,
`BZ_TT_AUDIO_MAX_BYTES = 1 MiB`) exactly as `platform/tabletop/client/
s_tabletop_null.c`'s `S_PlaySoundFile()` already fills it — no ABI change
was made or was found to be necessary.

**Three-thread model**, mirroring `bz_quest_bridge.h`'s "one thread owns
the lifecycle" convention, extended with AAudio's own two callback
threads:

1. **Control thread** — Android's main/UI thread, the same one
   `android_main()` already runs everything else on. Every function in
   `bz_quest_audio.h` except the two callbacks below must only ever be
   called here. `bz_quest_audio_drain()` — called once per
   `android_main()` loop iteration, exactly like `bz_quest_snapshot_capture()`
   already is — is the one function that dequeues, parses, converts,
   submits, reaps, and logs; i.e. every allocating/decoding/I/O step in
   this whole sink happens here, never on either callback thread.
2. **AAudio RT (data) callback thread** — a dedicated real-time-priority
   thread AAudio itself creates
   (<https://developer.android.com/ndk/guides/audio/aaudio/aaudio#using-a-high-priority-callback>).
   Calls **only** `bz_quest_audio_mixer_render()` — never allocates, locks,
   logs, decodes, or calls into the bridge/engine (see "Real-time-safety"
   below).
3. **AAudio error-callback thread** — a separate thread AAudio may invoke
   on disconnect. Per AAudio's own docs
   (<https://developer.android.com/ndk/guides/audio/aaudio/aaudio#disconnected>:
   "If you are notified of the disconnect in an error callback thread then
   the stopping and closing of the stream must be done from another
   thread"), this callback does nothing but publish one atomic flag;
   `bz_quest_audio_drain()` (control thread) performs the actual
   close+reopen+restart on its own next call.

**Real-time-safety, verified structurally** (`make
test-quest-audio-rt-callback-safety`, see "Testing" above): the data
callback (`bz_quest_audio_data_callback`) touches nothing but
`bz_quest_audio_mixer_render()`, which itself only does `memset` +
saturating-add arithmetic over already-owned buffers plus a single atomic
load/store per voice per callback (see `bz_quest_audio_mixer.h`'s header
comment for the full lock-free producer/consumer handoff design: each
voice's `active` field is the *sole* synchronization point between the
control thread's `submit()`/`reap()` and the callback thread's `render()`
— a release-store-then-acquire-load handoff, not a queue, with no mutex
anywhere in the hot path).

**WAV parsing** (`bz_quest_wav.h`/`.c`) is a strict, pure, host-testable
RIFF/WAVE parser — the only container this sink ever needs, since every
Warcraft III sound asset this engine loads is a `.wav` file (see
`games/warcraft-3/docs/sounds.md`'s SLK-driven sound catalog). Explicitly
supported: `RIFF`...`WAVE` with a `fmt ` chunk strictly before `data`,
`audioFormat == 1` (integer PCM) only, 1 or 2 channels, 8-bit
unsigned/16-bit signed samples, with `blockAlign`/`byteRate` required to be
internally consistent with the header's own channel/bit-depth/sample-rate
fields. Every chunk walk is bounds-checked against the buffer size before
any read (a chunk size that would read past the end is a parse failure,
never an out-of-bounds read), and RIFF's mandatory even-alignment pad byte
after an odd-sized chunk is honored. Anything else — WAVE_FORMAT_EXTENSIBLE,
ADPCM, MP3-in-WAV, 24/32-bit or float PCM, >2 channels, a truncated/
misaligned/inconsistent header — is a hard, explicit rejection with a
human-readable reason, logged **once per distinct rejection reason** (not
once per occurrence) by the one real caller (`bz_quest_audio_drain()`),
matching AGENTS.md's "Missing Asset Placeholders" once-per-unique-condition
convention applied here to audio instead of textures/models. No decoder
fallback of any kind exists.

**Format negotiation**: the sink requests `AAUDIO_FORMAT_PCM_I16` and
`BZ_QUEST_AUDIO_TARGET_CHANNELS` (2, stereo) explicitly on the builder,
then re-verifies both after opening (defensive, not redundant — a future
edit that drops one of the explicit builder calls would otherwise silently
start converting to the wrong layout). Sample rate is deliberately **not**
requested — AAudio picks the device's native rate
(<https://developer.android.com/ndk/guides/audio/aaudio/aaudio>: "After the
stream is opened you must query the sample data format"), queried once via
`AAudioStream_getSampleRate()` after open. `bz_quest_audio_mixer_convert()`
(control thread only) then deterministically converts every voice to that
rate: 8-bit unsigned source samples widen to signed 16-bit, mono source
duplicates to both output channels, and a source rate that differs from
the target is resampled via **deterministic linear interpolation** (never
a black-box resampling library) with an exact, test-asserted output frame
count and per-sample values — never a heuristic/best-effort approximation.
A conversion that would exceed `BZ_QUEST_AUDIO_MAX_OUTPUT_FRAMES` (30
seconds at 48 kHz — already far beyond any real WC3 sound asset, since the
*source* file is itself bounded to `BZ_TT_AUDIO_MAX_BYTES` = 1 MiB) is
rejected rather than allocating an unbounded buffer.

**Bounded voice pool/mixer** (`bz_quest_audio_mixer.h`/`.c`):
`BZ_QUEST_AUDIO_MAX_VOICES = 8` fixed slots; `submit()` (control thread)
takes ownership of a `malloc()`'d, already-converted PCM buffer into the
first free slot or fails without touching ownership if every slot is
active (the caller must `free()` it and count a drop); `render()` (RT
thread) mixes every active voice's next `frameCount` frames into the
caller's zeroed output buffer with saturating add-and-clamp, advances each
mixed voice's cursor, and flips a voice inactive the instant it reaches
its end (a single atomic store — still real-time-safe); `reap()` (control
thread) frees the owned buffer of any voice the callback has since marked
inactive. Two overlapping voices genuinely sum (not overwrite/replace) and
clip-saturate at the `int16` boundary rather than wrapping.

**Lifecycle** (`bz_quest_audio_lifecycle.h`/`.c`, pure state machine):
`STOPPED` → `RUNNING` (start) ⇄ `PAUSED` (pause/resume) → `STOPPED` (stop),
with a `FAILED` terminal state reachable from a failed start *or* a failed
disconnect-restart, itself allowing a fresh start attempt to retry (never
auto-retried by this module itself — mirrors `bz_quest_renderer_init()`'s
own "never silently retry from inside the command loop" convention).
`bz_quest_host.c` wires this to the Android lifecycle exactly like the
bridge/renderer: `bz_quest_ensure_audio_start()` on `APP_CMD_START`
(guarded, so a second `APP_CMD_START` never double-opens a stream),
`bz_quest_audio_suspend()`/`_resume()` on `APP_CMD_PAUSE`/`APP_CMD_RESUME`
(each gated on the lifecycle's own `can_pause`/`can_resume` check, not
just the module's internal error log, so a stray extra pause/resume is a
true no-op rather than a logged-but-otherwise-silent double-call), one
`bz_quest_audio_drain()` call per main-loop iteration, and
`bz_quest_audio_stop()` in final teardown (guarded on
`audioStartAttempted && bz_quest_audio_lifecycle_can_stop(...)`, so tearing
down a sink that never successfully started is a safe no-op, never a
double-close). `bz_quest_audio_stop()` frees every voice slot's owned PCM
regardless of its active flag — safe because no callback can still be
running once `AAudioStream_close()` has returned
(<https://developer.android.com/ndk/guides/audio/aaudio/aaudio>: "Closing
an audio stream").

**Drop-counter logging** follows the same one-shot-per-value-change rule as
the shared queue's own drop counter: `BZ_TTAudio_DroppedCount()` (queue
overflow — `S_PlaySoundFile()` filled the shared queue faster than
`drain()` emptied it) and this sink's own voice-pool-full count
(`bz_quest_audio_mixer_submit()` found every slot active) are each logged
only when their value actually changes since the last log, never once per
`drain()` call/frame/callback — verified by
`bz_quest_audio_lifecycle_counter_changed()`'s own tests.

### Supported vs. unsupported audio behavior

| Behavior | Status |
|---|---|
| Mono or stereo, 8-bit unsigned or 16-bit signed PCM WAVE | Supported |
| Any other WAVE subtype (ADPCM, float, WAVE_FORMAT_EXTENSIBLE) or non-WAVE container | Explicitly rejected, logged once, dropped |
| Sample-rate conversion to the device's native rate | Supported (deterministic linear interpolation) |
| Up to 8 concurrent overlapping one-shot voices, saturating-mixed | Supported |
| A 9th concurrent voice while all 8 are active | Dropped, counted, logged once per count change |
| Pause/resume/stop across Android `APP_CMD_*` transitions | Supported, idempotent, no double-close |
| AAudio disconnect (e.g. headset/route change) | Detected via error callback, restarted from the control thread on the next `drain()` |
| Spatial/positional audio (Meta XR Audio SDK) | **Out of scope** — every voice plays as flat, non-positional stereo; this is an honest prototype limitation, not a bug |
| Exclusive sharing mode / low-latency performance mode | **Out of scope** — `AAUDIO_SHARING_MODE_SHARED` / `AAUDIO_PERFORMANCE_MODE_NONE` (the balanced default) only |

## Layer 8: Meta Quest hand tracking (`bz_quest_xr_hands.c`, `bz_quest_hand_input.c`, `bz_quest_input_state.c`)

Layer 8 adds native Meta Quest **hand-tracking input parity** alongside
(never replacing) layer 6's Touch controllers: the same select/smart-entity/
smart-point/cancel/board-pan commands, reachable via a pinch gesture and a
tracked-joint- or Meta-standardized-aim-pose-derived ray instead of a
physical controller, with Touch remaining the preferred source whenever it
is actually tracked. It is entirely **optional and additive** — every
existing layer-6/7 behavior, test, and build target is unchanged, and a
runtime/device/account without hand tracking behaves identically to before
this layer existed.

### Module map and ownership

| File | Kind | Owns |
|---|---|---|
| `bz_quest_hand_input.h`/`.c` | **Pure** (plain `cc`, no OpenXR link) | `bzQuestHandCapability_t` (NONE/EXT_ONLY/FB_AIM), the plain-POD joint/aim-sample structs, the EXT-only pinch-distance hysteresis latch, and `bz_quest_hand_sample_build()` — the one function that turns either representation into a `bzQuestInputHandSample_t`, the SAME type layer 6's Touch path already produces. No `Xr*` type appears anywhere in this file (verified structurally — see "Tests and build wiring" below). |
| `bz_quest_xr_hands.h`/`.c` | Impure (owns `XrHandTrackerEXT`) | The two hand trackers (`XR_HAND_JOINT_SET_DEFAULT_EXT`), the three resolved `XR_EXT_hand_tracking` function pointers, the once-per-frame focus-gated `xrLocateHandJointsEXT` call (chaining `XrHandTrackingAimStateFB` when negotiated), and the EXT-only pinch hysteresis latch's *storage* (the pure module owns the *logic*). Mirrors `bz_quest_xr_actions.c`'s exact discipline, except every function here is optional/never-fatal. |
| `bz_quest_xr.h`/`.c` (extended) | Impure | `bzQuestXrHandCapability_t` — the negotiated capability (extension-enabled + system-supported, for both `XR_EXT_hand_tracking` and `XR_FB_hand_tracking_aim`), decided once at `bz_quest_xr_create_instance()`/`_get_system()` time, alongside the pre-existing `passthroughCapabilities`. |
| `bz_quest_input_state.h`/`.c` (extended) | **Pure** | `bzQuestInputSourceKind_t` (CONTROLLER/HAND) and `bz_quest_input_arbitrate_source()` — the deterministic, hysteresis-debounced per-hand source arbitration folded into the top of `bz_quest_input_state_update()`, plus the two small `bzQuestInputFrame_t`/`bzQuestInputState_t` fields (`handSample[]`, `source[]`/`controllerLossSeconds[]`) that carry it. Every hit-test/board/command-mapping line below the arbitration step is **completely unchanged** from layer 6/7 and stays unaware of which source produced the sample it reads. |
| `bz_quest_renderer.c` (extended) | Impure | Creates `xrHands` after the session (mirroring `xrActions`), calls `bz_quest_xr_hands_sync()` right after `bz_quest_xr_actions_sync()` into the SAME `bzQuestInputFrame_t`, and destroys it before the session/instance in `bz_quest_renderer_shutdown()`. Adds no new pointer/HUD rendering code — see "Rendering" below. |

The pure/impure split mirrors layer 6's `bz_quest_xr_bindings.h` (pure
tables) vs. `bz_quest_xr_actions.c` (impure `XrAction`/`XrSpace` owner)
discipline exactly: no `XrHandTrackerEXT`/`XrHandJointLocationEXT`/
`XrHandTrackingAimStateFB` ever appears in a host-testable header.

### Capability negotiation (explicit, optional, never fatal)

Unlike `XR_KHR_android_create_instance`/`XR_KHR_vulkan_enable2`/
`XR_FB_passthrough` (hard startup requirements — see "OpenXR session
lifecycle" above), hand tracking is negotiated at **two** independent,
always-optional gates, both probed the same way the three required
extensions are checked (`bz_quest_check_required_names()`), but neither ever
fails `bz_quest_xr_create_instance()`/`_get_system()`:

| Extension | Number/depends (OpenXR registry `xr.xml`) | What it adds | Gate |
|---|---|---|---|
| `XR_EXT_hand_tracking` | `number="52"`, no dependency beyond core 1.0 | `XrHandTrackerEXT`, `xrCreateHandTrackerEXT`/`xrDestroyHandTrackerEXT`/`xrLocateHandJointsEXT`, `XrHandJointLocationEXT[26]` (`XR_HAND_JOINT_SET_DEFAULT_EXT`), `XrSystemHandTrackingPropertiesEXT.supportsHandTracking` | Probed in `xrEnumerateInstanceExtensionProperties`; if present, enabled at `xrCreateInstance` and `xr->handCapability.extEnabled=true`. Final `xr->handCapability.supported` additionally requires the chained `XrSystemHandTrackingPropertiesEXT.supportsHandTracking==true` from `bz_quest_xr_get_system()`. |
| `XR_FB_hand_tracking_aim` | `number="112"`, `depends="XR_VERSION_1_0+XR_EXT_hand_tracking"` | `XrHandTrackingAimStateFB` (chained onto `XrHandJointLocationsEXT.next` at locate time) — a standardized `aimPose` + `status` bitmask (`COMPUTED_BIT_FB`, `VALID_BIT_FB`, `INDEX_PINCHING_BIT_FB`, `MIDDLE_PINCHING_BIT_FB`, plus reserved `RING`/`LITTLE`/`SYSTEM_GESTURE`/`DOMINANT_HAND`/`MENU_PRESSED` bits this layer does not consume — see "Unsupported hand semantics" below). Adds **no new function** — only the struct. | Only probed/enabled when `extEnabled` is ALSO true (its declared dependency); final `xr->handCapability.aimSupported` additionally requires `xr->handCapability.supported`. |

Both extension names, the exact struct/enum layout, and the `depends=`
relationship above were verified directly against the **bundled
`org.khronos.openxr:openxr_loader_for_android:1.1.49`** AAR (the exact
Prefab `headers` module `find_package(OpenXR CONFIG)` resolves — see
"Prerequisites" above): `openxr.h`'s `XR_HAND_JOINT_COUNT_EXT`/
`XrHandJointEXT`/`XrHandTrackerCreateInfoEXT`/`XrHandJointLocationsEXT`/
`XrHandTrackingAimStateFB`/`XrHandTrackingAimFlagsFB` declarations, and the
canonical machine-readable `xr.xml` registry's `<extension>` elements for
`XR_EXT_hand_tracking`/`XR_FB_hand_tracking_aim` (both fetched from
`KhronosGroup/OpenXR-SDK-Source`, the same repo `docs/quest-tabletop.md`'s
"Related documents" already cites for the loader).

A failed per-hand `xrCreateHandTrackerEXT` (e.g. a transient runtime error)
disables only that hand (tracker left `XR_NULL_HANDLE`, logged once); a
failed proc-pointer resolution (the extension enumerated but the runtime's
own advertised functions do not resolve — a genuine runtime defect) disables
hand tracking entirely for the session, logged once. **Neither ever fails
renderer init** — `bz_quest_xr_hands_create()` always returns `true`.
`test-quest-hand-tracking-layout.sh` structurally asserts this "never
returns false on missing capability" contract (see "Tests and build
wiring" below).

**Android manifest gate.** Independent of the OpenXR extension mechanics
above, Quest's OS/runtime broker will not enumerate hand-tracking devices to
**any** app — VrApi or OpenXR — lacking
`<uses-permission android:name="com.oculus.permission.HAND_TRACKING" />` and
`<uses-feature android:name="oculus.software.handtracking" />` in its
manifest (verified against Meta's hand-tracking guidance,
<https://developers.meta.com/horizon/documentation/native/android/mobile-hand-tracking/>,
"Add Flag to Android Manifest to Enable Hand Tracking": *"Apps that do not
specify these flags will not see hand devices enumerated"*). This project's
`AndroidManifest.xml` declares both, with the `uses-feature`'s
`android:required` explicitly `"false"` (unlike the mandatory
`android.hardware.vr.headtracking` feature above it) — this app's
Touch-controller path must keep working, and the app must not be filtered
from a device/account lacking hand tracking, matching this layer's "hand
support must not be required for startup" contract. No other hand-tracking
manifest hint (e.g. the VrApi-only `com.oculus.handtracking.frequency`/
`.version` meta-data tuning knobs some Meta samples set) was added — this
project has no evidence they apply to the OpenXR path at all, and the task
scope is "only what is demonstrably necessary".

### OpenXR lifecycle integration (adds to, never duplicates, `bz_quest_xr.c`/`bz_quest_xr_actions.c`)

- `bz_quest_xr_hands_create()` is called once from `bz_quest_renderer_init()`
  **after** `bz_quest_xr_actions_create()` (both need the session created by
  `bz_quest_xr_create_session()`). No-ops (every tracker left
  `XR_NULL_HANDLE`) when `!xr->handCapability.supported`.
- `bz_quest_xr_hands_sync()` is called once per frame from
  `bz_quest_renderer_process_input()`, immediately after
  `bz_quest_xr_actions_sync()`, into the **same** `bzQuestInputFrame_t` —
  mirroring that function's exact focus/session-running gate
  (`xr->sessionRunning && xr->sessionState == XR_SESSION_STATE_FOCUSED`):
  when not focused, every `frame->handSample[]` entry is left zeroed/
  inactive and `xrLocateHandJointsEXT` is never called (undefined/wasteful
  while unfocused, per the same OpenXR spec reasoning layer 6 already
  documents for `xrSyncActions`). When focused, it locates each active
  hand's joints (+ chained `XrHandTrackingAimStateFB` when negotiated) at
  the frame's predicted display time against `xr->appSpace` — the exact
  same `XrTime`/reference space `bz_quest_xr_actions_sync()` already uses
  for controller poses and `bz_quest_xr_locate_views()` uses for the eye
  views.
- Every joint/aim read honors its own validity bits
  (`XR_SPACE_LOCATION_POSITION_VALID_BIT` per joint,
  `COMPUTED_BIT_FB|VALID_BIT_FB` for the aim struct) before being trusted —
  an untracked/invalid joint or aim state is never treated as usable data,
  mirroring `bz_quest_xr_actions.c`'s `bz_quest_xr_locate_pose()` convention
  exactly (same bit, same "both must be set" check).
- `bz_quest_xr_hands_destroy()` destroys both trackers
  (`XR_NULL_HANDLE`-guarded), called from `bz_quest_renderer_shutdown()`
  alongside `bz_quest_xr_actions_destroy()`, **before** the session/instance
  are torn down by `bz_quest_xr_destroy()` — this exact ordering requirement
  is structurally enforced (see "Tests and build wiring" below; a
  deliberately-injected reversal was proven to fail that check, then
  reverted — see this repo's PR description).

### Pure hand sample/gesture representation (`bz_quest_hand_input.h`)

`bz_quest_hand_sample_build()` turns one hand's raw data into exactly the
same `bzQuestInputHandSample_t` layer 6 already defines (`active`,
`aimValid`, `aimOrigin`/`aimDir`, `gripPos`, `selectDown`, `squeezeDown`,
`primaryDown`, `secondaryDown`, `thumbstick`) — **no new sample type, no
parallel command mapper**. Two tiers, chosen once per session by the
negotiated capability (never re-decided per frame):

- **`BZ_QUEST_HAND_CAPABILITY_FB_AIM`** (preferred, when
  `XR_FB_hand_tracking_aim` was negotiated): `aimOrigin`/`aimDir` come
  directly from `XrHandTrackingAimStateFB.aimPose` (position + the local
  −Z-forward direction, via the shared `bz_quest_quat_forward()` pure
  helper — see below); `selectDown`/`squeezeDown` come directly from the
  runtime's own `INDEX_PINCHING_BIT_FB`/`MIDDLE_PINCHING_BIT_FB` status
  bits, never re-derived from a distance/strength. This matches Meta's own
  documented guidance for the API-equivalent VrApi hand-tracking surface
  (<https://developers.meta.com/horizon/documentation/native/android/mobile-hand-tracking/>,
  "Pinches": *"actual triggering of a pinch event should be based on the
  `ovrInputStateHandStatus_<finger>Pinching` bit being set... [not] the
  `PinchStrength` field"*) — the OpenXR `XR_FB_hand_tracking_aim` bits are
  the same underlying system-level pinch detector's status output.
  `squeezeDown` from the **middle**-finger pinch bit is this layer's one
  evidence-backed second pinch/grab (see "Command mapping" below) — it
  exists because the extension itself exposes a dedicated bit for it, not
  because this layer invented a gesture.
- **`BZ_QUEST_HAND_CAPABILITY_EXT_ONLY`** (fallback, base extension only):
  no aim pose or pinch bit exists at this tier, so:
  - **Ray basis**: origin = the index fingertip
    (`XR_HAND_JOINT_INDEX_TIP_EXT`) position; direction = the normalized
    vector from the index metacarpal (`XR_HAND_JOINT_INDEX_METACARPAL_EXT`)
    to the index fingertip — the finger's own, long-baseline (~8-10 cm)
    pointing direction. This is grounded **only in tracked joint
    positions**, deliberately never a joint's orientation quaternion: the
    OpenXR hand-joint orientation-axis convention lives only in the spec's
    prose "Convention of Hand Joints" section, which this environment could
    not fetch/verify (no internet access to the ratified prose spec beyond
    the machine-readable registry/header — see AGENTS.md's "never guess"
    rule, which applies equally to a new piece of geometry, not just a bug
    fix). Grounding the ray in two real joint *positions* instead needs no
    unverified convention and is still "a stable ray basis grounded in
    tracked joint poses" per this layer's task contract.
  - **Pinch**: the thumb-tip/index-tip Euclidean distance, with an explicit
    engage/release hysteresis band (`BZ_QUEST_HAND_PINCH_ENGAGE_M`=2.5 cm /
    `BZ_QUEST_HAND_PINCH_RELEASE_M`=3.5 cm) so a fingertip gap sitting
    between the two thresholds can never chatter `selectDown` frame to
    frame — the same shape of hysteresis this layer's source arbitration
    debounce uses (see below). **Unvalidated on real hardware** (no
    physical Quest available — see "Hardware-only acceptance gates"), a
    deliberately bounded, documented, trivially-tunable estimate, exactly
    like this project's existing haptic-pulse/board-rate constants already
    are.
  - **`squeezeDown` is ALWAYS false** at this tier — no second pinch/grab
    signal exists without `XR_FB_hand_tracking_aim`'s dedicated bit, and
    this layer does not invent a middle-finger (or any other) distance
    threshold to approximate one; see "Unsupported hand semantics" below.
  - `gripPos` (the board-pan grip-pose analog) is always the wrist joint
    (`XR_HAND_JOINT_WRIST_EXT`) position, at **either** capability tier —
    the closest tracked point to where a physical controller actually sits
    in the hand, and the only joint this module reads purely for its
    position-as-a-rigid-anchor property (used for delta-drag, never for a
    direction).
- **`BZ_QUEST_HAND_CAPABILITY_NONE`**: `out` is fully zeroed/inactive —
  no hand tracking this session at all.

`bz_quest_hand_sample_build()` is **frame-critical**: it allocates nothing,
locks nothing, does no file I/O, calls no bridge/transport API, and calls no
logging function, since it runs once per hand every frame on the XR render
thread — structurally guarded by
`test-quest-hand-tracking-layout.sh` (mirroring
`test-quest-audio-rt-callback-safety.sh`'s technique for the AAudio
callback).

`bz_quest_quat_forward()` (the shared −Z-forward quaternion rotation) was
extracted from `bz_quest_xr_actions.c`'s previously-private
`bz_quest_xr_quat_forward()` into `bz_quest_pure.h`/`.c` so both the
controller aim ray and this layer's `XR_FB_hand_tracking_aim` ray share one
tested implementation (AGENTS.md's DRY rule) — proven identical to the
pre-existing rotation via `bz_quest_pose_to_view_matrix()`'s own rotation
matrix (see `test_bz_quest_pure.c`'s
`test_quat_forward_matches_view_matrix_rotation_column`).

### Deterministic source arbitration (`bz_quest_input_state.h`'s `bzQuestInputSourceKind_t`)

Per-hand, per-frame, an enum — never two booleans (AGENTS.md) — decides
which sample (`frame->hands[h]`, the Touch controller, or
`frame->handSample[h]`, the hand-tracking gesture) is authoritative:

```
                 controllerActive
                        │
        ┌───────────────┴────────────────┐
       true                             false
        │                                 │
   CONTROLLER                    accumulate controllerLossSeconds
  (loss reset to 0)                       │
                              ┌───────────┴────────────┐
                        was HAND already?          was CONTROLLER
                          │        │                    │
                      handActive  !handActive     loss>=DEBOUNCE(0.3s)
                          │            │            &&  handActive?
                        HAND      CONTROLLER          │        │
                                                     true     false
                                                      │          │
                                                    HAND     CONTROLLER
```

- **Touch reclaims instantly** — the instant `controllerActive` is true
  again, arbitration reports `CONTROLLER` with **no debounce**: the
  preferred device coming back is never second-guessed.
- **Hand takeover is debounced** (`BZ_QUEST_HAND_SOURCE_SWITCH_DEBOUNCE_SEC`
  = 0.3 s, ~22 frames at 72 Hz): a hand sample only becomes authoritative
  once the controller has been **continuously** inactive for the whole
  window — a single dropped-frame controller tracking blip can never bounce
  the source to hand and back. Unvalidated on real hardware, a deliberately
  bounded, documented, trivially-tunable constant (see
  `test_bz_quest_input_state.c`'s `test_arbitrate_debounces_before_handoff`/
  `test_arbitrate_hands_off_after_debounce_elapses`, which prove the exact
  boundary with real per-frame `dt` accumulation, not one artificial large
  step).
- **Losing the hand falls back to CONTROLLER immediately** (never a third
  "none" state) — the enum's steady state then matches exactly what a
  build with hand tracking disabled entirely already had, which is *why*
  every one of layers 6/7's pre-existing tests keeps passing unmodified
  with this layer stacked on top (`frame->handSample[]` defaults to fully
  zeroed/inactive in every caller that never populates it, so arbitration
  can never produce `HAND` there at all — see
  `test_controller_only_frame_unaffected_by_arbitration`).
- **A source change for either hand clears exactly once** — folded into the
  SAME idempotent-clear check layer 6 already runs for focus loss/
  controller loss/map reload
  (`bz_quest_input_state_update()`'s `if (state->source[h] != newSource[h])
  clear = true`), independent of whether `active` itself happens to blip
  false in between (it does not always — see
  `test_source_switch_clears_exactly_once`). This resets every edge latch
  and the board-pan drag anchor exactly once on the switch frame
  (`test_source_switch_clears_board_drag`), never re-fires while the new
  source persists, and — per the SAME "reset, then the next observed
  transition is a fresh press" rule this codebase's existing controller
  reconnect test already establishes
  (`test_focus_reconnect_map_reset_with_left_grip_held_never_posts`'s
  "re-anchors (new rising edge)") — a hand sample that is *already* pressed
  the very frame it becomes authoritative correctly fires once, then never
  repeats while held (`test_source_switch_requires_fresh_edge_no_repeat_while_held`).
- Arbitration happens **once, at the very top** of
  `bz_quest_input_state_update()`, into a local `bzQuestInputFrame_t`
  copy — every hit-test/board-manipulation/command-mapping line below is
  byte-for-byte unchanged from layer 6/7 and never inspects which source
  won.

### Command mapping (reuses layer 6's table unchanged — no parallel state machine)

| Hand gesture | Maps to (same as controller) | Capability required |
|---|---|---|
| Index pinch (`selectDown`) | Select / smart-point / target-point / HUD button / HUD cancel — exactly `bz_quest_input_state.c`'s existing `kTargetTable` | Either tier |
| Middle pinch (`squeezeDown`, right hand) | Smart-entity order (mirrors right controller grip) | `FB_AIM` only |
| Middle pinch + wrist-position drag (`squeezeDown`+`gripPos`, left hand) | Board pan (mirrors left controller grip-drag) — reuses `bz_update_board()` unchanged | `FB_AIM` only |
| — (no hand analog) | Cancel via secondary button, board yaw/zoom/height (thumbstick), menu-reset | Neither — see below |

**HUD/entity/terrain hit-test priority is completely unchanged** — a hand
sample flows through the exact same `bz_quest_input_hit_test()` (HUD >
disabled-consumed > nearest entity > terrain > none) layer 6 already proved,
since arbitration hands it an identical `bzQuestInputHandSample_t` shape
regardless of source (`test_hand_source_selects_entity_when_controller_inactive`,
`test_hand_reaches_hud_cancel_without_secondary_button`).

**Unsupported hand semantics (intentional, not bugs):**

- **Cancel without a secondary button.** Hand tracking has no B/Y-button
  analog, but cancel remains fully reachable: the HUD's own synthetic
  Cancel region is hit-tested and selected via `selectDown` exactly like
  any other HUD button (`BZ_QUEST_INPUT_HIT_HUD_CANCEL` →
  `BZ_QUEST_INPUT_CMD_CANCEL`) — `secondaryDown` simply always stays false
  for a hand sample, and the redundant hardware-button shortcut just never
  fires for hands (proven directly by
  `test_hand_reaches_hud_cancel_without_secondary_button`).
- **Board rotate/zoom/height (thumbstick-driven) is controller-only.**
  OpenXR hand-tracking joints provide poses only, no 2-axis analog-stick
  equivalent — `bzQuestInputHandSample_t.thumbstick` is always `{0,0}` for
  a hand sample (`test_hand_only_frame_cannot_rotate_zoom_or_height_board`
  proves this holds even with both hands fully hand-tracked and active).
  Left-hand grip-drag PAN, by contrast, has a natural hand equivalent
  (`squeezeDown`+`gripPos`, see the table above) and IS reachable at the
  `FB_AIM` tier.
- **Menu-reset (left menu click) is controller-only.**
  `XR_HAND_TRACKING_AIM_MENU_PRESSED_BIT_FB` exists in the OpenXR registry,
  but this layer deliberately does **not** wire it to the board-reset
  action: its precise trigger semantics are not described anywhere this
  environment could verify (only the bit's name, not ratified prose), and
  it sits immediately adjacent to the reserved
  `XR_HAND_TRACKING_AIM_SYSTEM_GESTURE_BIT_FB` in the same status mask —
  mirroring this project's existing "reserved-button rule" (the Oculus/
  system button is never bound on controllers either), the conservative,
  evidence-respecting choice is to leave it unmapped rather than guess.
  Menu-reset remains reachable via the left Touch controller.
- **A second pinch/grab is EXT-only-tier absent, not invented.** With only
  `XR_EXT_hand_tracking` negotiated, there is no dedicated second-pinch
  signal at all — this layer does not approximate one via a middle-finger
  distance threshold of its own (which would be indistinguishable from
  "inventing a gesture" the task explicitly forbids); `squeezeDown` simply
  stays false, and smart-entity/board-pan remain controller-only at that
  capability level.
- **No hand mesh.** Neither `XR_MSFT_hand_tracking_mesh` nor
  `XR_FB_hand_tracking_mesh` is referenced anywhere in this layer — out of
  scope per the task contract ("do not add hand mesh rendering unless
  required"), structurally guarded (see below).

### Rendering (reuses layer 6's pointer/reticle unchanged)

This layer adds **no new shader, no new Vulkan pipeline, and no new
renderer module.** `bz_quest_vk_wc3_pointer.c` (layer 6) already draws a
per-hand ray + hit-kind-tinted reticle from `bzQuestInputFeedback_t`; the
only change is where that struct's per-hand aim ray now comes from.
Previously the renderer re-read its own `frame.hands[h]` (the raw,
pre-arbitration Touch sample) to build the beam/reticle geometry — a latent
bug this layer's own implementation surfaced and fixed: once arbitration
can legitimately swap in a hand's sample, re-reading the controller-only
copy would show the ray frozen at the (possibly inactive) controller's last
pose instead of following the hand. `bzQuestInputFeedback_t` now also
carries the **arbitrated** `aimOrigin`/`aimDir` per hand (populated by the
same `bz_hand_hit()` that already computes `visible`/`hitKind`/`reticle`
from the resolved sample), and `bz_quest_renderer_process_input()` reads
those instead of `frame.hands[]` directly — this is the one minimal,
hand-specific addition to feedback state the task's "reuse existing
pointer/HUD rendering" scope permits, not a new rendering path. The
reticle's hit-kind tint (amber HUD, green entity, cyan terrain, dim
no-hit) is unchanged and applies identically regardless of source.

### Quest-vs-visionOS differences (intentional, documented, not gaps)

- **visionOS's direct/indirect pinch model does not fully carry over.**
  visionOS's `TabletopControls.swift` distinguishes a *direct* pinch (hand
  reaching into a UI element's hit volume) from an *indirect* pinch (a
  pinch performed anywhere, aimed via gaze/hand ray at a distant target) —
  RealityKit provides both natively. OpenXR's `XR_FB_hand_tracking_aim`
  gives Quest an **indirect-only** model: one synthesized aim ray + pinch
  bits, with no separate "am I reaching into this element" signal. This
  layer therefore only implements the indirect-equivalent path (ray +
  pinch, exactly like a controller trigger), and does not attempt to
  fabricate a direct/proximity pinch Quest's OpenXR surface cannot prove —
  matching this project's "match visionOS ... only where Quest can prove
  parity" contract.
- **No hand mesh/skeleton visualization**, unlike some visionOS reference
  hand-presence UIs — out of scope for this layer, and this project's
  in-game HUD/board rendering never needed one for controllers either.
- **Second pinch/grab exists on Quest (middle-finger bit) but not on
  visionOS's ARKit hand-tracking surface** (which exposes only a single
  index-pinch-equivalent chirality gesture) — Quest's `FB_AIM` tier is
  therefore *more* capable here, not less; this asymmetry is called out
  explicitly rather than silently matched or silently ignored.

### Supported vs. unsupported behavior

| Behavior | Status |
|---|---|
| Index pinch → select/smart-point/target-point/HUD button/HUD cancel, both hands, either capability tier | Supported |
| Middle pinch → smart-entity (right) / board-pan (left, with wrist-position drag) | Supported — `XR_FB_hand_tracking_aim` only |
| Deterministic Touch-preferred, hysteresis-debounced source arbitration per hand | Supported |
| HUD > entity > terrain hit-test priority, identical regardless of source | Supported |
| Ray/reticle visualization for hand-sourced input, reusing layer 6's pointer pipeline | Supported |
| Board yaw/zoom/height (thumbstick), menu-reset | **Controller-only** — no reliable hand-tracking analog exists (see "Unsupported hand semantics") |
| Cancel via a dedicated hand gesture | **Not needed** — reachable via the HUD's own Cancel region instead |
| A second pinch/grab without `XR_FB_hand_tracking_aim` | **Not implemented** — no evidence-backed signal exists at the EXT-only tier; not approximated |
| Hand mesh/skeleton rendering | **Out of scope** for this layer |
| Direct (proximity) pinch, matching visionOS's dual pinch model | **Not implemented** — OpenXR's `XR_FB_hand_tracking_aim` exposes only an indirect-equivalent ray+pinch model |

### Tests and build wiring

- `platform/android/quest/tests/test_bz_quest_hand_input.c` — the pure
  gesture-builder suite (22 tests): both hands independently, capability
  NONE/EXT_ONLY/FB_AIM, tracker active/inactive, every joint individually
  valid/invalid (including a degenerate zero-length ray-direction
  rejection), the FB-aim path (index/middle pinch bits used directly, aim
  pose passthrough, aim-invalid producing no output), the EXT-only path
  (ray basis from real joint positions, pinch engage/release/no-chatter
  hysteresis at and around both thresholds, thumb-validity gating,
  `squeezeDown` always false), tracker loss/reacquisition requiring a fresh
  ENGAGE crossing (never resuming from a mid-hysteresis gap), and the
  intentionally-zero unsupported fields (`primaryDown`/`secondaryDown`/
  `thumbstick`).
- `platform/android/quest/tests/test_bz_quest_input_state.c` — extended
  with 16 new arbitration/integration tests: the `bz_quest_input_arbitrate_source()`
  state machine directly (instant controller reclaim, debounced handoff
  with real per-frame `dt` accumulation, no handoff without an active hand,
  falling back from HAND when the hand itself drops, no re-debounce once
  already on HAND), then full `bz_quest_input_state_update()` integration:
  a hand pinch selecting an entity once the controller is inactive long
  enough, the controller winning even when a hand is simultaneously
  pressed, exactly-once clearing on a source switch (with the precise
  float-accumulation boundary verified, not assumed), a fresh-edge-fires-
  once-then-no-repeat switch, a board-pan drag dropped by a switch, a hand
  driving board-pan via middle-pinch+wrist-drag, hands unable to
  rotate/zoom/height the board, cancel reachable via a hand pinch at the
  HUD's Cancel region without ever touching `secondaryDown`, and an
  explicit "controller-only frame behaves exactly as before" regression
  guard. **All pre-existing layer 6/7 tests in this file pass completely
  unmodified** — every one leaves `frame.handSample[]` at its zeroed
  default, so arbitration can only ever resolve to `CONTROLLER` there (see
  "Deterministic source arbitration" above for why this is provably true,
  not merely observed).
- `platform/android/quest/tests/test_bz_quest_pure.c` — 4 new tests for the
  extracted `bz_quest_quat_forward()` helper (identity, a 90° yaw case
  cross-checked by hand, a cross-check against
  `bz_quest_pose_to_view_matrix()`'s own rotation matrix at a non-trivial
  quaternion, and a unit-length invariant).
- All of the above run under `make test-quest-host-tests` — **5243/5243
  assertions pass** (up from the base branch's 5107/5107 — 136 new
  assertions across 42 new test functions), see "What was verified this
  session" below for the exact command output.
- `platform/android/quest/scripts/test-quest-hand-tracking-layout.sh` (wired
  into `make test`/`make quest` as `test-quest-hand-tracking-layout`)
  structurally guards: the CMakeLists.txt source list; that both extension
  names are probed and the aim extension is only ever gated alongside the
  base extension (never standalone); that `bz_quest_xr_hands_create()`'s
  unsupported-capability path returns `true` (never fails startup); that
  `xrCreateHandTrackerEXT`/`xrDestroyHandTrackerEXT` are present, paired,
  and `XR_NULL_HANDLE`-guarded; that `xrLocateHandJointsEXT` is called
  exactly once, gated on the same focus/session-running check the action
  module uses; that `bz_quest_hand_sample_build()`'s body contains no
  allocation/lock/log/file/bridge call (mirroring
  `test-quest-audio-rt-callback-safety.sh`'s technique for the real-time
  audio callback); that the renderer wires create/sync/destroy and destroys
  hand trackers strictly before the OpenXR session/instance; that no
  hand-mesh extension/type is referenced anywhere; and that the manifest
  declares the permission/feature with the feature marked optional. Two
  violations were deliberately injected and confirmed caught by this
  script during development, then reverted (a forbidden `malloc()` inside
  the frame-critical gesture builder, and a reversed hand-tracker/session
  teardown order) — see this repository's PR description for the exact
  evidence.
- `CMakeLists.txt`'s `bz_quest_native` source list includes the two new
  `.c` files (`bz_quest_hand_input.c`, `bz_quest_xr_hands.c`). No new
  compile-time `BZ_QUEST_ENABLE_*` seam was added — see "Capability
  negotiation" above for why.

### Acceptance gates

This layer's **pure** logic (gesture builder, source arbitration, command
mapping reuse) is fully host-verified by the tests above. The **impure**
OpenXR hand-tracking glue was compiled/linked for the real arm64-v8a target
via the project's Gradle/CMake build and checked by
`quest-verify-native-lib`, but was **not** run on a physical device — see
"Hardware-only acceptance gates" below, which now also lists every
hand-tracking-specific item that requires real Quest 3/3S hardware+OS-level
hand tracking to actually confirm.

### Troubleshooting

- **Hand rays never appear on-device.** Check, in order: (1) the Quest
  OS-level "Hands and Controllers" setting has hand tracking enabled for
  this app/account (a manifest-only requirement cannot force this); (2)
  logcat for `"XR_EXT_hand_tracking not supported this session"` (capability
  negotiation failed — either the manifest permission/feature is missing,
  the runtime doesn't advertise the extension, or the system genuinely
  reports no support); (3) that at least one controller is actually
  inactive/put-down long enough (0.3 s) for arbitration to hand off — while
  a controller is tracked, hands are correctly never consulted.
- **Ray direction looks wrong at the EXT-only tier.** This tier's ray
  direction is the index-metacarpal→index-tip vector, not a joint
  orientation — check the physical index finger's own pointing direction,
  not the palm's facing direction; these can visibly differ when the
  finger is curled.
- **Middle-pinch smart-entity/board-pan never triggers.** Confirm
  `XR_FB_hand_tracking_aim` is actually negotiated (logcat: `"hand tracking
  enabled (XR_FB_hand_tracking_aim)"` vs. `"... (XR_EXT_hand_tracking
  only)"`) — this gesture is intentionally EXT-only-tier absent, not a bug.
- **A hand ray flickers on/off rapidly.** Expected at the very edge of the
  hand-tracking camera's field of view or under poor lighting — the
  `isActive`/joint-validity bits are read as-is every frame, with no
  additional smoothing beyond the documented pinch-distance and
  source-switch hysteresis bands (see "Deterministic source arbitration").

### Exact on-device acceptance procedure (requires a connected Quest 3/3S)

```sh
# 1. Build + install (see "Build/run/log commands" above for env setup).
make quest-assemble-debug
adb install -r platform/android/quest/app/build/outputs/apk/debug/app-debug.apk
# 2. Confirm the device/account has hand tracking enabled in Quest OS
#    settings (Settings > Movement Tracking > Hands and Controllers), then
#    launch and tail this app's log tag.
adb shell am start -n <package>/android.app.NativeActivity
adb logcat -s bz_quest_native:V
# 3. With a real map loaded, verify by hand (each is hardware-only, unproven here):
#    - logcat shows "hand tracking enabled (...)" once at startup, never
#      per-frame, and shows which capability tier (EXT-only vs FB aim);
#    - putting BOTH controllers down: after a brief (~0.3s) pause, both hand
#      rays appear, tinted/behaving identically to the controller rays they
#      replaced (amber HUD, green entity, cyan terrain, dim no-hit);
#    - an index pinch aimed at a unit selects it (a LATER-frame selection
#      marker, never instant/local); aimed at empty terrain issues a
#      smart-point order; aimed at a command-card button posts its action;
#      aimed at the HUD Cancel region cancels - all with no B/Y button;
#    - (if XR_FB_hand_tracking_aim is active - check logcat) a right-hand
#      middle pinch over a unit issues a smart-entity order; a left-hand
#      middle pinch + physical hand drag pans the board (grip-drag feel,
#      not a controller trigger feel);
#    - board rotate/zoom/height (right/left thumbstick equivalents) do NOT
#      respond to any hand gesture - picking a controller back up restores
#      them instantly;
#    - picking a controller back up INSTANTLY reclaims that hand's ray (no
#      debounce), with no phantom command fired from whatever the hand was
#      doing the instant before;
#    - removing the headset (focus loss) and putting it back on resumes
#      cleanly with no stuck/latched hand-sourced command;
#    - capture a bounded logcat window (e.g. `adb logcat -d -s
#      bz_quest_native:V | tail -500`) across a hand-tracking session and
#      confirm no per-frame hand-tracking log line appears - only the
#      one-time "hand tracking enabled"/capability-unavailable line and any
#      once-logged xrLocateHandJointsEXT failure.
```

**No physical Meta Quest device was available in this development
environment** — every item in this procedure is written from the OpenXR
1.1.49 registry/headers, Meta's own hand-tracking guidance, and this
layer's own pure-logic test evidence, not from having run it. Do not report
any of it as confirmed until checked on real hardware.

## Manifest requirements

Unchanged from layer 2: `AndroidManifest.xml`'s NativeActivity metadata,
`com.oculus.supportedDevices` scoped to `quest3|quest3s`, and headtracking
`uses-feature` were verified against Meta's mobile-native manifest guidance
(<https://developers.meta.com/horizon/documentation/native/android/mobile-native-manifest/>).
No `horizonos:uses-horizonos-sdk` element (Spatial SDK panel-app specific,
not applicable to a plain OpenXR NativeActivity app). **New this layer**:
`com.oculus.permission.HAND_TRACKING` and the optional
(`android:required="false"`) `oculus.software.handtracking` `uses-feature`
were verified against Meta's hand-tracking guidance
(<https://developers.meta.com/horizon/documentation/native/android/mobile-hand-tracking/>)
— see "[Layer 8](#layer-8-meta-quest-hand-tracking-bz_quest_xr_handsc-bz_quest_hand_inputc-bz_quest_input_statec)"'s
"Capability negotiation" above for the full citation and why `required` is
explicitly `false` here.

## Package/app identifiers

Unchanged from layer 2: `org.openrealm.quest` / `bz_quest_native` /
`openwarcraft3-*` are project-private placeholders for this sideloaded
debug prototype. Replace before any wider distribution.

## Current limitations

Everything below is explicitly out of scope for this layer; each has a
compile-time `#error`-guarded seam in `bz_quest_host.c`
(`BZ_QUEST_ENABLE_DATA_STAGING`) so a later layer can flip it on only if it
ever grows a real in-app meaning, instead of a silent stub reporting fake
success. `BZ_QUEST_ENABLE_VULKAN_RENDERER`, `BZ_QUEST_ENABLE_ENGINE_START`,
`BZ_QUEST_ENABLE_BRIDGE_SNAPSHOTS`, `BZ_QUEST_ENABLE_WC3_RENDERER`,
`BZ_QUEST_ENABLE_INPUT`, and `BZ_QUEST_ENABLE_AUDIO` are all hard-required to
`1` (a `#error` fires if a build tries to set any of them to `0`) since the
Warcraft renderer, procedural renderer, tabletop bridge, OpenXR Touch input,
and AAudio output are no longer optional. **Layer 8 (hand tracking) adds no
new compile-time seam at all** — see its "Capability negotiation" section
above for why runtime OpenXR negotiation replaces a compile-time gate here:

- Touch-controller input (layer 6) and AAudio output (layer 7) are now both
  present. War3 MPQ data staging is provided as a developer-time ADB script
  (`platform/android/quest/scripts/stage-wc3-data.sh`, see
  [Layer 7](#layer-7-warcraft-iii-data-staging--native-aaudio-output) above)
  rather than an in-app feature — this is a deliberate design choice (the
  user's own purchased game data is never bundled/copied by this project),
  not a gap.
- **Hand tracking (layer 8) is now present, alongside Touch controllers,
  never replacing them.** Optional `XR_EXT_hand_tracking`/
  `XR_FB_hand_tracking_aim` capability negotiation, per-hand
  `XrHandTrackerEXT` lifecycle, the pure gesture builder, and deterministic
  Touch-preferred source arbitration are all implemented and fully
  host-tested (see [Layer 8](#layer-8-meta-quest-hand-tracking-bz_quest_xr_handsc-bz_quest_hand_inputc-bz_quest_input_statec)
  above) — but no physical Quest device was available to confirm any of it
  actually tracks/pinches/arbitrates correctly against a real hand; a hand
  mesh/skeleton is intentionally never rendered.
- **Staged TFT archives are now mounted at runtime.** `bz_quest_data_build_argv()`
  emits the `"-tft"` dash-flag when `bz_quest_data_detect_edition()` finds a
  `War3x*.mpq` in the resolved directory, so `fs_expansion` is `1` before
  `FS_AddDataDirectory()` scans it — see
  [Layer 7](#layer-7-warcraft-iii-data-staging--native-aaudio-output)'s
  "Engine startup argv construction" for the detection/argv/log-evidence
  design and why this must be a startup dash-flag rather than a later
  `+fs_expansion 1`.
- **Spatial/positional audio (Meta XR Audio SDK) is out of scope for this
  prototype** — every voice plays as flat, non-positional stereo through
  the device's built-in speakers/output route; see [Layer 7](#layer-7-warcraft-iii-data-staging--native-aaudio-output)'s
  "Supported vs. unsupported audio behavior" table.
- No map is ever loaded (`bz_quest_bridge_start()` always passes a `NULL`
  map name) — see "Known limitations of this frame descriptor" above. With
  no map loaded, `BZ_TT_Latest()` returns an entity-less snapshot, so layer
  5A's captured render list is always empty in this environment and every
  eye renders the procedural diagnostic scene (see "Diagnostic-scene
  fallback is explicit, not silent" above) — this was never observed
  rendering real Warcraft geometry on a live snapshot in this environment
  (see "Hardware/data-only acceptance procedure" below). With no map
  loaded, `S_PlaySoundFile()` also never actually fires in this
  environment — see "Hardware/data-only acceptance procedure" below for
  what was, and was not, exercised for audio this session.
- Terrain (layer 5B), model animation (layer 5C), fog of war, per-entity
  selection markers (layer 5D), and the status/command-card HUD (layer 5E)
  are now all present; particles/effects and any renderable player
  target-point/entity overlay are still out of scope — see the layer
  sections above for the exact, deliberate boundaries and the evidence for
  the "target mode has no location" no-op.
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
- Gameplay controller input (layer 6) now reaches the engine via the typed
  `BZ_TT_Post*` transport (select/smart/target/cancel/button) — but only the
  point-based target path exists (no `BZ_TT_PostTargetEntity` in the ABI; see
  "Layer 6"'s command-mapping table for the documented workaround), and no
  command round-trip was verified on-device or against real map data.
- No Vulkan multiview, MSAA, or fixed foveation (`XR_FB_foveation`) — see
  "Vulkan render pass/pipeline/targets" above for why this is an explicit,
  documented seam rather than an oversight.
- The tabletop asset ABI **was** widened once, in layer 5C: `bzTTAsset_t`
  went from v2 (layers 5A/5B's static geometry/materials + terrain) to v3
  (node hierarchy/keyframe tracks/sequences/global sequences/geoset alpha),
  after concrete evidence showed v2 exposed no animation data whatsoever —
  see "ABI decision: extended in place (v2 → v3), not tunneled" above. No
  further widening has occurred since (layer 7 made no ABI change at all —
  `platform/bridge/bz_tabletop_audio.h` was consumed exactly as-is); camera-
  facing billboarding and texture-coordinate/material-ID animation (TXAN/
  KMTF) remain outside the ABI's exposed surface — see "Layer 5C"'s
  "Supported vs. unsupported animated behavior" table above.

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
- **New this layer (7)** — none of the following was verified against a
  real Meta Quest device, a real adb-attached Android install, or real
  retail Warcraft III data; **do not report any of these as confirmed**
  until checked with real hardware/data (see "Exact acceptance commands"
  at the end of the Hardware/data-only acceptance procedure below):
  - Whether `stage-wc3-data.sh` actually transfers real files over a real
    `adb`/`run-as` connection to a real installed debug APK — this session
    only exercised it against `test-stage-wc3-data.sh`'s fake `adb`/
    `run-as`/`pm`/`df`/`sha256sum`/`cp` shims (no real device attached),
    which prove the script's *logic* (quoting, atomicity, ROC/TFT
    validation, idempotency, error paths) but not real `adb`/`run-as`
    behavior on a real Quest OS build.
  - Whether a real staged ROC or TFT-over-ROC data directory is actually
    discovered, mounted, and loaded successfully end-to-end by
    `BZ_RuntimeInit()`/`FS_AddDataDirectory()` on-device with real
    `War3.mpq`/`War3x.mpq`/`War3xlocal.mpq` archives — no retail Warcraft
    III data was available in this environment; only synthetic fixture
    bytes were used in `test-stage-wc3-data.sh`.
  - Whether the AAudio stream actually opens, starts, and produces audible
    sound through the Quest's real output route on real hardware — this
    session verified the AAudio sink compiles against the real NDK headers
    (see "What *was* verified this session" below) and that its pure
    parsing/mixing/lifecycle logic is correct on the host, but no real
    `AAudioStream_open()`/`_requestStart()` call was ever executed against
    real AAudio hardware/drivers.
  - Whether a real AAudio disconnect (e.g. a Bluetooth/wired route change)
    is actually delivered to `bz_quest_audio_error_callback()` and
    correctly triggers `bz_quest_audio_drain()`'s close+reopen+restart path
    on a real device — only the pure lifecycle state-machine transitions
    this depends on were host-tested.
  - Whether a real Warcraft III sound asset (a real `.wav` payload
    extracted from a real MPQ) actually parses successfully through
    `bz_quest_wav_parse()` and sounds correct once mixed/resampled and
    played — the parser's format/bounds/consistency rules were derived
    from the WAVE spec and tested against synthetic fixture bytes built to
    match those rules, not against a real extracted Warcraft III `.wav`
    file (none was available in this environment).
- **New this layer (8)** — none of the following was verified against a
  real Meta Quest device or a real human hand; **do not report any of these
  as confirmed** until checked on real Quest 3/3S hardware with hand
  tracking enabled (see "Exact on-device acceptance procedure" in
  [Layer 8](#layer-8-meta-quest-hand-tracking-bz_quest_xr_handsc-bz_quest_hand_inputc-bz_quest_input_statec)
  above):
  - Whether `xrCreateHandTrackerEXT`/`xrLocateHandJointsEXT` actually
    succeed and return plausible joint data against a real Quest 3/3S
    OpenXR runtime with hand tracking enabled at the OS level — only
    compile-time/syntax-verified against the extracted OpenXR AAR headers
    (and against the real NDK/Vulkan headers via a syntax-only cross-
    compile) on this machine, never executed against a runtime.
  - Whether `XR_FB_hand_tracking_aim` is actually negotiated/populated by a
    real Quest 3/3S runtime, and whether its `aimPose`/pinch bits feel
    accurate and responsive to a real human hand — this session could not
    determine which capability tier a real device would even reach.
  - Whether the EXT-only fallback's joint-position-only ray basis and
    pinch-distance thresholds (`BZ_QUEST_HAND_PINCH_ENGAGE_M`/`_RELEASE_M`)
    feel natural/accurate against a real hand — these are documented,
    bounded estimates verified only against synthetic fixture geometry in
    `test_bz_quest_hand_input.c`, never against real hand-tracking camera
    output.
  - Whether the 0.3 s controller-vs-hand source-arbitration debounce
    (`BZ_QUEST_HAND_SOURCE_SWITCH_DEBOUNCE_SEC`) feels responsive/correct
    in practice (avoids visible oscillation without feeling sluggish when
    setting a controller down) — only proven not to oscillate against
    synthetic per-frame `dt` sequences on the host.
  - Whether a real controller-to-hand or hand-to-controller handoff, mid-
    gameplay, ever produces a visually/functionally surprising result (a
    ray jump, a missed press, a double-fire) that the pure state machine's
    test suite did not anticipate — only the documented, tested transitions
    were exercised.
  - Whether removing/wearing the headset (focus loss/regain) while hands
    (not controllers) are the active source behaves identically to the
    already-verified controller focus-loss/regain path — the underlying
    clear mechanism is shared and host-tested, but the full real Android/
    OpenXR event sequence with hands active was never exercised on-device.

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

**C. Data staging + audio acceptance (layer 7, requires a real connected
Quest 3/3S with the debug APK installed and a developer's own local
ROC/TFT data directory).** Not run in this environment (no device/data
available) — this is the exact procedure and expected output:

```sh
# 1. Build + install (see "Build/run/log commands" above).
JAVA_HOME=/path/to/temurin-17 make quest-assemble-debug
JAVA_HOME=/path/to/temurin-17 make quest-install-debug

# 2. Stage a ROC-only directory (exactly War3.mpq) or a TFT-over-ROC
#    directory (War3.mpq + War3x.mpq + War3xlocal.mpq) from your own local,
#    user-owned Warcraft III install.
make quest-stage-wc3-data BZ_QUEST_WC3_DATA=/path/to/your/wc3/data
# Expect: one "staged <name> (<N> bytes, sha256 <hash>)" line per archive,
# then the app's private data root/override file printed at the end.

# 3. Confirm what is actually staged (no transfer, read-only).
make quest-verify-wc3-data
# Expect: each archive's size/sha256 and the override file's exact
# contents (an absolute path ending in ".../files/Warcraft III").

# 4. Re-run step 2 unchanged - expect every archive to report "unchanged,
#    skipping" (idempotent skip, no re-transfer).
make quest-stage-wc3-data BZ_QUEST_WC3_DATA=/path/to/your/wc3/data

# 5. Launch and tail the log tag (see "Exact on-device acceptance
#    procedure" above for the full expected sequence); log line 10 should
#    now read `bz_quest_bridge_start succeeded (data dir '.../Warcraft III')`
#    instead of the failure line "Hardware-only acceptance gates" A above
#    describes, since a valid data directory is now staged.
make quest-run
make quest-log

# 6. With a real map loaded and an in-game sound-triggering action taken
#    (e.g. selecting/ordering a unit), confirm audible, non-spatial stereo
#    sound plays through the headset's speakers/output route with no
#    stutter/dropout, and that pausing the app (removing the headset or
#    pressing the Oculus button) stops sound cleanly with no stuck/looping
#    audio on resume - all hardware-only, unproven here.

# 7. Clean up (requires explicit confirmation - only removes this app's
#    own private Warcraft III data subdirectory and override file).
make quest-clean-wc3-data
```

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

### What *was* verified this session (layer 7)

- `stage-wc3-data.sh` and its fake-device test harness
  `test-stage-wc3-data.sh` were iterated against each other until all 14
  cases passed (`make test-quest-stage-wc3-data`), including diagnosing and
  fixing two real bugs found only by actually running the harness (not
  assumed/guessed): (1) the fake `adb push`/`run-as` filesystem mapping
  initially let an absolute `/data/local/tmp/...` path inside a `run-as`-
  wrapped command resolve against the real host filesystem instead of the
  fake device root — fixed by making the script's remote bounce directory
  an overridable variable (`BZ_QUEST_STAGE_REMOTE_TMP`, defaulting to the
  real `/data/local/tmp` in normal use) so the test harness can point it at
  a real, harness-owned host directory instead; (2) a `TMPDIR` with a
  trailing slash on this host produced a double-slash in the harness's own
  expected-path string that a real `pwd`-based path (used by the override-
  file write) naturally collapses away — fixed by canonicalizing the
  harness's scratch directory once via `cd ... && pwd` at startup. Also
  fixed a test-isolation bug where every test shared one fake device root,
  so an earlier test's staged files could make a later test's assertions
  pass or fail for the wrong reason — fixed by resetting the fake device's
  package-root and remote-tmp directories at the start of every test.
- The real AAudio-owning `bz_quest_audio.c`/`.h` (plus
  `bz_quest_wav.c`/`.h`, `bz_quest_audio_mixer.c`/`.h`,
  `bz_quest_audio_lifecycle.c`/`.h`) compiled successfully in the real
  Gradle/CMake `assembleDebug` build (`JAVA_HOME` pointed at Temurin 17,
  NDK 27.2.12479018) — the first time any of these four files had been
  compiled under the real NDK/AAudio headers, with no compile errors
  against the API-usage assumptions made during the pure-host design phase
  (builder pattern, callback signatures, format/channel/performance-mode
  enums).
- `scripts/verify-native-lib.sh` passes against that APK. Manual
  `llvm-readelf -d`/`-s` inspection additionally confirmed: `libaaudio.so`
  is now a `DT_NEEDED` entry alongside the existing allow-listed set
  (`libopenxr_loader.so`, `libvulkan.so`, `libandroid.so`, `liblog.so`,
  `libz.so`, `libm.so`, `libdl.so`, `libc.so`) — no SDL2/desktop-GL/Apple-
  ObjC/VrApi dependency entered the APK from the new audio code — and that
  `AAudio_createStreamBuilder`/`AAudioStreamBuilder_*`/`AAudioStream_*`
  symbols are present as expected undefined (external, platform-provided)
  symbols, never statically bundled.
- `make test-quest-host-tests` passes at **5067/5067** assertions (up from
  4880/4880 at the end of layer 6's own session; the delta is this layer's
  new `bz_quest_wav.c`/`bz_quest_audio_mixer.c`/`bz_quest_audio_lifecycle.c`
  test suites — 47 new test functions across the three files).
- A new structural test script,
  `platform/android/quest/scripts/test-quest-audio-rt-callback-safety.sh`
  (wired into `make test-quest-audio-rt-callback-safety`, and into both the
  `test` and `quest` convenience targets in `build.mk`), extracts the real
  `bz_quest_audio_data_callback()`/`bz_quest_audio_mixer_render()` function
  bodies and greps them for any allocation/lock/log/file/bridge call,
  confirming the real-time-safety contract holds in the actual shipped
  source — verified to actually catch a violation by temporarily injecting
  a `malloc()` call into the callback body and confirming the script fails
  loudly, then reverting.
- `make test-quest-source-sync`, `make test-quest-host-tests` (5067/5067),
  `make test-quest-bridge` (67/67, unchanged), `make test-quest-stage-wc3-data`
  (14/14), `make test-quest-audio-rt-callback-safety`, and the full
  repo-root `make test` all pass with no regressions.
- `git diff --check clancey-quest-touch-controls...HEAD` reports no
  whitespace errors.
- **Not verified this session** — see "Hardware-only acceptance gates" and
  "Hardware/data-only acceptance procedure" above's "New this layer (7)"
  and "C. Data staging + audio acceptance" subsections: no physical Quest
  device, no real retail Warcraft III data, and therefore no real `adb`/
  `run-as` transfer, no real AAudio stream open/start, and no real audible
  sound were exercised — do not report any of these as confirmed until
  checked against real hardware and real staged data.

### What *was* verified this session (layer 7 follow-up: TFT-mount fix + staging robustness)

A focused review of the initial layer 7 PR found that staged TFT archives
were never actually mounted at runtime (`bz_quest_data_build_argv()` never
emitted `-tft`) plus two staging test-coverage/robustness gaps
(`--serial`/multi-device handling, and the free-space preflight not
accounting for the transient bounce+stage peak). This follow-up closed all
three, in new commits on the same branch (no history rewritten):

- Added `bz_quest_data_detect_edition()` (re-scans the resolved data
  directory using the exact same `.mpq` + case-insensitive `"War3x"`-prefix
  signal `FS_AddArchiveScanEntry()` uses) and threaded its result through
  `bz_quest_data_build_argv()`'s new `"-tft"` dash-flag emission —
  confirmed by `git blame`/reading `common/common.c`'s
  `FS_AddArchiveScanEntry()`/`Cvar_ApplyCommandLine()` ordering that a
  startup dash-flag (not a late `+fs_expansion 1`) is required, since
  `Cvar_ApplyCommandLine()` runs inside `Com_Init()`, which completes
  before `BZ_RuntimeInit()`'s one `FS_AddDataDirectory()` call.
  `BZ_QUEST_DATA_ARGV_MAX` was raised 5→6 to hold the extra flag.
- `test_bz_quest_data.c` gained 10 new pure-host tests (ROC never emits
  `-tft`, TFT emits it before `+map` in the right position, undersized-
  buffer rejection with the flag present, and edition detection's ROC-
  only/TFT-over-ROC/case-insensitivity/non-`.mpq`-false-positive/missing-
  directory/NULL-arg paths) — the file's total grew from 22 to 32 tests.
- `test_bz_quest_bridge.c` gained 3 new **integration** tests that pack
  real, distinctly-named synthetic MPQ archives at test-runtime via the
  existing `build/bin/mpqtool`'s `pack` command (one containing a
  `ROC_MARKER.txt`, one a `TFT_MARKER.txt`), start the real bridge against
  them, and assert both `Cvar_Integer("fs_expansion", -1)` **and**
  `FS_FileExists()` on the marker file actually inside the archive —
  proving genuine archive mounting, not just cvar state. Fixed a resulting
  test-order fragility (`fs_expansion` is a real process-global cvar that
  `Cvar_Get()` never resets across `BZ_RuntimeInit()` calls in the same
  test binary) by adding an explicit `Cvar_Set("fs_expansion", "0")`
  baseline reset at the start of each new test to remove that ordering
  dependency — a test-hygiene-only concern, since a real Android process
  only calls `Com_Init()` once. `test_bz_quest_bridge.c` grew from 10 to 13
  test functions (95 assertions total, `make test-quest-bridge`); the log
  line `bz_quest_bridge_start: resolved data dir '...', edition=roc|tft` is
  now visible in that target's own output as the expected log evidence for
  this fix.
- Confirmed (via `git log --oneline -- stage-wc3-data.sh`) that
  `--serial`/multi-device rejection, unknown-serial rejection, and
  offline-serial detection were already fully implemented in the original
  layer 7 commit — this half of the task was a **test-coverage gap**, not
  a code gap. Extended `test-stage-wc3-data.sh`'s fake-`adb` shim to
  simulate N attached devices (`DEVICES="serial:state ..."`, each routed to
  its own per-serial fake device root) and log every invocation's command +
  selected serial, then added 4 new tests: multiple devices without
  `--serial` rejected, `--serial` routing every adb call to the correct
  device (verified via the invocation log, not just the final file
  layout), an unknown `--serial` rejected, and an offline `--serial`
  device rejected.
- Found and fixed a **genuine gap** in the free-space preflight:
  `cmd_stage()` previously checked only the sum of final archive sizes,
  but each changed file transiently needs up to 2x its own size at once
  (the `/data/local/tmp` bounce copy plus the app-private `.stage.$$`
  copy, freed at different points). Added a `remote_file_hash()` helper
  (deduplicating a hash lookup previously inlined in `stage_one_file()`)
  and rewrote the preflight to require
  `transfer_bytes + max_transfer_bytes` free, where both figures are
  computed only from files that actually need transfer — so an idempotent
  re-stage (every file unchanged) correctly requires ~0 extra space and is
  never blocked even on a near-full device. Added 2 new tests: a near-full
  device rejected using the peak (not final-size) estimate, with the
  prior valid file proven byte-identical afterward and no `.stage.*`
  leftover; and an idempotent re-stage proven **not** blocked by the same
  near-full space.
- `test-stage-wc3-data.sh` grew from 14 to 20 tests, all passing
  (`make test-quest-stage-wc3-data`).
- Re-ran the full validation suite after all of the above:
  `make test-quest-host-tests` (5107/5107, up from 5067/5067 — the 10 new
  `bz_quest_data` tests' assertions), `make test-quest-bridge` (95/95),
  `make test-quest-stage-wc3-data` (20/20), the full repo-root `make test`
  (all suites reporting matching `N/N assertions passed`/`tests passed`,
  no `FAIL`), a real arm64 `assembleDebug` Gradle/CMake build (`BUILD
  SUCCESSFUL`), `scripts/verify-native-lib.sh` against the built APK, and
  `git diff --check clancey-quest-touch-controls...HEAD` (no whitespace
  errors) — all with no regressions. Manual `llvm-readelf -d` inspection
  of the rebuilt `.so` reconfirmed the same allow-listed `DT_NEEDED` set
  (`libopenxr_loader.so`, `libvulkan.so`, `libaaudio.so`, `libandroid.so`,
  `liblog.so`, `libz.so`, `libm.so`, `libdl.so`, `libc.so`) — no new
  dependency was introduced by this fix.
- **Not verified this session** (unchanged from the caveats above): no
  physical Quest device, no real retail Warcraft III data. The TFT-mount
  *code path* is now proven correct against real synthetic MPQ archives
  and the real engine, but whether a real retail `War3x.mpq`/
  `War3xlocal.mpq` pair actually loads and plays correctly on real
  hardware after a real `adb`-transferred stage remains a hardware/data
  acceptance item — see "Hardware/data-only acceptance procedure" above.

### What *was* verified this session (layer 8)

- `platform/android/quest/tests/test_bz_quest_hand_input.c` (22 tests) and
  the 16 new arbitration/integration tests appended to
  `test_bz_quest_input_state.c`, plus 4 new `bz_quest_quat_forward()` tests
  in `test_bz_quest_pure.c`, were iterated to green against real, by-hand-
  computed expected values (not assumed) — including two of this session's
  own mistakes caught with independent evidence rather than guessed at: an
  initial (wrong) assumption about which `bz_quest_pose_to_view_matrix()`
  output indices correspond to the shared quaternion-forward rotation was
  caught and corrected via a standalone Python cross-check *before* that
  test was ever run for the first time; and an initial (wrong) assumption
  about which controller-inactive frame the 0.3 s/0.1 s-per-frame source-
  switch debounce accumulator actually crosses its threshold on was caught
  by a genuine failing `make test-quest-host-tests` run, root-caused with a
  standalone float-accumulation check (not guessed), then fixed and
  re-verified green.
- `make test-quest-host-tests` passes at **5243/5243** assertions (up from
  the base branch's 5107/5107 — 136 new assertions across 42 new test
  functions, zero regressions in any pre-existing test).
- Every impure new/modified file (`bz_quest_xr.c`, `bz_quest_xr_hands.c`,
  `bz_quest_hand_input.c`, `bz_quest_xr_actions.c`, `bz_quest_renderer.c`)
  was syntax-checked with the real NDK 27.2.12479018 `clang`
  (`--target=aarch64-linux-android29 -fsyntax-only`) against the real,
  extracted `openxr_loader_for_android:1.1.49` AAR headers and the NDK's
  own bundled Vulkan headers, with zero errors, before attempting the full
  Gradle build.
- A real arm64-v8a Gradle/CMake `assembleDebug` (Temurin JDK 17,
  `ANDROID_HOME`/NDK 27.2.12479018 exactly as documented in
  "Prerequisites") reports **`BUILD SUCCESSFUL`**, compiling every new/
  modified Quest `.c` file for the first time under the real toolchain with
  no warnings/errors beyond this project's existing baseline.
- `scripts/verify-native-lib.sh` passes against the built APK:
  **`verified quest arm64-v8a native library:
  lib/arm64-v8a/libbz_quest_native.so`**. Manual `llvm-readelf -d`
  inspection of the extracted `.so` confirms the `DT_NEEDED` set is
  **byte-for-byte unchanged** from before this layer
  (`libopenxr_loader.so`, `libvulkan.so`, `libaaudio.so`, `libandroid.so`,
  `liblog.so`, `libz.so`, `libm.so`, `libdl.so`, `libc.so`) — no new shared-
  library dependency was introduced, confirming the hand-tracking functions
  are resolved purely via the pre-existing `xrGetInstanceProcAddr`
  mechanism (confirmed present as an undefined dynamic symbol via
  `llvm-nm -D --undefined-only`), never a new direct link dependency.
- The new `test-quest-hand-tracking-layout.sh` structural guard was proven
  to actually catch a real defect twice, in two separate deliberate
  injection/revert cycles during this session: (1) a `malloc()` call
  injected into `bz_quest_hand_sample_build()`'s body was initially **missed**
  by a first draft of the script's function-body-extraction logic (which
  anchored on the bare substring "bz_quest_hand_sample_build(" and matched
  this very file's own header-comment prose mention of the function name
  before its real definition) — the extraction was fixed to anchor on a
  line actually starting the declaration, re-verified to catch the same
  injected `malloc()`, then the injection was reverted and the script
  re-confirmed clean; (2) a deliberately reversed hand-tracker/session
  teardown order in `bz_quest_renderer_shutdown()` was caught immediately,
  then reverted and re-confirmed clean. Separately, and organically (not a
  deliberate test of the guard), this same script caught a **real,
  unintentional bug** in this session's first draft of
  `bz_quest_renderer_shutdown()`: `bz_quest_xr_hands_destroy()` was never
  called at all — fixed immediately upon the guard's first real run.
- `make test-quest-source-sync`, `test-quest-wc3-descriptor-pool-headroom`,
  `test-quest-wc3-bone-palette-layout`, `test-quest-wc3-fog-selection-layout`,
  `test-quest-wc3-hud-layout`, `test-quest-wc3-pointer-layout`,
  `test-quest-hand-tracking-layout`, and `test-quest-audio-rt-callback-safety`
  all pass with no regressions.
- `make test-quest-bridge` passes at **95/95** assertions, unchanged from
  the base branch (this layer does not touch `bz_quest_bridge.c`).
- The full repo-root `make test` passes end-to-end (`EXIT_CODE=0`),
  covering every WC3/WoW/SC2/tool/Quest suite in the repository with no
  `FAIL` anywhere, confirmed by redirecting the complete run to a log file
  and grepping it exhaustively rather than trusting a truncated terminal
  scrollback.
- `git diff --cached --check clancey-quest-data-staging-audio` (the full
  staged changeset, including every new file) reports zero whitespace
  errors.
- **Not verified this session** (unavoidable, no hardware available): every
  item listed under "Hardware-only acceptance gates"' "New this layer (8)"
  subsection above — no physical Meta Quest device, no real human hand, and
  therefore no real `xrCreateHandTrackerEXT`/`xrLocateHandJointsEXT` call,
  no real `XR_FB_hand_tracking_aim` negotiation, and no real pinch/aim/
  source-switch feel were ever exercised. `make run-sc2
  ARGS="+com_frame_limit 100"` (an unrelated repository-wide smoke check
  this task's validation checklist also requires) fails in this environment
  with "Failed to add data directory: data/StarCraft2" — a pre-existing
  environment characteristic (no StarCraft II data present at all, exactly
  like every other "no retail game data in this environment" caveat already
  documented throughout this file), confirmed unrelated to this layer by
  `git status` showing zero files touched outside `docs/` and
  `platform/android/quest/`.

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
- `RealityTabletopView.swift`'s `TabletopActionPanel` and
  `TabletopControls.swift`'s `TabletopActionButtonSnapshot` (under
  `platform/apple/visionos/tabletop/app/`) — the reviewed visionOS reference
  layer 5E's command-card grid rules (hidden-slot filtering, row-major sort,
  fixed 4-column grid, unconditional Cancel slot, stable action identity
  tuple) and icon/portrait/health/mana non-goals are traced against; see
  "Layer 5E: Warcraft III status/command-card HUD" above for the exact
  citations.
- `share/fonts/fixed_8x13.h` — the existing, source-committed, public-domain
  (X11 misc-fixed `8x13.bdf`) bitmap font layer 5E's `bz_quest_wc3_hud_font.c`
  packs into a project-owned Vulkan texture atlas, reused rather than
  fabricating a second opaque generated binary asset.
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
- Android scoped storage (why an ADB shell cannot write directly into
  another app's private storage on API 29+, motivating the `run-as` +
  `/data/local/tmp` bounce design):
  <https://developer.android.com/training/data-storage/app-specific>
- `adb`/`run-as` reference (`run-as <package> pwd` as the official
  debuggable-app capability-check command, and `run-as`'s default working
  directory being the package's own private data root):
  <https://developer.android.com/studio/debug>,
  <https://developer.android.com/tools/adb>
- `ANativeActivity` NDK struct reference (`internalDataPath`'s
  `Context.getFilesDir()` equivalence, `externalDataPath` mapping to
  `Context.getExternalFilesDir(null)` and its documented possible-`NULL`
  case):
  <https://developer.android.com/ndk/reference/struct/a-native-activity>
- AAudio developer guide (real-time data-callback constraints, high-
  priority callback threads, format/sample-rate query-after-open,
  disconnect/error-callback threading rules, stream close semantics):
  <https://developer.android.com/ndk/guides/audio/aaudio/aaudio>
- OpenXR API registry (`xr.xml`), the canonical machine-readable source for
  extension numbers/dependencies/enum-offset math: fetched from
  <https://raw.githubusercontent.com/KhronosGroup/OpenXR-SDK-Source/main/specification/registry/xr.xml>
  — `XR_EXT_hand_tracking` (`number="52"`, no dependency), `XR_FB_hand_tracking_aim`
  (`number="112"`, `depends="XR_VERSION_1_0+XR_EXT_hand_tracking"`), and the
  `1000000000 + (extnumber-1)*1000 + offset` enum-value formula cited above
  (verified by hand-computing `XR_TYPE_HAND_TRACKING_AIM_STATE_FB`'s
  `1000111001` from `number="112"` offset `1`).
- The bundled `org.khronos.openxr:openxr_loader_for_android:1.1.49`
  `openxr.h`/`openxr_platform.h` headers (same Maven Central artifact/AAR
  cited above for the loader) — the primary source for every `Xr*` type/
  enum/function-pointer this layer's `bz_quest_xr_hands.c` uses
  (`XrHandTrackerEXT`, `XrHandJointEXT`, `XR_HAND_JOINT_COUNT_EXT=26`,
  `XrHandTrackerCreateInfoEXT`, `XrHandJointsLocateInfoEXT`,
  `XrHandJointLocationEXT`/`XrHandJointLocationsEXT`,
  `XrSystemHandTrackingPropertiesEXT`, `XrHandTrackingAimStateFB`/
  `XrHandTrackingAimFlagsFB` and its `COMPUTED`/`VALID`/`INDEX_PINCHING`/
  `MIDDLE_PINCHING`/`SYSTEM_GESTURE`/`DOMINANT_HAND`/`MENU_PRESSED` bits,
  `xrCreateHandTrackerEXT`/`xrDestroyHandTrackerEXT`/`xrLocateHandJointsEXT`),
  extracted and inspected directly from the AAR's
  `prefab/modules/headers/include/openxr/` Prefab module rather than assumed.
- Meta's hand-tracking guidance (manifest permission/feature requirement;
  the "trust the runtime's `*Pinching` status bit, not the raw strength"
  pinch-detection guidance this layer's `FB_AIM` tier follows):
  <https://developers.meta.com/horizon/documentation/native/android/mobile-hand-tracking/>
  (VrApi-surfaced documentation — this project links no VrApi code, per
  AGENTS.md; the manifest gate is an Android/system-level requirement
  independent of which API subsequently reads the hand data, and the
  pinch-bit-over-strength guidance reflects the same underlying Quest
  system-level pinch detector both APIs expose).

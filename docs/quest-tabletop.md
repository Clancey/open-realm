# Meta Quest (Android/NDK + OpenXR) tabletop shell — Layer 3

This is layer 3 of a stacked Meta Quest port:

- Layer 1: [docs/visionos-tabletop.md](visionos-tabletop.md)'s extraction of
  `platform/tabletop/` — the portable pthreads lifecycle host and headless
  null client/renderer/UI seams — into shared, platform-neutral C.
- Layer 2 (`clancey-quest-android-openxr-shell`): a native Android/NDK +
  Khronos OpenXR build shell under `platform/android/quest/` that statically
  links the headless Warcraft III engine/game/asset/jass/sheet/shared source
  groups plus the shared tabletop lifecycle host, and proves the Khronos
  OpenXR Android loader + a minimal `xrCreateInstance` probe.
- **Layer 3 (this layer)**: replaces the layer-2 instance probe with a real
  OpenXR **session**, a Vulkan **stereo frame loop**, an
  `XR_FB_passthrough` **compositor layer**, and a minimal **head-tracked
  tabletop test scene**.

**This layer still does not start the engine thread, consume bridge
snapshots, poll gameplay input, play audio, or stage WC3 data.** See
[Current limitations](#current-limitations) and `bz_quest_host.c`'s
compile-time seams (`BZ_QUEST_ENABLE_*`, each guarded by a `#error` until its
real implementation lands) — `BZ_QUEST_ENABLE_VULKAN_RENDERER` is the one
seam this layer replaces with a real implementation.

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
    bin2c.c                      # host tool: SPIR-V binary -> C byte array
  tests/
    test_bz_quest_pure.c         # bz_quest_pure.c unit tests (host-buildable)
    test_bz_quest_scene.c        # bz_quest_scene.c unit tests (host-buildable)
    test_bz_quest_pure_main.c    # runs both suites, wired into `make test`
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
# passthrough-capability selection, procedural scene generator). Runs as
# part of `make test`.
make test-quest-host-tests

# Regenerate the SPIR-V-embedded shader header standalone (also run
# automatically by the CMake build below via a custom_command):
ANDROID_NDK_HOME=/path/to/ndk platform/android/quest/scripts/build-shaders.sh /tmp/shader-out

# Full Gradle/CMake debug build (arm64-v8a only). Also compiles shaders,
# builds the Vulkan/OpenXR/passthrough renderer, and links everything into
# one .so.
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

1. `bz_quest_host: starting (layer 3: OpenXR/Vulkan/passthrough renderer)`
2. `BZ_TabletopCreate/Destroy link proof OK (state=..., not started)`
3. `APP_CMD_START`
4. `xrInitializeLoaderKHR succeeded`
5. `xrGetSystem succeeded: systemName=... vendorId=... passthroughCapabilities=0x...`
   (the `0x...` bitmask must have `XR_PASSTHROUGH_CAPABILITY_BIT_FB` set —
   see "Hardware-only acceptance gates" below for what happens if not)
6. `Vulkan API version bound: min=1.x max=1.x`
7. `swapchain[0]: WxH, N images` and `swapchain[1]: WxH, N images`
8. `passthrough object + reconstruction layer created`
9. `passthrough started`
10. `bz_quest_renderer_init succeeded`
11. `APP_CMD_RESUME`
12. Repeated `XrEventDataSessionStateChanged: state=...` lines progressing
    `READY` -> `xrBeginSession succeeded` -> (eventually) `SYNCHRONIZED` ->
    `VISIBLE` -> `FOCUSED`.
13. No further `BZ_QUEST_LOGE` lines once `FOCUSED` is reached and frames are
    flowing (a healthy frame loop produces **no** per-frame log output at
    all — see "No busy loop / no per-frame logging" below).

Visual acceptance (must be confirmed by a human wearing the headset — no
automated check exists for this): the passthrough camera feed is visible as
the background, and a checkerboard tabletop with four colored cubes appears
roughly at waist height ~1m in front of the headset's `LOCAL` origin, with
correct stereo separation (looking left/right shows appropriate parallax)
and correct occlusion (cubes closer to the eye occlude the table behind
them, verifying the depth buffer is wired correctly).

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
timeout per iteration: `-1` (block indefinitely, zero CPU cost) whenever
`bz_quest_xr_is_session_running()` is false (renderer not ready yet, or the
app is backgrounded so the session is `SYNCHRONIZED`/`IDLE`/`STOPPING`), and
`0` (poll without blocking) only while a running session needs its
wait/begin/end frame calls kept flowing every iteration. No code path in
the frame loop logs anything on a healthy per-frame basis — every
`BZ_QUEST_LOGI`/`BZ_QUEST_LOGE` call site in `bz_quest_xr.c`/
`bz_quest_vk.c`/`bz_quest_passthrough.c` is inside a one-time init/teardown
function or an error branch, never inside `bz_quest_xr_wait_frame`/
`bz_quest_xr_begin_frame`/`bz_quest_xr_end_frame`/`bz_quest_vk_render_target`'s
success paths.

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
   and runs it on each `.spv`, which **verifies the SPIR-V magic number
   (`0x07230203`) before embedding** and writes a C byte-array header
   (`<name>.spv.h`, array `g_bz_quest_<name>_spv`).
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
- **`xrLocateViews` can report invalid tracking even mid-session.**
  `XrViewState.viewStateFlags` must be checked
  (`XR_VIEW_STATE_POSITION_VALID_BIT`/`ORIENTATION_VALID_BIT`) on every call
  — a momentary tracking loss does not change `XrSessionState`, so relying
  on session state alone to decide whether to render is insufficient. OpenXR
  1.0 spec, `xrLocateViews`/`XrViewState` reference pages.
- **The Khronos Android OpenXR loader AAR's Prefab package name is
  `"OpenXR"`, not the Maven artifact name.** Carried over from layer 2,
  still true and still load-bearing for `CMakeLists.txt`'s
  `find_package(OpenXR REQUIRED CONFIG)` — see layer 2's original
  verification of `org.khronos.openxr:openxr_loader_for_android:1.1.49`.

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
(`BZ_QUEST_ENABLE_ENGINE_START`, `BZ_QUEST_ENABLE_BRIDGE_SNAPSHOTS`,
`BZ_QUEST_ENABLE_INPUT`, `BZ_QUEST_ENABLE_AUDIO`,
`BZ_QUEST_ENABLE_DATA_STAGING`) so a later layer flips exactly one on as its
real implementation lands, instead of a silent stub reporting fake success.
`BZ_QUEST_ENABLE_VULKAN_RENDERER` is now hard-required to `1` (a
`#error` fires if a build tries to set it to `0`) since this layer's
renderer is no longer optional:

- The engine thread is never started (`BZ_TabletopStart()` is never
  called) — only `BZ_TabletopCreate()`/`BZ_TabletopDestroy()` are exercised.
- No `bz_tabletop_transport.h` bridge-snapshot consumption, no OpenXR
  action/input polling, no audio output, no War3 MPQ data staging onto the
  device.
- No Warcraft III asset rendering — the test scene is a procedurally
  generated checkerboard + cubes, nothing loaded from `.mdx`/`.blp`/MPQ
  data.
- No Vulkan multiview, MSAA, or fixed foveation (`XR_FB_foveation`) — see
  "Vulkan render pass/pipeline/targets" above for why this is an explicit,
  documented seam rather than an oversight.

### Hardware-only acceptance gates

**No physical Meta Quest device was available in this development
environment.** Everything below requires real hardware and was **not**
verified this session — do not report session creation, passthrough
activation, stereo correctness, frame rate/timing, or any visual result as
confirmed until each is checked against a real device:

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
- Whether `adb install`/launch/logcat actually behave as described in
  "Exact on-device acceptance procedure" above — the procedure is written
  from the OpenXR/Android/NDK spec and Meta's own documentation, not from
  having run it.
- The Meta manifest guidance page and the OpenXR/Vulkan spec URLs cited
  throughout this document are versioned/updatable; re-fetch and diff
  before relying on them for a release build.

### What *was* verified this session (compile-time/host-only)

- All `bz_quest_*.c` files individually syntax-check clean
  (`-fsyntax-only -Wall -Wextra --target=aarch64-linux-android29`) against
  the real extracted OpenXR AAR headers and the NDK's Vulkan headers.
- The full Gradle/CMake `assembleDebug` build succeeds end to end,
  producing `app-debug.apk` with `lib/arm64-v8a/libbz_quest_native.so`.
- `scripts/verify-native-lib.sh` passes against that APK: exactly one
  packaged ABI (`arm64-v8a`); `DT_NEEDED` is exactly
  `libopenxr_loader.so libvulkan.so libandroid.so liblog.so libz.so libm.so
  libdl.so libc.so` (no SDL2/desktop-GL/Apple-ObjC/VrApi dependency); no
  forbidden symbol referenced; no `main()` symbol linked;
  `ANativeActivity_onCreate` and `android_main` both present in the dynamic
  symbol table.
- The shader build pipeline produces valid SPIR-V (magic-number-checked)
  for both `tabletop_vert.vert`/`tabletop_frag.frag`, embedded into the
  linked `.so` with no committed binary.
- `make test-quest-source-sync`, `make test-quest-host-tests` (3252/3252
  assertions across `bz_quest_pure.c`'s math/selection helpers and
  `bz_quest_scene.c`'s procedural generator), and the full repo-root
  `make test` all pass.

## Related documents

- [visionos-tabletop.md](visionos-tabletop.md) — the shared
  `platform/tabletop/` extraction this layer links unmodified.
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

# Meta Quest (Android/NDK + OpenXR) tabletop shell — Layer 2

This is layer 2 of a stacked Meta Quest port (layer 1:
[docs/visionos-tabletop.md](visionos-tabletop.md)'s extraction of
`platform/tabletop/` — the portable pthreads lifecycle host and headless
null client/renderer/UI seams — into shared, platform-neutral C). This layer
adds a native Android/NDK + Khronos OpenXR build shell under
`platform/android/quest/` that statically links the same headless Warcraft
III engine/game/asset/jass/sheet/shared source groups the visionOS build
links, plus the shared `platform/tabletop/bridge/bz_tabletop_lifecycle.c`
lifecycle host and `platform/bridge/bz_tabletop_catalog.c`, into one
arm64-v8a `.so`, and correctly initializes the Khronos OpenXR Android loader.

**This layer does not render, start the engine thread, consume bridge
snapshots, poll input, play audio, or stage WC3 data.** It only proves the
native build/link/loader-init foundation those later layers build on. See
[Current limitations](#current-limitations) and `bz_quest_host.c`'s
compile-time seams (`BZ_QUEST_ENABLE_*`, each guarded by a `#error` until its
real implementation lands).

## Architecture boundary

```text
platform/android/quest/
  app/src/main/
    AndroidManifest.xml        # NativeActivity + Quest 3/3S manifest metadata
    cpp/
      CMakeLists.txt           # fetches BZ_XR_* source lists/cflags from
                                # platform/apple/visionos/build.mk via
                                # `make print-<VAR>` (see below) - never a
                                # second, hand-duplicated file list
      bz_quest_host.c           # android_native_app_glue entry point; OpenXR
                                # Android loader init/instance probe;
                                # BZ_TabletopCreate/Destroy link proof
    res/values/strings.xml
  build.gradle, app/build.gradle, settings.gradle, gradle.properties
  gradlew, gradle/wrapper/      # pinned Gradle wrapper (see Prerequisites)
  scripts/
    verify-native-lib.sh        # forbidden-dependency/ABI/entry-point check
    test-source-sync.sh         # Make -> CMake source-list sync-contract test
platform/android/quest/build.mk # thin `make quest*`/`test-quest-*` wrappers
```

No SDL2, desktop OpenGL, Apple/Objective-C/Swift, `common/main.c`, or Meta's
proprietary Mobile SDK/VrApi may enter `platform/android/quest/`; this is
enforced by `scripts/verify-native-lib.sh` (run via `make
quest-verify-native-lib`), not just by convention.

### Source-list synchronization contract

`platform/apple/visionos/build.mk` defines `BZ_XR_ENGINE_SRCS`,
`BZ_XR_GAME_SRCS`, `BZ_XR_ASSET_SRCS`, `BZ_XR_JASS_SRCS`, `BZ_XR_SHEET_SRCS`,
`BZ_XR_SHARED_SRCS`, `BZ_XR_BASE_CFLAGS`, `BZ_XR_CFLAGS`, and
`BZ_XR_FDF_CFLAGS` — all confirmed 100% portable (no Apple/arch-specific
flags). Rather than copy those file lists into a second, driftable literal
for CMake, the root `Makefile`'s generic debug target:

```make
print-%: ; @echo $*=$($*)
```

lets any tool query Make's *actual current* resolved value of a variable.
`platform/android/quest/app/src/main/cpp/CMakeLists.txt` shells out to `make
-s print-<VAR>` at configure time for exactly those nine variables, fails
the configure loudly (`FATAL_ERROR`) if any resolve empty or unexpectedly,
and generates one "unity" translation unit per source group (mirroring
`platform/apple/visionos/build.mk`'s own `bz_xr_unity_o` unity-build macro,
which some files' sibling-static-helper visibility depends on — splitting
them into individually compiled `add_library()` sources would silently
break that ordering contract).

`platform/android/quest/scripts/test-source-sync.sh` (wired into `make
test`, no Gradle/NDK required) independently asserts all nine variables
still resolve non-empty and still contain a known sentinel file from each
group, and that an undefined variable name still resolves to an empty
`NAME=` line (the exact "silently empty" case CMakeLists.txt's
`bz_make_srcs()` must catch and fail loudly on) — catching drift in build.mk
before it ever reaches a slow Gradle/CMake configure.

`Sys_Quit()` is supplied by `platform/tabletop/bridge/bz_tabletop_lifecycle.c`
(not `common/main.c`, which this build never links — verified absent from
the linked `.so` via `verify-native-lib.sh`).

## Prerequisites

Toolchain versions actually installed and used to build/verify this layer,
verified from this machine on 2026-05-28 (rerun the version commands below
before relying on them — this is one developer's toolchain snapshot, not a
pinned/reproducible-build guarantee):

| Tool | Verified version | Command |
|---|---|---|
| Android NDK | 27.2.12479018 | `ls "$ANDROID_HOME/ndk"` |
| Android SDK platforms/build-tools | up to android-36 / 36.1.0 | `sdkmanager --list_installed` |
| CMake (SDK-managed, used by AGP) | 3.22.1 | `app/build.gradle`'s `externalNativeBuild.cmake.version` |
| Gradle (wrapper, pinned) | 8.9 | `platform/android/quest/gradle/wrapper/gradle-wrapper.properties` |
| Android Gradle Plugin | 8.5.2 | `platform/android/quest/build.gradle` |
| Java | Temurin 17 (`JAVA_HOME` must point here for AGP 8.5.x) | `/usr/libexec/java_home -V` |
| adb | present, used only for `make quest-install-debug` (untested against real hardware) | `adb --version` |

Set `JAVA_HOME` to a JDK 17 install and `ANDROID_HOME`/`local.properties`'s
`sdk.dir` to your Android SDK before building — `local.properties` is
machine-local and gitignored, never committed.

## Build commands

```sh
# Fast, Gradle/NDK-free: fails loudly if the Make->CMake source-list sync
# contract above ever drifts. Runs as part of `make test`.
make test-quest-source-sync

# Full Gradle/CMake debug build (arm64-v8a only).
JAVA_HOME=/path/to/temurin-17 make quest-assemble-debug

# The above, plus the forbidden-dependency/ABI/entry-point check.
JAVA_HOME=/path/to/temurin-17 make quest-verify-native-lib

# Convenience target bundling everything that doesn't need physical
# hardware (source-sync test + full build + native-lib verification).
JAVA_HOME=/path/to/temurin-17 make quest

# Installs (does not launch) the built debug APK via adb - untested against
# real Quest hardware, see Current limitations.
JAVA_HOME=/path/to/temurin-17 make quest-install-debug
```

`make quest-verify-native-lib` was run against a real local build on this
machine and confirmed: exactly one packaged ABI (`arm64-v8a`); the only
`DT_NEEDED` shared libraries are `libopenxr_loader.so` and the standard
Bionic/NDK system libraries (`libandroid.so liblog.so libz.so libm.so
libdl.so libc.so`); no SDL2/desktop-GL/Apple-ObjC-runtime symbol is
referenced; no `main()` symbol is linked; `ANativeActivity_onCreate` and
`android_main` are both present in the dynamic symbol table.

## OpenXR Android loader integration

The Khronos-published (not Meta proprietary) Android OpenXR loader is
consumed via Gradle/Prefab:

```gradle
// app/build.gradle
android.buildFeatures.prefab true
dependencies {
    implementation 'org.khronos.openxr:openxr_loader_for_android:1.1.49'
}
```

```cmake
# app/src/main/cpp/CMakeLists.txt
find_package(OpenXR REQUIRED CONFIG)
target_link_libraries(bz_quest_native PRIVATE OpenXR::openxr_loader ...)
```

Verified (2026-05-28) via Maven Central and by downloading and inspecting
the `.aar` directly: `org.khronos.openxr:openxr_loader_for_android:1.1.49`
is the correct published artifact
([search.maven.org](https://search.maven.org/artifact/org.khronos.openxr/openxr_loader_for_android)).
Its Prefab package name is `"OpenXR"` (not the Maven artifact name), with two
modules: `headers` (header-only, `openxr/*.h`) and `openxr_loader` (shared
lib for arm64-v8a/armeabi-v7a/x86/x86_64, depends on `:headers`).

`bz_quest_host.c`'s Android loader-init/instance-probe sequence exactly
mirrors Khronos' own reference implementation, verified by fetching
`KhronosGroup/OpenXR-SDK-Source`'s `src/tests/hello_xr/main.cpp` and
`platformplugin_android.cpp` (main branch, 2026-05-28):

1. Resolve `xrInitializeLoaderKHR` via `xrGetInstanceProcAddr(XR_NULL_HANDLE,
   "xrInitializeLoaderKHR", ...)` — this must happen before any other OpenXR
   call.
2. Populate `XrLoaderInitInfoAndroidKHR{type, next, applicationVM,
   applicationContext}` from `android_app->activity->{vm,clazz}` and call
   it.
3. Call `xrCreateInstance` with the `XR_KHR_android_create_instance`
   extension and an `XrInstanceCreateInfoAndroidKHR{type, next,
   applicationVM, applicationActivity}` next-chain entry, to prove the
   loader can actually reach a runtime end to end.

Struct/field names and extension name macros were confirmed exact from the
extracted `.aar`'s own `openxr_platform.h`/`openxr.h`/
`openxr_loader_negotiation.h` headers, not guessed. Off Quest hardware (no
OpenXR runtime broker installed), step 3 is *expected* to fail; the code
logs this explicitly via `__android_log_print` rather than treating it as
silent success — see [Current limitations](#current-limitations).

## Manifest requirements

`AndroidManifest.xml`'s Quest-specific metadata (package `NativeActivity`
theme/orientation/configChanges/launchMode/resizeableActivity,
`com.oculus.supportedDevices`, headtracking `uses-feature`, the
`com.oculus.intent.category.VR` intent-filter category) was verified against
Meta's own documented mobile-native manifest guidance:
<https://developers.meta.com/horizon/documentation/native/android/mobile-native-manifest/>
(page's own `last_updated` metadata: 2026-05-28). That page recommends
`minSdkVersion 29` / `targetSdkVersion 32` for the Quest 1/2/Pro/3 device
tier, which `app/build.gradle` uses.

**Deliberately omitted:** Meta's example manifest also shows a
`horizonos:uses-horizonos-sdk` XML element. That element is specific to
Meta's Horizon OS Spatial SDK / panel-app model, not a plain OpenXR
NativeActivity app, so it was left out rather than added speculatively —
revisit only if a later layer adopts Spatial SDK panel features.

`com.oculus.supportedDevices` is scoped to `quest3|quest3s` only (this
layer's explicit target), narrower than Meta's own multi-device example
value.

## Package/app identifiers

`namespace`/`applicationId` (`org.openrealm.quest`) in `app/build.gradle`
and the Prefab/CMake target names (`bz_quest_native`,
`openwarcraft3-engine`, etc.) are project-private placeholders for this
sideloaded debug prototype. Replace `applicationId`/`namespace` with a real,
project-owned identifier before any wider distribution.

## Current limitations

Everything below is explicitly out of scope for this layer; each has a
compile-time `#error`-guarded seam in `bz_quest_host.c`
(`BZ_QUEST_ENABLE_VULKAN_RENDERER`, `BZ_QUEST_ENABLE_ENGINE_START`,
`BZ_QUEST_ENABLE_BRIDGE_SNAPSHOTS`, `BZ_QUEST_ENABLE_INPUT`,
`BZ_QUEST_ENABLE_AUDIO`, `BZ_QUEST_ENABLE_DATA_STAGING`) so a later layer
flips exactly one on as its real implementation lands, instead of a silent
stub reporting fake success:

- No Vulkan swapchain/renderer, no passthrough.
- The engine thread is never started (`BZ_TabletopStart()` is never
  called) — only `BZ_TabletopCreate()`/`BZ_TabletopDestroy()` are exercised,
  proving the archives link without needing real WC3 data staged on-device
  yet.
- No `bz_tabletop_transport.h` bridge-snapshot consumption, no OpenXR
  action/input polling, no audio output, no War3 MPQ data staging onto the
  device.

Additional things verified only on this development machine, not yet
confirmed against real hardware or a clean environment, and that **must be
rechecked before any release**:

- **No physical Meta Quest device was available to test this layer.**
  `xrCreateInstance` was only probed on this machine's build (expected to
  fail off-device, logged explicitly, never treated as success); actual
  on-device OpenXR runtime behavior, `adb install`/launch, and logcat output
  are unverified.
- Whether AGP's `externalNativeBuild.cmake` genuinely needs the
  SDK-managed `cmake;3.22.1` package (as opposed to a host-installed CMake)
  was not conclusively tested as a *failure* case — `$ANDROID_HOME/cmake`
  did not exist before this layer's first `assembleDebug` run; AGP
  transparently downloaded/installed `cmake;3.22.1` itself (no manual
  `sdkmanager` install was needed or performed).
- The Meta manifest guidance page is versioned/updatable
  (`last_updated: 2026-05-28` at fetch time); re-fetch and diff before
  relying on it for a store submission, which has additional requirements
  (e.g. store-listing metadata, signing) this developer-sideload manifest
  does not attempt to satisfy.

## Related documents

- [visionos-tabletop.md](visionos-tabletop.md) — the shared
  `platform/tabletop/` extraction this layer links unmodified.
- Khronos OpenXR Android loader: <https://github.com/KhronosGroup/OpenXR-SDK-Source>
  (`src/tests/hello_xr`), Maven Central artifact
  `org.khronos.openxr:openxr_loader_for_android`.
- Meta Quest native manifest requirements:
  <https://developers.meta.com/horizon/documentation/native/android/mobile-native-manifest/>

# ---------------------------------------------------------------------------
# Meta Quest (Android/NDK + Khronos OpenXR) tabletop native shell — Layer 4.
#
# Thin wrapper targets only: the real build is Gradle/CMake-driven (see
# platform/android/quest/app/build.gradle and
# platform/android/quest/app/src/main/cpp/CMakeLists.txt), which in turn
# fetches its engine/game/asset/jass/sheet/shared source lists and cflags
# live from platform/apple/visionos/build.mk's BZ_XR_* variables via this
# Makefile's print-% debug target, instead of duplicating them here. See
# docs/quest-tabletop.md for the full synchronization contract, prerequisite
# toolchain versions, and current limitations.
# ---------------------------------------------------------------------------
BZ_QUEST_DIR := platform/android/quest
BZ_QUEST_APK := $(BZ_QUEST_DIR)/app/build/outputs/apk/debug/app-debug.apk
BZ_QUEST_CPP_DIR := $(BZ_QUEST_DIR)/app/src/main/cpp
BZ_QUEST_TESTS_DIR := $(BZ_QUEST_DIR)/tests
BZ_QUEST_HOST_TEST_BIN := $(BZ_QUEST_TESTS_DIR)/.bz_quest_pure_tests

# Fast, Gradle/NDK-free check: fails loudly if the print-% Make variables the
# Quest CMakeLists.txt depends on go missing, empty, or drop a known source -
# runs on every `make test`, unlike the (slow, toolchain-dependent) actual
# Gradle build below.
.PHONY: test-quest-source-sync
test-quest-source-sync:
	@$(BZ_QUEST_DIR)/scripts/test-source-sync.sh

# Structural (no-Gradle/no-NDK) check that the layer-5A texture descriptor
# pool keeps its +1 create-before-evict spare slot - see
# bz_quest_vk_wc3.h's BZ_QUEST_VK_WC3_TEXTURE_DESCRIPTOR_POOL_CAPACITY
# comment and platform/android/quest/scripts/test-wc3-descriptor-pool-headroom.sh
# for the exact deadlock this guards against.
.PHONY: test-quest-wc3-descriptor-pool-headroom
test-quest-wc3-descriptor-pool-headroom:
	@$(BZ_QUEST_DIR)/scripts/test-wc3-descriptor-pool-headroom.sh

# Structural (no-Gradle/no-NDK) check that the layer-5C GPU-skinning bone-
# palette UBO layout, vertex attribute formats, and anim-arena ownership
# contract stay intact - see
# platform/android/quest/scripts/test-wc3-bone-palette-layout.sh for the
# exact regressions this guards against.
.PHONY: test-quest-wc3-bone-palette-layout
test-quest-wc3-bone-palette-layout:
	@$(BZ_QUEST_DIR)/scripts/test-wc3-bone-palette-layout.sh

# Structural (no-Gradle/no-NDK) check that layer 5D keeps its fog/selection
# shader interfaces, image format, and shared eye-pass ordering intact.
.PHONY: test-quest-wc3-fog-selection-layout
test-quest-wc3-fog-selection-layout:
	@$(BZ_QUEST_DIR)/scripts/test-wc3-fog-selection-layout.sh

# Structural (no-Gradle/no-NDK) check that layer 5E keeps its status/command-
# card HUD shader interfaces, font atlas image format, pipeline depth/cull/
# topology flags, mvp-only push constants, and render-pass ordering intact.
.PHONY: test-quest-wc3-hud-layout
test-quest-wc3-hud-layout:
	@$(BZ_QUEST_DIR)/scripts/test-wc3-hud-layout.sh

.PHONY: test-quest-wc3-pointer-layout
test-quest-wc3-pointer-layout:
	@$(BZ_QUEST_DIR)/scripts/test-wc3-pointer-layout.sh

# Structural (no-Gradle/no-NDK) check of layer 9's PRE2 particle-emitter GPU
# contract: CMake/shader-pipeline source-sync, the particle pipeline's own
# blend-mode-keyed variant cache (cull-none, depth-test-on), vertex format,
# renderer create/capture/record/destroy wiring and ordering (relative to
# fog/hud/the model renderer's own capture_and_upload/record_blended), and
# the pure simulation module's + impure Vulkan draw module's frame-critical
# forbidden-call (alloc/lock/log/file-IO/bridge-ABI) contract - see
# platform/android/quest/scripts/test-wc3-particles-layout.sh.
.PHONY: test-quest-wc3-particles-layout
test-quest-wc3-particles-layout:
	@$(BZ_QUEST_DIR)/scripts/test-wc3-particles-layout.sh

# Structural (no-Gradle/no-NDK) check of the PR #28 premultiplied-alpha
# passthrough-compositor contract: the XR projection layer's
# unpremultipliedAlpha flag, every WC3 blend mode's alpha-coverage factors
# (never mirrored from the color factors), the shared
# bz_quest_vk_straight_over_blend_state() "over" helper's 6 call sites
# (terrain-water/fog/selection-markers/hud-panel/hud-text/pointer), and the
# CPU+shader force-coverage-alpha=1 mechanism for every blendEnable=false
# draw path (model layers, particles, terrain ground/cliff) - see
# platform/android/quest/scripts/test-wc3-premultiplied-blend-layout.sh and
# docs/quest-tabletop.md's "Premultiplied-alpha passthrough contract"
# section for the exact defect this guards against.
.PHONY: test-quest-wc3-premultiplied-blend-layout
test-quest-wc3-premultiplied-blend-layout:
	@$(BZ_QUEST_DIR)/scripts/test-wc3-premultiplied-blend-layout.sh

# Structural (no-Gradle/no-NDK) check of the PR #28 stale model/texture GPU
# cache fix: bzQuestVkWc3_t's shared bzQuestWc3EpochTracker_t map-reload
# detector (modelTextureCacheEpoch/particlePoolEpoch, the DRY refactor of
# the pre-existing standalone particle-pool epoch fields), the transactional
# reset_model_texture_caches() shutdown+reinit order (device-idle wait,
# then BOTH caches destroyed, then BOTH re-initialized - never just one),
# and that the reset runs strictly before bz_quest_wc3_capture_frame() -
# see platform/android/quest/scripts/test-wc3-map-epoch-cache-reset-layout.sh
# and docs/quest-tabletop.md's "Map-reload GPU cache reset" section for the
# exact identity-reuse defect this guards against. Host-testable coverage of
# the underlying reset pattern/epoch tracker itself lives in
# test_bz_quest_wc3_cache.c (test-quest-host-tests).
.PHONY: test-quest-wc3-map-epoch-cache-reset-layout
test-quest-wc3-map-epoch-cache-reset-layout:
	@$(BZ_QUEST_DIR)/scripts/test-wc3-map-epoch-cache-reset-layout.sh

# Structural (no-NDK/no-device) check of layer 8's Meta Quest hand-tracking
# capability negotiation, tracker create/destroy lifecycle, frame-critical
# RT-safety of the pure gesture builder, renderer wiring, and manifest
# permission/feature flags - see
# platform/android/quest/scripts/test-quest-hand-tracking-layout.sh.
.PHONY: test-quest-hand-tracking-layout
test-quest-hand-tracking-layout:
	@$(BZ_QUEST_DIR)/scripts/test-quest-hand-tracking-layout.sh

# Structural (no-NDK/no-device) check that the AAudio real-time data
# callback and the mixer render function it calls never allocate, lock,
# log, touch files, or call bridge APIs - see
# platform/android/quest/scripts/test-quest-audio-rt-callback-safety.sh and
# bz_quest_audio.c's bz_quest_audio_data_callback header comment for the
# real-time-safety contract this guards.
.PHONY: test-quest-audio-rt-callback-safety
test-quest-audio-rt-callback-safety:
	@$(BZ_QUEST_DIR)/scripts/test-quest-audio-rt-callback-safety.sh

# Host-native (no NDK/Gradle/Quest hardware required) unit tests for the
# platform-independent bz_quest_pure.c/bz_quest_scene.c helpers (projection/
# view-matrix math, format/extension/passthrough-capability selection, and
# the procedural tabletop test-scene generator) - see those files' header
# comments for why they're kept plain-C/host-buildable. Runs on every
# `make test`, unlike the Gradle/NDK-dependent targets below.
.PHONY: test-quest-host-tests
test-quest-host-tests:
	@$(CC) -Wall -Wextra -std=c11 -I. -I$(BZ_QUEST_CPP_DIR) -I$(BZ_QUEST_TESTS_DIR) -Itests \
		$(BZ_QUEST_TESTS_DIR)/test_bz_quest_pure_main.c \
		$(BZ_QUEST_TESTS_DIR)/test_bz_quest_pure.c \
		$(BZ_QUEST_TESTS_DIR)/test_bz_quest_scene.c \
		$(BZ_QUEST_TESTS_DIR)/test_bz_quest_data.c \
		$(BZ_QUEST_TESTS_DIR)/test_bz_quest_frame.c \
		$(BZ_QUEST_TESTS_DIR)/test_bz_quest_wc3_render.c \
		$(BZ_QUEST_TESTS_DIR)/test_bz_quest_wc3_cache.c \
		$(BZ_QUEST_TESTS_DIR)/test_bz_quest_wc3_terrain.c \
		$(BZ_QUEST_TESTS_DIR)/test_bz_quest_wc3_anim.c \
		$(BZ_QUEST_TESTS_DIR)/test_bz_quest_wc3_particles.c \
		$(BZ_QUEST_TESTS_DIR)/test_bz_quest_wc3_fog.c \
		$(BZ_QUEST_TESTS_DIR)/test_bz_quest_wc3_world_transform.c \
		$(BZ_QUEST_TESTS_DIR)/test_bz_quest_wc3_hud_font.c \
		$(BZ_QUEST_TESTS_DIR)/test_bz_quest_wc3_hud.c \
		$(BZ_QUEST_TESTS_DIR)/test_bz_quest_input_state.c \
		$(BZ_QUEST_TESTS_DIR)/test_bz_quest_xr_bindings.c \
		$(BZ_QUEST_TESTS_DIR)/test_bz_quest_hand_input.c \
		$(BZ_QUEST_TESTS_DIR)/test_bz_quest_wav.c \
		$(BZ_QUEST_TESTS_DIR)/test_bz_quest_audio_mixer.c \
		$(BZ_QUEST_TESTS_DIR)/test_bz_quest_audio_lifecycle.c \
		$(BZ_QUEST_CPP_DIR)/bz_quest_pure.c \
		$(BZ_QUEST_CPP_DIR)/bz_quest_scene.c \
		$(BZ_QUEST_CPP_DIR)/bz_quest_data.c \
		$(BZ_QUEST_CPP_DIR)/bz_quest_frame.c \
		$(BZ_QUEST_CPP_DIR)/bz_quest_wc3_render.c \
		$(BZ_QUEST_CPP_DIR)/bz_quest_wc3_cache.c \
		$(BZ_QUEST_CPP_DIR)/bz_quest_wc3_terrain.c \
		$(BZ_QUEST_CPP_DIR)/bz_quest_wc3_anim.c \
		$(BZ_QUEST_CPP_DIR)/bz_quest_wc3_particles.c \
		$(BZ_QUEST_CPP_DIR)/bz_quest_wc3_fog.c \
		$(BZ_QUEST_CPP_DIR)/bz_quest_wc3_hud_font.c \
		$(BZ_QUEST_CPP_DIR)/bz_quest_wc3_hud.c \
		$(BZ_QUEST_CPP_DIR)/bz_quest_input_state.c \
		$(BZ_QUEST_CPP_DIR)/bz_quest_xr_bindings.c \
		$(BZ_QUEST_CPP_DIR)/bz_quest_hand_input.c \
		$(BZ_QUEST_CPP_DIR)/bz_quest_wav.c \
		$(BZ_QUEST_CPP_DIR)/bz_quest_audio_mixer.c \
		$(BZ_QUEST_CPP_DIR)/bz_quest_audio_lifecycle.c \
		-lm -o $(BZ_QUEST_HOST_TEST_BIN)
	@$(BZ_QUEST_HOST_TEST_BIN)
	@rm -f $(BZ_QUEST_HOST_TEST_BIN)

# Host-native coverage for bz_quest_bridge.c, the Quest lifecycle adapter
# (layer 4). Kept as its own test_schema target (rather than folded into
# test-quest-host-tests above) because - unlike bz_quest_pure/scene/data/
# frame's pure math/path helpers - this module links directly against the
# REAL platform/tabletop/bridge/bz_tabletop_lifecycle.c plus the lightweight
# common/bz_runtime.c stub set and the shared/sheet libs those pull in (file
# system + config load), exactly mirroring games/warcraft-3/tests/
# test_bz_tabletop_lifecycle.c's own technique (see that file's header
# comment and bz_quest_bridge.h's). Depends on test-assets: its
# "running"-path tests stage an ADB-style override file pointing at
# build/tests' real tests.mpq fixture (see bz_quest_data.h's data-path
# contract) so bz_quest_bridge_start() exercises the REAL
# BZ_TabletopCreate()/Start() success path, not just its failure paths.
.PHONY: test-quest-bridge
$(eval $(call test_schema,test-quest-bridge,test-assets $(SHARED_LIB) $(SHEET_LIB),$(CFLAGS) -I$(BZ_QUEST_CPP_DIR) -Iplatform/tabletop/bridge -Iserver -Iclient -Igames/warcraft-3 -I$(BZ_QUEST_TESTS_DIR) -Itests,$(BIN_DIR)/test_bz_quest_bridge$(EXE_EXT),$(BZ_QUEST_TESTS_DIR)/test_bz_quest_bridge_main.c $(BZ_QUEST_TESTS_DIR)/test_bz_quest_bridge.c $(BZ_QUEST_CPP_DIR)/bz_quest_data.c $(BZ_QUEST_CPP_DIR)/bz_quest_bridge.c platform/tabletop/bridge/bz_tabletop_lifecycle.c common/bz_runtime.c common/common.c common/cmd.c common/cvar.c common/msg.c common/net.c common/mpq.c,-lsheet -lshared -lm -lz -lpthread,))

# Fake-adb/run-as/pm/df host harness (no NDK/Gradle/device required) for the
# Layer 7 developer data-staging script - see
# platform/android/quest/scripts/test-stage-wc3-data.sh and
# docs/quest-tabletop.md's "Data staging" section for the exact path
# contract, ROC/TFT rules, and safety properties this exercises.
.PHONY: test-quest-stage-wc3-data
test-quest-stage-wc3-data:
	@$(BZ_QUEST_DIR)/scripts/test-stage-wc3-data.sh

test: test-quest-source-sync test-quest-wc3-descriptor-pool-headroom test-quest-wc3-bone-palette-layout test-quest-wc3-fog-selection-layout test-quest-wc3-hud-layout test-quest-wc3-pointer-layout test-quest-wc3-particles-layout test-quest-wc3-premultiplied-blend-layout test-quest-wc3-map-epoch-cache-reset-layout test-quest-hand-tracking-layout test-quest-audio-rt-callback-safety test-quest-host-tests test-quest-bridge test-quest-stage-wc3-data

# Assembles the unsigned arm64-v8a debug APK via the project's own Gradle
# wrapper. Requires an installed Android SDK/NDK (see docs/quest-tabletop.md)
# - this target intentionally does not attempt to install any of them.
.PHONY: quest-assemble-debug
quest-assemble-debug:
	@cd $(BZ_QUEST_DIR) && ./gradlew assembleDebug

# Verifies the assembled APK's native library: arm64-v8a only, no forbidden
# (SDL2/desktop-GL/Apple-ObjC/VrApi) shared-library dependency or symbol, no
# desktop main(), and the NativeActivity entry points are present. See
# platform/android/quest/scripts/verify-native-lib.sh.
.PHONY: quest-verify-native-lib
quest-verify-native-lib: quest-assemble-debug
	@$(BZ_QUEST_DIR)/scripts/verify-native-lib.sh $(BZ_QUEST_APK)

# Installs the assembled debug APK onto a connected/sideloaded Quest device
# via adb. Does not launch it or claim on-device verification - see
# docs/quest-tabletop.md's limitations section.
.PHONY: quest-install-debug
quest-install-debug: quest-assemble-debug
	@adb install -r $(BZ_QUEST_APK)

# Stages a developer's local, user-owned Warcraft III ROC/TFT data directory
# onto a connected/sideloaded Quest device via the app's own run-as identity
# (works under Android scoped storage without requiring a rooted device or
# shell access to another app's /sdcard/Android/data). Requires the debug
# APK already installed (quest-install-debug) and BZ_QUEST_WC3_DATA=<dir>.
# Never bundles/copies any Warcraft III data into git or the APK - see
# platform/android/quest/scripts/stage-wc3-data.sh and
# docs/quest-tabletop.md's "Data staging" section for the full contract.
.PHONY: quest-stage-wc3-data
quest-stage-wc3-data:
	@test -n "$(BZ_QUEST_WC3_DATA)" || { echo "usage: make quest-stage-wc3-data BZ_QUEST_WC3_DATA=/path/to/your/wc3/data" >&2; exit 2; }
	@$(BZ_QUEST_DIR)/scripts/stage-wc3-data.sh stage "$(BZ_QUEST_WC3_DATA)"

# Reports what is currently staged on the device (files present, sizes,
# sha256, and the override file's contents) without transferring anything.
.PHONY: quest-verify-wc3-data
quest-verify-wc3-data:
	@$(BZ_QUEST_DIR)/scripts/stage-wc3-data.sh verify

# Removes only the app's private staged Warcraft III data subdirectory and
# override file (never a broader/recursive delete) - requires an explicit
# confirmation, which this target supplies since invoking it is itself the
# developer's confirmation.
.PHONY: quest-clean-wc3-data
quest-clean-wc3-data:
	@$(BZ_QUEST_DIR)/scripts/stage-wc3-data.sh clean --yes

# Launches the installed app on the connected/sideloaded device via adb.
.PHONY: quest-run
quest-run:
	@$(BZ_QUEST_DIR)/scripts/stage-wc3-data.sh run

# Tails this app's logcat output (filtered to its own log tag) for the
# connected/sideloaded device via adb.
.PHONY: quest-log
quest-log:
	@$(BZ_QUEST_DIR)/scripts/stage-wc3-data.sh log

# Convenience target bundling the checks that do not require physical Quest
# hardware: source-list sync plus a full Gradle/CMake build and native
# library verification.
.PHONY: quest
quest: test-quest-source-sync test-quest-wc3-descriptor-pool-headroom test-quest-wc3-bone-palette-layout test-quest-wc3-fog-selection-layout test-quest-wc3-hud-layout test-quest-wc3-pointer-layout test-quest-wc3-particles-layout test-quest-wc3-premultiplied-blend-layout test-quest-wc3-map-epoch-cache-reset-layout test-quest-hand-tracking-layout test-quest-audio-rt-callback-safety test-quest-stage-wc3-data quest-verify-native-lib

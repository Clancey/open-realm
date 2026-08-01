# ---------------------------------------------------------------------------
# Meta Quest (Android/NDK + Khronos OpenXR) tabletop native shell — Layer 3.
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

# Host-native (no NDK/Gradle/Quest hardware required) unit tests for the
# platform-independent bz_quest_pure.c/bz_quest_scene.c helpers (projection/
# view-matrix math, format/extension/passthrough-capability selection, and
# the procedural tabletop test-scene generator) - see those files' header
# comments for why they're kept plain-C/host-buildable. Runs on every
# `make test`, unlike the Gradle/NDK-dependent targets below.
.PHONY: test-quest-host-tests
test-quest-host-tests:
	@$(CC) -Wall -Wextra -std=c11 -I$(BZ_QUEST_CPP_DIR) -I$(BZ_QUEST_TESTS_DIR) -Itests \
		$(BZ_QUEST_TESTS_DIR)/test_bz_quest_pure_main.c \
		$(BZ_QUEST_TESTS_DIR)/test_bz_quest_pure.c \
		$(BZ_QUEST_TESTS_DIR)/test_bz_quest_scene.c \
		$(BZ_QUEST_CPP_DIR)/bz_quest_pure.c \
		$(BZ_QUEST_CPP_DIR)/bz_quest_scene.c \
		-lm -o $(BZ_QUEST_HOST_TEST_BIN)
	@$(BZ_QUEST_HOST_TEST_BIN)
	@rm -f $(BZ_QUEST_HOST_TEST_BIN)

test: test-quest-source-sync test-quest-host-tests

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

# Convenience target bundling the checks that do not require physical Quest
# hardware: source-list sync plus a full Gradle/CMake build and native
# library verification.
.PHONY: quest
quest: test-quest-source-sync quest-verify-native-lib

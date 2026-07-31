# ---------------------------------------------------------------------------
# Meta Quest (Android/NDK + Khronos OpenXR) tabletop native shell — Layer 2.
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

# Fast, Gradle/NDK-free check: fails loudly if the print-% Make variables the
# Quest CMakeLists.txt depends on go missing, empty, or drop a known source -
# runs on every `make test`, unlike the (slow, toolchain-dependent) actual
# Gradle build below.
.PHONY: test-quest-source-sync
test-quest-source-sync:
	@$(BZ_QUEST_DIR)/scripts/test-source-sync.sh

test: test-quest-source-sync

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

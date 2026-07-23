# ---------------------------------------------------------------------------
# visionOS tabletop engine archives (Layer 1: callable static WC3 engine)
#
# Builds a statically-linked, headless Warcraft III engine library for the
# visionOS Simulator (xrsimulator) and device (xros) SDKs, using only
# `xcrun`/clang target triples and `ar` - no Xcode project is created or
# required. The result is a set of `.a` archives under
# build/lib/visionos/<platform>/ for a later Objective-C++ host
# (platform/apple/visionos/tabletop/bridge/) to link against.
#
# This build target intentionally excludes client/ (SDL window + SDL input
# polling + per-game gameplay input), renderer/ (SDL/OpenGL), and sound/
# (SDL audio): the real common/*.c and server/*.c never call into those
# modules directly (verified: no CM_/S_/R_ references outside client/), so
# the only replacement needed is platform/apple/visionos/tabletop/null/
# cl_null.c, a small explicit null client that satisfies the handful of
# CL_/Key_/Cmd_ForwardToServer symbols common/bz_runtime.c and
# common/common.c call unconditionally (see that file's header comment).
#
# Desktop dylib/so/dll targets defined elsewhere in this Makefile and in
# games/warcraft-3/game.mk are entirely unaffected by this file.
# ---------------------------------------------------------------------------
BZ_XR_MIN_OS  ?= 1.0
BZ_XR_NULL_DIR := platform/apple/visionos/tabletop/null
BZ_XR_LIB_DIR  := $(LIB_DIR)/visionos

# Independent from the desktop CFLAGS/WC3_CFLAGS: those pick up Darwin's
# `-arch $(ARCH)` (conflicts with `-target`) and Homebrew include paths that
# do not exist inside the visionOS SDK sysroots.
BZ_XR_BASE_CFLAGS := -Wall -fno-common -I. -Ishared -Ishared/types
BZ_XR_CFLAGS       := $(BZ_XR_BASE_CFLAGS) -I$(WC3_DIR) -I$(WC3_DIR)/common
BZ_XR_FDF_CFLAGS   := $(BZ_XR_CFLAGS) -DSTB_FDF_IMPLEMENTATION -DSTB_FDF_GLOBALS

# common/world.c and common/routing.c are intentionally excluded from the
# engine unit: games/warcraft-3/game/g_world.c already #includes both
# directly (the same trick the desktop GAME_LIB dylib relies on, since each
# dylib needs its own copy under macOS's two-level namespace). Combining a
# second copy into one static archive would duplicate-define CM_*/world.
BZ_XR_ENGINE_SRCS := $(shell find common -name '*.c' ! -name main.c ! -name macos.c ! -name world.c ! -name routing.c | sort) \
                     $(shell find server -name '*.c' | sort) \
                     $(BZ_XR_NULL_DIR)/cl_null.c
BZ_XR_GAME_SRCS   := $(shell find $(WC3_DIR)/game -name '*.c' | sort) \
                     $(shell find $(WC3_DIR)/common -name '*.c' ! -name world_w3.c | sort)
BZ_XR_JASS_SRCS   := $(shell find $(WC3_JASS_DIR) -name '*.c' | sort)
BZ_XR_SHEET_SRCS  := $(WC3_SHEET_DIR)/parser.c $(WC3_SHEET_DIR)/sheet.c
BZ_XR_SHARED_SRCS := $(shell find shared -name '*.c' | sort)

# Unity-compiles a fixed source list into one .o for a cross target - the
# same "concatenate as #include, compile as one TU" approach UNITY uses for
# the desktop dylibs (needed here too: some files rely on being unity-built
# after a sibling, e.g. games/warcraft-3/game/hud/*.c defines shared static
# helpers only visible to files compiled after it in the same TU).
#   $(1)=target .o  $(2)=label  $(3)=source list  $(4)=cflags  $(5)=sdk  $(6)=triple
define bz_xr_unity_o
$(1): $(3)
	@mkdir -p $$(@D)
	@echo "[$(2):$(5)]"
	@printf '\043include "%s"\n' $(3) | \
		xcrun --sdk $(5) clang -target $(6) -isysroot "$$$$(xcrun --sdk $(5) --show-sdk-path)" $(4) -x c -c -o $$@ -
endef

BZ_XR_TARGETS := xrsimulator xros
BZ_XR_TRIPLE_xrsimulator := arm64-apple-xros$(BZ_XR_MIN_OS)-simulator
BZ_XR_SDK_xrsimulator    := xrsimulator
BZ_XR_TRIPLE_xros        := arm64-apple-xros$(BZ_XR_MIN_OS)
BZ_XR_SDK_xros           := xros

# Per-platform: 5 unity objects (engine, game, jass, sheet, shared) archived
# into one libopenwarcraft3-engine.a. Sys_Quit() is intentionally left
# undefined by the archive - exactly like Quake 2's per-platform sys_*.c,
# it is supplied by whichever host links this archive (the desktop main.c,
# or the visionOS bridge's lifecycle-aware Sys_Quit()).
define bz_xr_platform_rules
BZ_XR_$(1)_DIR      := $$(BZ_XR_LIB_DIR)/$(1)
BZ_XR_$(1)_ARCHIVE  := $$(BZ_XR_$(1)_DIR)/libopenwarcraft3-engine.a

$$(eval $$(call bz_xr_unity_o,$$(BZ_XR_$(1)_DIR)/engine.o,engine,$$(BZ_XR_ENGINE_SRCS),$$(BZ_XR_CFLAGS),$$(BZ_XR_SDK_$(1)),$$(BZ_XR_TRIPLE_$(1))))
$$(eval $$(call bz_xr_unity_o,$$(BZ_XR_$(1)_DIR)/game.o,game,$$(BZ_XR_GAME_SRCS),$$(BZ_XR_FDF_CFLAGS),$$(BZ_XR_SDK_$(1)),$$(BZ_XR_TRIPLE_$(1))))
$$(eval $$(call bz_xr_unity_o,$$(BZ_XR_$(1)_DIR)/jass.o,jass,$$(BZ_XR_JASS_SRCS),$$(BZ_XR_CFLAGS),$$(BZ_XR_SDK_$(1)),$$(BZ_XR_TRIPLE_$(1))))
$$(eval $$(call bz_xr_unity_o,$$(BZ_XR_$(1)_DIR)/sheet.o,sheet,$$(BZ_XR_SHEET_SRCS),$$(BZ_XR_BASE_CFLAGS),$$(BZ_XR_SDK_$(1)),$$(BZ_XR_TRIPLE_$(1))))
$$(eval $$(call bz_xr_unity_o,$$(BZ_XR_$(1)_DIR)/shared.o,shared,$$(BZ_XR_SHARED_SRCS),$$(BZ_XR_BASE_CFLAGS),$$(BZ_XR_SDK_$(1)),$$(BZ_XR_TRIPLE_$(1))))

$$(BZ_XR_$(1)_ARCHIVE): $$(BZ_XR_$(1)_DIR)/engine.o $$(BZ_XR_$(1)_DIR)/game.o $$(BZ_XR_$(1)_DIR)/jass.o $$(BZ_XR_$(1)_DIR)/sheet.o $$(BZ_XR_$(1)_DIR)/shared.o
	@mkdir -p $$(@D)
	@echo "[archive $(1)]"
	@ar rcs $$@ $$^

.PHONY: $(1)
$(1): $$(BZ_XR_$(1)_ARCHIVE)
endef

$(foreach t,$(BZ_XR_TARGETS),$(eval $(call bz_xr_platform_rules,$(t))))

.PHONY: visionos
visionos: xrsimulator xros

# ---------------------------------------------------------------------------
# Objective-C++ lifecycle host (Layer 1 item 2)
#
# platform/apple/visionos/tabletop/bridge/ hosts the callable engine archive
# above from a dedicated thread with explicit idle/starting/running/failed/
# suspended/stopped state - no Swift, no RealityKit. bz_tabletop_lifecycle.c
# is the portable pthreads core (tested by make test, see
# games/warcraft-3/game.mk's test-bz-tabletop-lifecycle); bz_tabletop_bridge.mm
# is a thin NSObject wrapper with zero logic beyond forwarding to that core.
#
# Both are compiled per-platform below and archived into
# libopenwarcraft3-bridge.a, then link-checked against the engine archive
# from this file plus Foundation/libz/libpthread via a throwaway smoke
# binary (never run here - a `-target ...-simulator` binary needs `simctl`,
# not a bare `xcrun`/clang toolchain, to actually execute; this rule only
# proves the archive is fully linkable, which is the deliverable this layer
# promises).
# ---------------------------------------------------------------------------
BZ_XR_BRIDGE_DIR      := platform/apple/visionos/tabletop/bridge
BZ_XR_BRIDGE_CFLAGS    := $(BZ_XR_BASE_CFLAGS)
BZ_XR_BRIDGE_CXXFLAGS  := -Wall -fobjc-arc -std=c++17 -I$(BZ_XR_BRIDGE_DIR)

# $(1)=target .o  $(2)=source .c  $(3)=sdk  $(4)=triple
define bz_xr_bridge_c_o
$(1): $(2)
	@mkdir -p $$(@D)
	@echo "[bridge-core:$(3)]"
	@xcrun --sdk $(3) clang -target $(4) -isysroot "$$$$(xcrun --sdk $(3) --show-sdk-path)" $(BZ_XR_BRIDGE_CFLAGS) -c $(2) -o $$@
endef

# $(1)=target .o  $(2)=source .mm  $(3)=sdk  $(4)=triple
define bz_xr_bridge_mm_o
$(1): $(2)
	@mkdir -p $$(@D)
	@echo "[bridge-objcxx:$(3)]"
	@xcrun --sdk $(3) clang++ -x objective-c++ -target $(4) -isysroot "$$$$(xcrun --sdk $(3) --show-sdk-path)" $(BZ_XR_BRIDGE_CXXFLAGS) -c $(2) -o $$@
endef

define bz_xr_bridge_rules
BZ_XR_$(1)_BRIDGE_ARCHIVE := $$(BZ_XR_$(1)_DIR)/libopenwarcraft3-bridge.a
BZ_XR_$(1)_BRIDGE_SMOKE   := $$(BZ_XR_$(1)_DIR)/bridge-link-smoke

$$(eval $$(call bz_xr_bridge_c_o,$$(BZ_XR_$(1)_DIR)/bz_tabletop_lifecycle.o,$$(BZ_XR_BRIDGE_DIR)/bz_tabletop_lifecycle.c,$$(BZ_XR_SDK_$(1)),$$(BZ_XR_TRIPLE_$(1))))
$$(eval $$(call bz_xr_bridge_mm_o,$$(BZ_XR_$(1)_DIR)/bz_tabletop_bridge.o,$$(BZ_XR_BRIDGE_DIR)/bz_tabletop_bridge.mm,$$(BZ_XR_SDK_$(1)),$$(BZ_XR_TRIPLE_$(1))))

$$(BZ_XR_$(1)_BRIDGE_ARCHIVE): $$(BZ_XR_$(1)_DIR)/bz_tabletop_lifecycle.o $$(BZ_XR_$(1)_DIR)/bz_tabletop_bridge.o
	@mkdir -p $$(@D)
	@echo "[archive $(1)-bridge]"
	@ar rcs $$@ $$^

$$(BZ_XR_$(1)_BRIDGE_SMOKE): $$(BZ_XR_$(1)_BRIDGE_ARCHIVE) $$(BZ_XR_$(1)_ARCHIVE) $$(BZ_XR_BRIDGE_DIR)/smoke/bz_tabletop_link_smoke.mm
	@echo "[link-check $(1)-bridge]"
	@xcrun --sdk $$(BZ_XR_SDK_$(1)) clang++ -target $$(BZ_XR_TRIPLE_$(1)) \
		-isysroot "$$$$(xcrun --sdk $$(BZ_XR_SDK_$(1)) --show-sdk-path)" $$(BZ_XR_BRIDGE_CXXFLAGS) \
		-x objective-c++ $$(BZ_XR_BRIDGE_DIR)/smoke/bz_tabletop_link_smoke.mm -x none \
		$$(BZ_XR_$(1)_BRIDGE_ARCHIVE) $$(BZ_XR_$(1)_ARCHIVE) \
		-framework Foundation -lpthread -lz -o $$@

.PHONY: $(1)-bridge
$(1)-bridge: $$(BZ_XR_$(1)_BRIDGE_SMOKE)
endef

$(foreach t,$(BZ_XR_TARGETS),$(eval $(call bz_xr_bridge_rules,$(t))))

.PHONY: visionos-bridge
visionos-bridge: xrsimulator-bridge xros-bridge


# ---------------------------------------------------------------------------
# visionOS tabletop engine archives (Layer 1: callable static WC3 engine;
# Layer 2: real headless client + bz_tabletop_transport ABI)
#
# Builds a statically-linked, headless Warcraft III engine library for the
# visionOS Simulator (xrsimulator) and device (xros) SDKs, using only
# `xcrun`/clang target triples and `ar` - no Xcode project is created or
# required. The result is a set of `.a` archives under
# build/lib/visionos/<platform>/ for a later Objective-C++/Swift host
# (platform/apple/visionos/tabletop/bridge/, platform/bridge/) to link
# against.
#
# This build target links the real client/*.c networking/parse/state path
# (cl_main.c, cl_parse.c, cl_view.c, cl_tent.c, keys.c) so the tabletop
# transport (platform/bridge/bz_tabletop_transport.c) can publish
# authoritative snapshots straight from cl.ents/cl.playerstate/cl.selection/
# cl.fow/configstrings - never by reading server ge->edicts directly (see
# docs/visionos-tabletop.md). It still excludes renderer/ (SDL/OpenGL),
# sound/ (SDL audio), and the SDL-tainted client/ files (cl_input.c,
# cl_input_w3.c, cl_input_wow.c, cl_scrn.c, console.c, cl_fx.c, cl_layout.c):
# platform/apple/visionos/tabletop/client/ supplies small, explicit,
# link-selected headless replacements for exactly the renderer/input/sound/
# UI-drawing seams the real client calls unconditionally (R_GetAPI/
# R_StdoutGetAPI, S_Init/S_Shutdown/S_PlaySound*, UI_GetAPI, CL_Input,
# CL_SetGameplayBindings, SCR_UpdateScreen, CL_ParseUnitUI, CON_printf/
# CON_Init, CL_EntityEvent) - see that directory's header comments for why
# each one is safe to no-op or simplify headlessly. None of them create a
# window, open a GL context, or poll platform input.
# ---------------------------------------------------------------------------
BZ_XR_MIN_OS  ?= 1.0
BZ_XR_TT_CLIENT_DIR := platform/apple/visionos/tabletop/client
BZ_XR_BRIDGE_TRANSPORT_DIR := platform/bridge
BZ_XR_LIB_DIR  := $(LIB_DIR)/visionos
BZ_XR_WC3_DATA_TOOL := platform/apple/visionos/scripts/wc3_data.sh
BZ_XR_WC3_DATA_TEST := platform/apple/visionos/tests/test_wc3_data.sh
BZ_XR_APP_STAGE_DIR ?=

# Build-time retail-data contract for the later app shell. The caller supplies
# the app/staging root; the helper owns only Resources/Warcraft III beneath it.
.PHONY: test-visionos-wc3-data visionos-verify-wc3-source visionos-stage-wc3-data visionos-verify-wc3-data
test-visionos-wc3-data:
	@"$(BZ_XR_WC3_DATA_TEST)"

test: test-visionos-wc3-data

visionos-verify-wc3-source:
	@"$(BZ_XR_WC3_DATA_TOOL)" verify-source

visionos-stage-wc3-data:
	@if [ -z "$(strip $(BZ_XR_APP_STAGE_DIR))" ]; then \
		echo "visionos-stage-wc3-data: set BZ_XR_APP_STAGE_DIR to an app/staging root" >&2; exit 2; \
	fi
	@"$(BZ_XR_WC3_DATA_TOOL)" stage "$(BZ_XR_APP_STAGE_DIR)"

visionos-verify-wc3-data:
	@if [ -z "$(strip $(BZ_XR_APP_STAGE_DIR))" ]; then \
		echo "visionos-verify-wc3-data: set BZ_XR_APP_STAGE_DIR to an app/staging root" >&2; exit 2; \
	fi
	@"$(BZ_XR_WC3_DATA_TOOL)" verify-bundle "$(BZ_XR_APP_STAGE_DIR)"

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
#
# The real client sources plus this layer's headless glue and the transport
# ABI are listed explicitly (not via `find | sort`, unlike the rest of this
# unity object): the null glue files must be compiled ahead of the real
# client files that call their symbols without a header-visible prototype
# (e.g. client/cl_view.c calls CL_MouseOverGameplayUI(), which only
# client/cl_input_local.h - itself SDL-tainted and excluded - declares).
# This mirrors how the desktop unity build (Makefile's `CSRC`, alphabetically
# sorted) happens to place cl_input.c before cl_view.c today.
BZ_XR_CLIENT_SRCS := $(shell find $(BZ_XR_TT_CLIENT_DIR) -name '*.c' | sort) \
                     $(BZ_XR_BRIDGE_TRANSPORT_DIR)/bz_tabletop_assets.c \
                     $(BZ_XR_BRIDGE_TRANSPORT_DIR)/bz_tabletop_transport.c \
                     client/cl_main.c client/cl_parse.c client/cl_view.c client/cl_tent.c client/keys.c
BZ_XR_ENGINE_SRCS := $(shell find common -name '*.c' ! -name main.c ! -name macos.c ! -name world.c ! -name routing.c | sort) \
                     $(shell find server -name '*.c' | sort) \
                     $(BZ_XR_CLIENT_SRCS)
BZ_XR_GAME_SRCS   := $(shell find $(WC3_DIR)/game -name '*.c' | sort) \
                     $(shell find $(WC3_DIR)/common -name '*.c' ! -name world_w3.c | sort)
BZ_XR_ASSET_SRCS  := $(shell find $(WC3_DIR)/visionos -name '*.c' | sort)
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

# Per-platform: 6 unity objects (engine, game, asset translation, jass, sheet, shared) archived
# into one libopenwarcraft3-engine.a. Sys_Quit() is intentionally left
# undefined by the archive - exactly like Quake 2's per-platform sys_*.c,
# it is supplied by whichever host links this archive (the desktop main.c,
# or the visionOS bridge's lifecycle-aware Sys_Quit()).
define bz_xr_platform_rules
BZ_XR_$(1)_DIR      := $$(BZ_XR_LIB_DIR)/$(1)
BZ_XR_$(1)_ARCHIVE  := $$(BZ_XR_$(1)_DIR)/libopenwarcraft3-engine.a

$$(eval $$(call bz_xr_unity_o,$$(BZ_XR_$(1)_DIR)/engine.o,engine,$$(BZ_XR_ENGINE_SRCS),$$(BZ_XR_CFLAGS),$$(BZ_XR_SDK_$(1)),$$(BZ_XR_TRIPLE_$(1))))
$$(eval $$(call bz_xr_unity_o,$$(BZ_XR_$(1)_DIR)/game.o,game,$$(BZ_XR_GAME_SRCS),$$(BZ_XR_FDF_CFLAGS),$$(BZ_XR_SDK_$(1)),$$(BZ_XR_TRIPLE_$(1))))
$$(eval $$(call bz_xr_unity_o,$$(BZ_XR_$(1)_DIR)/assets.o,assets,$$(BZ_XR_ASSET_SRCS),$$(BZ_XR_CFLAGS),$$(BZ_XR_SDK_$(1)),$$(BZ_XR_TRIPLE_$(1))))
$$(eval $$(call bz_xr_unity_o,$$(BZ_XR_$(1)_DIR)/jass.o,jass,$$(BZ_XR_JASS_SRCS),$$(BZ_XR_CFLAGS),$$(BZ_XR_SDK_$(1)),$$(BZ_XR_TRIPLE_$(1))))
$$(eval $$(call bz_xr_unity_o,$$(BZ_XR_$(1)_DIR)/sheet.o,sheet,$$(BZ_XR_SHEET_SRCS),$$(BZ_XR_BASE_CFLAGS),$$(BZ_XR_SDK_$(1)),$$(BZ_XR_TRIPLE_$(1))))
$$(eval $$(call bz_xr_unity_o,$$(BZ_XR_$(1)_DIR)/shared.o,shared,$$(BZ_XR_SHARED_SRCS),$$(BZ_XR_BASE_CFLAGS),$$(BZ_XR_SDK_$(1)),$$(BZ_XR_TRIPLE_$(1))))

$$(BZ_XR_$(1)_ARCHIVE): $$(BZ_XR_$(1)_DIR)/engine.o $$(BZ_XR_$(1)_DIR)/game.o $$(BZ_XR_$(1)_DIR)/assets.o $$(BZ_XR_$(1)_DIR)/jass.o $$(BZ_XR_$(1)_DIR)/sheet.o $$(BZ_XR_$(1)_DIR)/shared.o
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

# Native SwiftUI/RealityKit shell with live production mode, explicit fixture
# tests, and a thin adapter over the Layer-2 lifecycle/transport archives.
BZ_XR_TABLETOP_SCRIPTS := platform/apple/visionos/tabletop/scripts

.PHONY: test-visionos-tabletop-host
test-visionos-tabletop-host:
	@$(BZ_XR_TABLETOP_SCRIPTS)/test-host.sh

.PHONY: visionos-tabletop-xrsimulator
visionos-tabletop-xrsimulator: xrsimulator-bridge
	@$(BZ_XR_TABLETOP_SCRIPTS)/build-tabletop.sh xrsimulator

.PHONY: visionos-tabletop-xros
visionos-tabletop-xros: xros-bridge
	@$(BZ_XR_TABLETOP_SCRIPTS)/build-tabletop.sh xros
.PHONY: visionos-tabletop
visionos-tabletop: test-visionos-tabletop-host visionos-tabletop-xrsimulator visionos-tabletop-xros

.PHONY: visionos-tabletop-simulator-acceptance
visionos-tabletop-simulator-acceptance: visionos-tabletop-xrsimulator
	@$(BZ_XR_TABLETOP_SCRIPTS)/launch-tabletop-simulator.sh

# ---------------------------------------------------------------------------
# Meta Quest (Android/NDK) tabletop native shell — Layer 2.
#
# Builds the same headless, platform-neutral Warcraft III engine/game/asset
# unit the visionOS build links (platform/apple/visionos/build.mk's
# BZ_XR_ENGINE_SRCS/BZ_XR_GAME_SRCS/BZ_XR_ASSET_SRCS/BZ_XR_JASS_SRCS/
# BZ_XR_SHEET_SRCS/BZ_XR_SHARED_SRCS) plus the portable tabletop lifecycle
# host (platform/tabletop/bridge/bz_tabletop_lifecycle.c, which supplies
# Sys_Quit()) and catalog (platform/bridge/bz_tabletop_catalog.c) into one
# arm64-v8a NativeActivity .so, alongside a native OpenXR/Android loader
# init shell (bz_quest_host.c). See docs/quest-tabletop.md.
#
# This layer never starts the engine thread (no BZ_TabletopStart call), does
# not create a Vulkan renderer/session, and does not consume bridge
# snapshots/input/audio/data staging - see bz_quest_host.c's compile-time
# seams for why, and which later layer owns each.
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.22)
project(bz_quest_native C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT ANDROID_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR
        "bz_quest: this project builds arm64-v8a only for the first Quest "
        "3/3S developer build (see docs/quest-tabletop.md); got ANDROID_ABI="
        "'${ANDROID_ABI}'. Restrict Gradle's abiFilters to arm64-v8a instead "
        "of widening this check.")
endif()

# --- Resolve the repository root --------------------------------------------
# This file lives at a fixed depth: platform/android/quest/app/src/main/cpp/
# (7 directories below the repo root). Walking up that exact count and then
# verifying two known root-only files exist fails the configure loudly (not
# silently) if the project is ever moved, instead of silently pointing the
# `make print-<VAR>` source-sync calls below at the wrong tree.
set(BZ_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
foreach(_bz_up RANGE 1 7)
    get_filename_component(BZ_REPO_ROOT "${BZ_REPO_ROOT}" DIRECTORY)
endforeach()
if(NOT EXISTS "${BZ_REPO_ROOT}/Makefile" OR NOT EXISTS "${BZ_REPO_ROOT}/AGENTS.md")
    message(FATAL_ERROR
        "bz_quest: could not resolve the repository root by walking up from "
        "${CMAKE_CURRENT_SOURCE_DIR} (got '${BZ_REPO_ROOT}', missing Makefile "
        "and/or AGENTS.md there). If platform/android/quest/app/src/main/cpp "
        "moved, update the RANGE count above to match its new depth.")
endif()

find_program(BZ_MAKE_EXECUTABLE NAMES make gmake)
if(NOT BZ_MAKE_EXECUTABLE)
    message(FATAL_ERROR
        "bz_quest: 'make' was not found on PATH. The engine/game/asset/jass/"
        "sheet/shared source lists and their portable cflags are fetched live "
        "from platform/apple/visionos/build.mk via 'make print-<VAR>' (the "
        "root Makefile's generic print-% debug target) instead of being "
        "duplicated here as a second, driftable literal list - see "
        "docs/quest-tabletop.md's source-list synchronization contract. "
        "Install GNU Make and re-run configure.")
endif()

# Reconfigure whenever the authoritative Make source lists could have
# changed, so a stale generated unity source list is never silently reused.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${BZ_REPO_ROOT}/Makefile"
    "${BZ_REPO_ROOT}/games/warcraft-3/game.mk"
    "${BZ_REPO_ROOT}/platform/apple/visionos/build.mk")

# Fetches Make variable `name`'s expanded value from the repo root via the
# print-% debug target and fails the configure step if `make` itself failed
# or produced unexpected output - never silently falls back to an empty
# value.
function(bz_make_var name out_var)
    execute_process(
        COMMAND "${BZ_MAKE_EXECUTABLE}" -s "print-${name}"
        WORKING_DIRECTORY "${BZ_REPO_ROOT}"
        OUTPUT_VARIABLE _bz_out
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _bz_result
    )
    if(NOT _bz_result EQUAL 0)
        message(FATAL_ERROR "bz_quest: 'make print-${name}' failed (exit ${_bz_result})")
    endif()
    string(FIND "${_bz_out}" "${name}=" _bz_prefix_pos)
    if(NOT _bz_prefix_pos EQUAL 0)
        message(FATAL_ERROR "bz_quest: unexpected 'make print-${name}' output: '${_bz_out}'")
    endif()
    string(LENGTH "${name}=" _bz_prefix_len)
    string(SUBSTRING "${_bz_out}" ${_bz_prefix_len} -1 _bz_value)
    set(${out_var} "${_bz_value}" PARENT_SCOPE)
endfunction()

# Same as bz_make_var, but turns a space-separated file list into a CMake
# list of absolute paths, and refuses to silently accept an empty result -
# a real engine build must never link zero sources for a named group.
function(bz_make_srcs name out_var)
    bz_make_var("${name}" _bz_raw)
    if(_bz_raw STREQUAL "")
        message(FATAL_ERROR
            "bz_quest: 'make print-${name}' resolved to an empty source list. "
            "Check that platform/apple/visionos/build.mk still defines ${name} "
            "(see docs/quest-tabletop.md's source-list synchronization contract).")
    endif()
    string(REPLACE " " ";" _bz_list "${_bz_raw}")
    list(TRANSFORM _bz_list PREPEND "${BZ_REPO_ROOT}/")
    set(${out_var} "${_bz_list}" PARENT_SCOPE)
endfunction()

# Same as bz_make_var, but rewrites the relative "-I<path>" flags Make
# resolved (relative to the repo root) into absolute paths CMake can use
# from any build directory, and returns a CMake list ready for
# target_compile_options.
function(bz_make_cflags name out_var)
    bz_make_var("${name}" _bz_raw)
    string(REGEX REPLACE "-I([^ ]+)" "-I${BZ_REPO_ROOT}/\\1" _bz_rewritten "${_bz_raw}")
    separate_arguments(_bz_list UNIX_COMMAND "${_bz_rewritten}")
    set(${out_var} "${_bz_list}" PARENT_SCOPE)
endfunction()

bz_make_srcs(BZ_XR_ENGINE_SRCS BZ_ENGINE_SRCS)
bz_make_srcs(BZ_XR_GAME_SRCS   BZ_GAME_SRCS)
bz_make_srcs(BZ_XR_ASSET_SRCS  BZ_ASSET_SRCS)
bz_make_srcs(BZ_XR_JASS_SRCS   BZ_JASS_SRCS)
bz_make_srcs(BZ_XR_SHEET_SRCS  BZ_SHEET_SRCS)
bz_make_srcs(BZ_XR_SHARED_SRCS BZ_SHARED_SRCS)

bz_make_cflags(BZ_XR_BASE_CFLAGS BZ_BASE_CFLAGS)
bz_make_cflags(BZ_XR_CFLAGS      BZ_CFLAGS)
bz_make_cflags(BZ_XR_FDF_CFLAGS  BZ_FDF_CFLAGS)

message(STATUS "bz_quest: engine sources: ${BZ_ENGINE_SRCS}")

# Unity-compiles `srcs` into exactly one translation unit that #includes each
# real .c file in order, mirroring platform/apple/visionos/build.mk's
# bz_xr_unity_o (some files rely on unity-build ordering, e.g. the headless
# null glue files must precede the real client files that call their
# symbols without a header-visible prototype - see that file's comment).
# Splitting these into one add_library() source per file instead would
# silently break that ordering contract with "implicit declaration"/link
# errors rather than a clear message, so this generated-unity-source
# approach is required, not merely stylistic.
function(bz_add_unity_lib target_name srcs cflags)
    set(_gen "${CMAKE_CURRENT_BINARY_DIR}/bz_generated/${target_name}_unity.c")
    set(_content "")
    foreach(_src ${srcs})
        string(APPEND _content "#include \"${_src}\"\n")
    endforeach()
    file(WRITE "${_gen}" "${_content}")
    add_library(${target_name} STATIC "${_gen}")
    target_compile_options(${target_name} PRIVATE ${cflags})
endfunction()

bz_add_unity_lib(openwarcraft3-engine "${BZ_ENGINE_SRCS}" "${BZ_CFLAGS}")
bz_add_unity_lib(openwarcraft3-game   "${BZ_GAME_SRCS}"   "${BZ_FDF_CFLAGS}")
bz_add_unity_lib(openwarcraft3-assets "${BZ_ASSET_SRCS}"  "${BZ_CFLAGS}")
bz_add_unity_lib(openwarcraft3-jass   "${BZ_JASS_SRCS}"   "${BZ_CFLAGS}")
bz_add_unity_lib(openwarcraft3-sheet  "${BZ_SHEET_SRCS}"  "${BZ_BASE_CFLAGS}")
bz_add_unity_lib(openwarcraft3-shared "${BZ_SHARED_SRCS}" "${BZ_BASE_CFLAGS}")

# --- Portable tabletop lifecycle/catalog bridge -----------------------------
# bz_tabletop_lifecycle.c supplies Sys_Quit() (see its header comment) for
# whichever host links the engine archive above; bz_tabletop_catalog.c reads
# the WC3 campaign/map catalog through common/mpq.c, already in the engine
# unit. Compiled per-file (not unity-built): both are already single, small,
# self-contained translation units in every other consumer (game.mk's
# test-bz-tabletop-lifecycle/test-bz-tabletop-catalog schemas compile them
# individually too), so there is no sibling-ordering contract to preserve.
add_library(openwarcraft3-bridge STATIC
    "${BZ_REPO_ROOT}/platform/tabletop/bridge/bz_tabletop_lifecycle.c"
    "${BZ_REPO_ROOT}/platform/bridge/bz_tabletop_catalog.c"
)
target_compile_options(openwarcraft3-bridge PRIVATE ${BZ_CFLAGS})
target_include_directories(openwarcraft3-bridge PRIVATE
    "${BZ_REPO_ROOT}/platform/tabletop/bridge"
)

# --- Khronos OpenXR Android loader (Gradle/Prefab) --------------------------
find_package(OpenXR REQUIRED CONFIG)

# --- android_native_app_glue -------------------------------------------------
# Vendored verbatim from the installed NDK (standard NativeActivity/CMake
# pattern - see the NDK's own native-activity sample); not copied into this
# repo, so it always matches the NDK version Gradle configured.
add_library(bz_native_app_glue STATIC
    "${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c"
)
target_include_directories(bz_native_app_glue PUBLIC
    "${ANDROID_NDK}/sources/android/native_app_glue"
)

# --- Native host shell -------------------------------------------------------
add_library(bz_quest_native SHARED
    bz_quest_host.c
)
target_include_directories(bz_quest_native PRIVATE "${BZ_REPO_ROOT}")
target_link_libraries(bz_quest_native PRIVATE
    bz_native_app_glue
    openwarcraft3-bridge
    openwarcraft3-game
    openwarcraft3-assets
    openwarcraft3-jass
    openwarcraft3-sheet
    openwarcraft3-engine
    openwarcraft3-shared
    OpenXR::openxr_loader
    android
    log
    z
)

# Keeps the linker from stripping android_native_app_glue's
# ANativeActivity_onCreate, the entry point the platform's NativeActivity
# Java shim dlsym()s by name.
target_link_options(bz_quest_native PRIVATE "-Wl,-u,ANativeActivity_onCreate")

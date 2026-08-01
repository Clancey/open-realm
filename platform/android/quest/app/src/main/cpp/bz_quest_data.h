/*
 * bz_quest_data.h - Warcraft III data-directory resolution and engine argv
 * construction for the Quest lifecycle bridge (layer 4).
 *
 * Every function here takes/returns plain char pointer/size_t/bool values and does
 * nothing Android-specific (only ordinary POSIX <stdio.h> file access to
 * read the override file) - see platform/android/quest/tests/
 * test_bz_quest_data.c, which builds and runs these exact decision paths on
 * the host with a plain C compiler and real temp directories, no NDK/
 * Android SDK required. bz_quest_bridge.c is the one real caller.
 *
 * Data-path contract (see docs/quest-tabletop.md's "Data-path contract"
 * section for the full write-up):
 *
 *   - Default: "<base>/Warcraft III", where <base> prefers
 *     ANativeActivity::externalDataPath (adb-push-accessible without root,
 *     survives an app update, mirrors Context.getExternalFilesDir(null))
 *     and falls back to ::internalDataPath only if external storage is
 *     unavailable (externalDataPath can be NULL per the NDK's
 *     ANativeActivity documentation - e.g. no shared storage volume
 *     mounted). This mirrors platform/apple/visionos/tabletop's
 *     "Resources/Warcraft III" bundled-data naming without literally
 *     reusing visionOS's bundle-relative path, since Quest has no app
 *     bundle to look inside.
 *   - Override: a narrowly-scoped, explicitly-staged mechanism for a
 *     developer who wants a non-default location (e.g. a symlink-free
 *     external SD card path) - never a silent secondary search path. A
 *     single-line text file at a *fixed*, documented location -
 *     "<internalDataPath>/warcraft_data_path_override.txt" - is read and
 *     strictly validated (see bz_quest_data_validate_override()) before use.
 *     internalDataPath is always non-NULL for a NativeActivity app (unlike
 *     externalDataPath), so this fixed location never itself depends on the
 *     value it might override - no chicken-and-egg resolution order. A
 *     developer stages it with a single `adb push` before first launch (see
 *     docs/quest-tabletop.md's acceptance procedure); this layer never
 *     writes, searches for, or bundles it itself. An invalid override
 *     (missing required absolute-path shape, disallowed character, empty,
 *     oversized) is a hard configuration error surfaced verbatim to the
 *     caller, never a silent fall-back to the default the developer was
 *     explicitly trying to avoid.
 */
#ifndef BZ_QUEST_DATA_H
#define BZ_QUEST_DATA_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    BZ_QUEST_DATA_DIR_MAX = 512,   /* generous bound for an Android filesystem path */
    BZ_QUEST_DATA_ERROR_MAX = 256,
    /* argv[0], "-data", <dir>[, "+map", <name>] - see bz_quest_data_build_argv(). */
    BZ_QUEST_DATA_ARGV_MAX = 5,
};

/* Fixed, documented override-file name (relative to internalDataPath) an
 * ADB staging step may `adb push` before first launch. Never guessed at a
 * second location - see this header's own comment above. */
#define BZ_QUEST_DATA_OVERRIDE_FILENAME "warcraft_data_path_override.txt"

/* The default data subdirectory name under whichever base path is used -
 * mirrors visionOS's "Warcraft III" bundled-data directory name. */
#define BZ_QUEST_DATA_SUBDIR "Warcraft III"

/*
 * Builds the deterministic default data directory "<base>/Warcraft III"
 * into out (cap bytes), preferring externalDataPath over internalDataPath
 * (see this header's data-path contract comment for why). Returns false
 * (out left unspecified) if neither path is a non-empty string, or if the
 * joined result would not fit in cap bytes - both are startup
 * configuration failures the caller must surface, not silently paper over.
 */
bool bz_quest_data_default_dir(const char *internalDataPath, const char *externalDataPath, char *out,
                                size_t cap);

/*
 * Validates and normalizes a candidate override path read from
 * BZ_QUEST_DATA_OVERRIDE_FILENAME: trims exactly one trailing '\n' (and a
 * preceding '\r', i.e. a CRLF line ending) if present, then requires the
 * remainder to be non-empty, start with '/' (absolute - a relative override
 * is ambiguous about what it is relative to and this bridge has no
 * "current directory" concept), fit within cap bytes, and contain none of
 * '"', '\r', '\n', ';' - mirroring bz_tabletop_lifecycle.c's own
 * BZ_TabletopSubmitMap() path validation exactly, since this value flows
 * into the same "-data" argv slot that eventually reaches a `map "<path>"`-
 * style console command internally. Returns false (out left unspecified)
 * for anything that fails these checks; the caller must treat that as a
 * hard configuration error (see this header's data-path contract comment).
 */
bool bz_quest_data_validate_override(const char *raw, char *out, size_t cap);

/*
 * Reads a single-line override from "<internalDataPath>/
 * BZ_QUEST_DATA_OVERRIDE_FILENAME" if that file exists. internalDataPath
 * must be non-NULL/non-empty (always true for a real NativeActivity app;
 * callers other than tests should never need to worry about this).
 *
 * Returns false (out left unspecified) if the file does not exist - this
 * is the normal, common case, not an error. Returns true if the file does
 * exist, with the *raw*, not-yet-validated first line copied into out (see
 * bz_quest_data_validate_override() for the validation step) - including
 * the case where the file exists but is empty or unreadable (a permission
 * error on a file a developer deliberately staged must never be silently
 * treated as "no override requested"; out is set to an empty string so the
 * validation step explicitly rejects it instead).
 */
bool bz_quest_data_read_override_file(const char *internalDataPath, char *out, size_t cap);

/*
 * Top-level resolution entry point bz_quest_bridge_start() calls once per
 * lifecycle attempt: looks for an override file (see
 * bz_quest_data_read_override_file()), validates it if present (see
 * bz_quest_data_validate_override()), and falls back to
 * bz_quest_data_default_dir() only when no override file exists at all.
 *
 * Returns true with the resolved directory in out_dir. Returns false (with
 * a human-readable reason in out_error) if: neither internalDataPath nor
 * externalDataPath is usable, or an override file exists but fails
 * validation. This function does NOT check the resolved directory actually
 * exists on disk or contains valid archives - that check happens inside
 * the engine's own BZ_RuntimeInit() (FS_AddDataDirectory), whose failure
 * the bridge surfaces separately as the lifecycle's real FAILED state (see
 * bz_quest_bridge.h).
 */
bool bz_quest_data_resolve(const char *internalDataPath, const char *externalDataPath, char *out_dir,
                            size_t out_cap, char *out_error, size_t error_cap);

/*
 * Builds a BZ_TabletopCreate()-compatible argv from a resolved data
 * directory and an optional map name, reusing the exact "-data" "<dir>"
 * [, "+map" "<name>"] convention platform/apple/visionos/tabletop/app/
 * OpenRealmTabletopApp.swift's LiveTabletopTransport already establishes
 * for this same lifecycle core (see common/bz_runtime.h's bzRuntimeArgs_t
 * and AGENTS.md's "Command Conventions": "+" is a process/startup argument
 * here, never an in-engine console command) - never inventing a second
 * startup-argument scheme for this platform.
 *
 * out_storage[i] must each provide BZ_QUEST_DATA_DIR_MAX bytes; out_argv[i]
 * is pointed at out_storage[i] (or a static string literal for argv[0]/
 * "-data"/"+map"), so the whole call can run with no heap allocation.
 * max_argv must be >= BZ_QUEST_DATA_ARGV_MAX (the caller-side constant that
 * is always sufficient for this function's output). Returns the argc
 * written (3 if mapName is NULL/empty, 5 otherwise), or 0 on a caller bug
 * (max_argv too small, or dataDir too long for one out_storage slot).
 */
int bz_quest_data_build_argv(const char *dataDir, const char *mapName,
                              char out_storage[][BZ_QUEST_DATA_DIR_MAX], const char **out_argv,
                              int max_argv);

#ifdef __cplusplus
}
#endif

#endif /* BZ_QUEST_DATA_H */

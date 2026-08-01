/*
 * bz_quest_data.c - see bz_quest_data.h.
 */
#include "bz_quest_data.h"

#include <stdio.h>
#include <string.h>

bool bz_quest_data_default_dir(const char *internalDataPath, const char *externalDataPath, char *out,
                                size_t cap) {
    const char *base = (externalDataPath && externalDataPath[0]) ? externalDataPath
                        : (internalDataPath && internalDataPath[0]) ? internalDataPath : NULL;
    if (!base) return false;
    int n = snprintf(out, cap, "%s/" BZ_QUEST_DATA_SUBDIR, base);
    return n > 0 && (size_t)n < cap;
}

bool bz_quest_data_validate_override(const char *raw, char *out, size_t cap) {
    if (!raw) return false;

    size_t length = strlen(raw);
    /* Trim exactly one trailing '\n', and a preceding '\r' if present (CRLF
     * line ending) - a text editor/adb push commonly appends one. */
    if (length && raw[length - 1] == '\n') length--;
    if (length && raw[length - 1] == '\r') length--;

    if (!length || length >= cap || raw[0] != '/' || memchr(raw, '"', length) ||
        memchr(raw, '\r', length) || memchr(raw, '\n', length) || memchr(raw, ';', length)) {
        return false;
    }

    memcpy(out, raw, length);
    out[length] = '\0';
    return true;
}

bool bz_quest_data_read_override_file(const char *internalDataPath, char *out, size_t cap) {
    if (!internalDataPath || !internalDataPath[0] || !cap) return false;

    char path[BZ_QUEST_DATA_DIR_MAX];
    int n = snprintf(path, sizeof(path), "%s/" BZ_QUEST_DATA_OVERRIDE_FILENAME, internalDataPath);
    if (n <= 0 || (size_t)n >= sizeof(path)) return false;

    FILE *f = fopen(path, "r");
    if (!f) return false; /* the common case: no override staged */

    out[0] = '\0';
    /* A staged file a developer deliberately placed must never be silently
     * treated as "no override" - even an empty/unreadable line still
     * reports true here so bz_quest_data_validate_override() can reject it
     * explicitly (empty input fails validation) instead of this function
     * quietly pretending the file did not exist at all. */
    if (!fgets(out, (int)cap, f)) out[0] = '\0';
    fclose(f);
    return true;
}

bool bz_quest_data_resolve(const char *internalDataPath, const char *externalDataPath, char *out_dir,
                            size_t out_cap, char *out_error, size_t error_cap) {
    char rawOverride[BZ_QUEST_DATA_DIR_MAX];

    if (bz_quest_data_read_override_file(internalDataPath, rawOverride, sizeof(rawOverride))) {
        if (!bz_quest_data_validate_override(rawOverride, out_dir, out_cap)) {
            snprintf(out_error, error_cap,
                     "invalid data directory override in '%s/" BZ_QUEST_DATA_OVERRIDE_FILENAME
                     "': must be a non-empty absolute path under %d bytes with no '\"', CR, LF, or ';'",
                     internalDataPath ? internalDataPath : "", (int)out_cap);
            return false;
        }
        return true;
    }

    if (!bz_quest_data_default_dir(internalDataPath, externalDataPath, out_dir, out_cap)) {
        snprintf(out_error, error_cap,
                 "no usable Warcraft III data directory: internalDataPath='%s' externalDataPath='%s'",
                 internalDataPath ? internalDataPath : "", externalDataPath ? externalDataPath : "");
        return false;
    }
    return true;
}

int bz_quest_data_build_argv(const char *dataDir, const char *mapName,
                              char out_storage[][BZ_QUEST_DATA_DIR_MAX], const char **out_argv,
                              int max_argv) {
    bool haveMap = mapName && mapName[0];
    int argc = haveMap ? 5 : 3;
    if (max_argv < argc || !dataDir) return 0;
    if (strlen(dataDir) >= BZ_QUEST_DATA_DIR_MAX) return 0;

    strcpy(out_storage[0], dataDir);
    out_argv[0] = "openwarcraft3-quest";
    out_argv[1] = "-data";
    out_argv[2] = out_storage[0];
    if (haveMap) {
        if (strlen(mapName) >= BZ_QUEST_DATA_DIR_MAX) return 0;
        strcpy(out_storage[1], mapName);
        out_argv[3] = "+map";
        out_argv[4] = out_storage[1];
    }
    return argc;
}

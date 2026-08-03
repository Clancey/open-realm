/*
 * bz_quest_data.c - see bz_quest_data.h.
 */
#include "bz_quest_data.h"

#include <dirent.h>
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

bool bz_quest_data_detect_edition(const char *dataDir, bzQuestDataEdition_t *out_edition) {
    if (!dataDir || !dataDir[0] || !out_edition) return false;

    DIR *dir = opendir(dataDir);
    if (!dir) return false;

    bzQuestDataEdition_t edition = BZ_QUEST_DATA_EDITION_ROC;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
        size_t len = strlen(name);
        /* Mirrors common/common.c's FS_AddArchiveScanEntry() exactly: a
         * ".mpq" file (case-insensitive extension) whose basename begins
         * with the 5-char "War3x" expansion prefix (case-insensitive) is
         * what makes the engine itself treat this data directory as
         * TFT-capable - see bz_quest_data.h's doc comment on this function
         * for the full citation. */
        if (len >= 4 && !strcasecmp(name + len - 4, ".mpq") && len >= 5 && !strncasecmp(name, "war3x", 5)) {
            edition = BZ_QUEST_DATA_EDITION_TFT;
            break;
        }
    }
    closedir(dir);

    *out_edition = edition;
    return true;
}

int bz_quest_data_build_argv(const char *dataDir, bzQuestDataEdition_t edition, const char *mapName,
                              char out_storage[][BZ_QUEST_DATA_DIR_MAX], const char **out_argv,
                              int max_argv) {
    bool haveMap = mapName && mapName[0];
    bool haveTft = edition == BZ_QUEST_DATA_EDITION_TFT;
    int argc = 3 + (haveTft ? 1 : 0) + (haveMap ? 2 : 0);
    if (max_argv < argc || !dataDir) return 0;
    if (strlen(dataDir) >= BZ_QUEST_DATA_DIR_MAX) return 0;
    if (haveMap && strlen(mapName) >= BZ_QUEST_DATA_DIR_MAX) return 0;

    strcpy(out_storage[0], dataDir);
    int i = 0;
    out_argv[i++] = "openwarcraft3-quest";
    out_argv[i++] = "-data";
    out_argv[i++] = out_storage[0];
    if (haveTft) {
        /* A dash *flag* (no value slot), applied by
         * Cvar_ApplyCommandLine() before BZ_RuntimeInit() ever calls
         * FS_AddDataDirectory() - see this function's doc comment in
         * bz_quest_data.h for the full ordering trace and why a late
         * "+fs_expansion 1" would not work. */
        out_argv[i++] = "-tft";
    }
    if (haveMap) {
        strcpy(out_storage[1], mapName);
        out_argv[i++] = "+map";
        out_argv[i++] = out_storage[1];
    }
    return argc;
}

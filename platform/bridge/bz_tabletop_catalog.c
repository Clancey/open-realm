#include "bz_tabletop_catalog.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <strings.h>
#endif

#include "common/mpq.h"
#include "games/warcraft-3/common/campaign.h"

#define BZ_TT_MAX_CATALOG_ARCHIVES 64
#define BZ_TT_MAX_CATALOG_MAPS 2048

struct bzTTCatalog {
    uint32_t count;
    bzTTMapEntry_t entries[BZ_TT_MAX_CATALOG_MAPS];
};

typedef struct {
    char path[BZ_TT_CATALOG_PATH_LEN * 2];
    HANDLE archive;
} bzTTCatalogArchive_t;

static int BZ_TTCatalogComparePaths(const void *a, const void *b) {
    return strcasecmp(((bzTTCatalogArchive_t const *)a)->path, ((bzTTCatalogArchive_t const *)b)->path);
}

static BOOL BZ_TTCatalogHasExtension(LPCSTR path, LPCSTR extension) {
    size_t path_len = strlen(path), extension_len = strlen(extension);
    return path_len >= extension_len && !strcasecmp(path + path_len - extension_len, extension);
}

static BOOL BZ_TTCatalogIsExpansionArchive(LPCSTR path) {
    LPCSTR base = strrchr(path, '/');
    base = base ? base + 1 : path;
    return !strncasecmp(base, "War3x", 5);
}

static DWORD BZ_TTCatalogOpenArchives(LPCSTR data_path, bzTTEdition_t edition,
                                      bzTTCatalogArchive_t *archives) {
    DIR *dir = opendir(data_path);
    struct dirent *entry;
    DWORD count = 0;
    if (!dir)
        return 0;
    while ((entry = readdir(dir)) && count < BZ_TT_MAX_CATALOG_ARCHIVES) {
        struct stat st;
        bzTTCatalogArchive_t *archive = &archives[count];
        if (!BZ_TTCatalogHasExtension(entry->d_name, ".mpq"))
            continue;
        snprintf(archive->path, sizeof(archive->path), "%s/%s", data_path, entry->d_name);
        if (edition == BZ_TT_EDITION_ROC && BZ_TTCatalogIsExpansionArchive(archive->path))
            continue;
        if (stat(archive->path, &st) || !S_ISREG(st.st_mode))
            continue;
        count++;
    }
    closedir(dir);
    qsort(archives, count, sizeof(*archives), BZ_TTCatalogComparePaths);
    DWORD opened = 0;
    FOR_LOOP(i, count) {
        if (!SFileOpenArchive(archives[i].path, 0, 0, &archives[opened].archive)) {
            fprintf(stderr, "BZTabletopCatalog: cannot open archive '%s'\n", archives[i].path);
            continue;
        }
        if (opened != i)
            snprintf(archives[opened].path, sizeof(archives[opened].path), "%s", archives[i].path);
        opened++;
    }
    return opened;
}

static void BZ_TTCatalogCloseArchives(bzTTCatalogArchive_t *archives, DWORD count) {
    FOR_LOOP(i, count)
        SFileCloseArchive(archives[i].archive);
}

static LPSTR BZ_TTCatalogReadHighest(bzTTCatalogArchive_t *archives, DWORD count,
                                     LPCSTR filename) {
    for (DWORD i = count; i > 0; i--) {
        HANDLE file;
        DWORD size, read = 0;
        LPSTR text;
        if (!SFileOpenFileEx(archives[i - 1].archive, filename, SFILE_OPEN_FROM_MPQ, &file))
            continue;
        size = SFileGetFileSize(file, NULL);
        text = malloc((size_t)size + 1);
        if (!text) {
            SFileCloseFile(file);
            return NULL;
        }
        if (!SFileReadFile(file, text, size, &read, NULL) || read != size) {
            free(text);
            SFileCloseFile(file);
            return NULL;
        }
        text[size] = '\0';
        SFileCloseFile(file);
        return text;
    }
    return NULL;
}

static BOOL BZ_TTCatalogHasPath(const bzTTCatalog_t *catalog, LPCSTR path) {
    FOR_LOOP(i, catalog->count)
        if (!strcasecmp(catalog->entries[i].map_path, path))
            return true;
    return false;
}

static void BZ_TTCatalogAddCampaigns(bzTTCatalog_t *catalog, LPCWC3CAMPAIGNCATALOG campaigns) {
    FOR_LOOP(order, campaigns->campaign_order_count) {
        DWORD campaign_index = campaigns->campaign_order[order];
        if (campaign_index >= campaigns->campaign_count)
            continue;
        LPCWC3CAMPAIGN campaign = &campaigns->campaigns[campaign_index];
        FOR_LOOP(mission_index, campaign->num_missions) {
            LPCWC3CAMPAIGNMISSION mission = &campaign->missions[mission_index];
            bzTTMapEntry_t *entry;
            if (!mission->map_path[0] || catalog->count >= BZ_TT_MAX_CATALOG_MAPS)
                continue;
            entry = &catalog->entries[catalog->count++];
            memset(entry, 0, sizeof(*entry));
            entry->source = BZ_TT_MAP_SOURCE_CAMPAIGN;
            entry->campaign_index = campaign_index;
            entry->mission_index = mission_index;
            snprintf(entry->campaign, sizeof(entry->campaign), "%s",
                     campaign->name[0] ? campaign->name : campaign->key);
            snprintf(entry->title, sizeof(entry->title), "%s",
                     mission->name[0] ? mission->name : mission->map_path);
            snprintf(entry->subtitle, sizeof(entry->subtitle), "%s", mission->header);
            snprintf(entry->map_path, sizeof(entry->map_path), "%s", mission->map_path);
        }
    }
}

static void BZ_TTCatalogNormalizeMapPath(LPCSTR input, LPSTR output, size_t size) {
    size_t i;
    for (i = 0; input[i] && i + 1 < size; i++)
        output[i] = input[i] == '/' ? '\\' : input[i];
    output[i] = '\0';
}

static BOOL BZ_TTCatalogIsMap(LPCSTR path) {
    return BZ_TTCatalogHasExtension(path, ".w3m") || BZ_TTCatalogHasExtension(path, ".w3x");
}

static void BZ_TTCatalogAddArchiveMaps(bzTTCatalog_t *catalog,
                                       bzTTCatalogArchive_t *archives, DWORD archive_count) {
    FOR_LOOP(i, archive_count) {
        SFILE_FIND_DATA found;
        HANDLE find = SFileFindFirstFile(archives[i].archive, "Maps\\*", &found, NULL);
        while (find && catalog->count < BZ_TT_MAX_CATALOG_MAPS) {
            PATHSTR path;
            BZ_TTCatalogNormalizeMapPath(found.cFileName, path, sizeof(path));
            if (BZ_TTCatalogIsMap(path) && !BZ_TTCatalogHasPath(catalog, path)) {
                bzTTMapEntry_t *entry = &catalog->entries[catalog->count++];
                LPCSTR base = strrchr(path, '\\');
                memset(entry, 0, sizeof(*entry));
                entry->source = BZ_TT_MAP_SOURCE_ARCHIVE;
                entry->campaign_index = UINT32_MAX;
                entry->mission_index = UINT32_MAX;
                snprintf(entry->title, sizeof(entry->title), "%s", base ? base + 1 : path);
                snprintf(entry->map_path, sizeof(entry->map_path), "%s", path);
            }
            if (!SFileFindNextFile(find, &found))
                break;
        }
        if (find)
            SFileFindClose(find);
    }
}

bzTTCatalogResult_t BZ_TTCatalog_Discover(uint32_t abi_version, LPCSTR data_path,
                                           bzTTEdition_t edition, bzTTCatalog_t **out) {
    bzTTCatalogArchive_t archives[BZ_TT_MAX_CATALOG_ARCHIVES] = { 0 };
    WC3CAMPAIGNCATALOG campaigns;
    bzTTCatalog_t *catalog;
    LPSTR text;
    DWORD archive_count;
    if (out)
        *out = NULL;
    if (abi_version != BZ_TABLETOP_CATALOG_ABI_VERSION || !data_path || !*data_path || !out ||
        (edition != BZ_TT_EDITION_ROC && edition != BZ_TT_EDITION_TFT))
        return BZ_TT_CATALOG_ERR_ARGUMENT;
    struct stat st;
    if (stat(data_path, &st) || !S_ISDIR(st.st_mode))
        return BZ_TT_CATALOG_ERR_DATA_DIRECTORY;
    archive_count = BZ_TTCatalogOpenArchives(data_path, edition, archives);
    if (!archive_count)
        return BZ_TT_CATALOG_ERR_NO_ARCHIVES;
    text = edition == BZ_TT_EDITION_TFT ?
        BZ_TTCatalogReadHighest(archives, archive_count, "UI\\CampaignStrings_exp.txt") : NULL;
    if (!text)
        text = BZ_TTCatalogReadHighest(archives, archive_count, "UI\\CampaignStrings.txt");
    if (!text) {
        BZ_TTCatalogCloseArchives(archives, archive_count);
        return BZ_TT_CATALOG_ERR_CAMPAIGN_METADATA;
    }
    WC3_CampaignCatalogReset(&campaigns);
    if (!WC3_CampaignCatalogParse(&campaigns, text)) {
        free(text);
        BZ_TTCatalogCloseArchives(archives, archive_count);
        return BZ_TT_CATALOG_ERR_CAMPAIGN_METADATA;
    }
    free(text);
    WC3_CampaignCatalogFinalize(&campaigns);
    catalog = calloc(1, sizeof(*catalog));
    if (!catalog) {
        BZ_TTCatalogCloseArchives(archives, archive_count);
        return BZ_TT_CATALOG_ERR_MEMORY;
    }
    BZ_TTCatalogAddCampaigns(catalog, &campaigns);
    BZ_TTCatalogAddArchiveMaps(catalog, archives, archive_count);
    BZ_TTCatalogCloseArchives(archives, archive_count);
    if (!catalog->count) {
        free(catalog);
        return BZ_TT_CATALOG_ERR_NO_MAPS;
    }
    *out = catalog;
    return BZ_TT_CATALOG_OK;
}

void BZ_TTCatalog_Release(bzTTCatalog_t *catalog) { free(catalog); }
uint32_t BZ_TTCatalog_Count(const bzTTCatalog_t *catalog) { return catalog ? catalog->count : 0; }

bool BZ_TTCatalog_Entry(const bzTTCatalog_t *catalog, uint32_t index, bzTTMapEntry_t *out) {
    if (!catalog || !out || index >= catalog->count)
        return false;
    *out = catalog->entries[index];
    return true;
}

const char *BZ_TTCatalog_ResultString(bzTTCatalogResult_t result) {
    switch (result) {
        case BZ_TT_CATALOG_OK: return "ok";
        case BZ_TT_CATALOG_ERR_ARGUMENT: return "invalid catalog request";
        case BZ_TT_CATALOG_ERR_DATA_DIRECTORY: return "Warcraft III data directory is missing";
        case BZ_TT_CATALOG_ERR_NO_ARCHIVES: return "no readable Warcraft III archives were found";
        case BZ_TT_CATALOG_ERR_CAMPAIGN_METADATA: return "campaign metadata is missing or malformed";
        case BZ_TT_CATALOG_ERR_NO_MAPS: return "no playable maps were discovered";
        case BZ_TT_CATALOG_ERR_MEMORY: return "catalog allocation failed";
    }
    return "unknown catalog error";
}

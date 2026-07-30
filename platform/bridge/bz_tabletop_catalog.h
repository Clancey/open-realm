#ifndef BZ_TABLETOP_CATALOG_H
#define BZ_TABLETOP_CATALOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BZ_TABLETOP_CATALOG_ABI_VERSION 1u

enum {
    BZ_TT_CATALOG_TITLE_LEN = 128,
    BZ_TT_CATALOG_PATH_LEN = 256,
};

typedef enum {
    BZ_TT_EDITION_ROC = 0,
    BZ_TT_EDITION_TFT,
} bzTTEdition_t;

typedef enum {
    BZ_TT_MAP_SOURCE_CAMPAIGN = 0,
    BZ_TT_MAP_SOURCE_ARCHIVE,
} bzTTMapSource_t;

typedef enum {
    BZ_TT_CATALOG_OK = 0,
    BZ_TT_CATALOG_ERR_ARGUMENT,
    BZ_TT_CATALOG_ERR_DATA_DIRECTORY,
    BZ_TT_CATALOG_ERR_NO_ARCHIVES,
    BZ_TT_CATALOG_ERR_CAMPAIGN_METADATA,
    BZ_TT_CATALOG_ERR_NO_MAPS,
    BZ_TT_CATALOG_ERR_MEMORY,
} bzTTCatalogResult_t;

typedef struct bzTTCatalog bzTTCatalog_t;

typedef struct {
    bzTTMapSource_t source;
    uint32_t campaign_index;
    uint32_t mission_index;
    char campaign[BZ_TT_CATALOG_TITLE_LEN];
    char title[BZ_TT_CATALOG_TITLE_LEN];
    char subtitle[BZ_TT_CATALOG_TITLE_LEN];
    char map_path[BZ_TT_CATALOG_PATH_LEN];
} bzTTMapEntry_t;

bzTTCatalogResult_t BZ_TTCatalog_Discover(uint32_t abi_version, const char *data_path,
                                           bzTTEdition_t edition, bzTTCatalog_t **out);
void BZ_TTCatalog_Release(bzTTCatalog_t *catalog);
uint32_t BZ_TTCatalog_Count(const bzTTCatalog_t *catalog);
bool BZ_TTCatalog_Entry(const bzTTCatalog_t *catalog, uint32_t index, bzTTMapEntry_t *out);
const char *BZ_TTCatalog_ResultString(bzTTCatalogResult_t result);

#ifdef __cplusplus
}
#endif

#endif

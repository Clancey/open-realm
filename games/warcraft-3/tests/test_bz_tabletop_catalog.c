#include <string.h>

#include "common/campaign.h"
#include "platform/bridge/bz_tabletop_catalog.h"
#include "test_framework.h"

static void test_campaign_parser_contract(void) {
    char text[] =
        "[Index]\n"
        "CampaignList=\"Test\"\n"
        "[Test]\n"
        "Header=\"Test Header\"\n"
        "Name=\"Test Campaign\"\n"
        "Cursor=1\n"
        "Mission0=\"Chapter One\",\"First Mission\",\"Maps\\\\Test\\\\First.w3m\"\n"
        "File1=\"Second\"\n";
    WC3CAMPAIGNCATALOG catalog;
    WC3_CampaignCatalogReset(&catalog);
    ASSERT(WC3_CampaignCatalogParse(&catalog, text));
    WC3_CampaignCatalogFinalize(&catalog);
    ASSERT_EQ_INT(catalog.campaign_count, 1);
    ASSERT_EQ_INT(catalog.campaign_order_count, 1);
    ASSERT_EQ_INT(catalog.campaigns[0].num_missions, 2);
    ASSERT_STR_EQ(catalog.campaigns[0].name, "Test Campaign");
    ASSERT_STR_EQ(catalog.campaigns[0].missions[0].map_path, "Maps\\\\Test\\\\First.w3m");
    ASSERT_STR_EQ(catalog.campaigns[0].missions[1].map_path, "Maps\\Campaign\\Second.w3m");
}

static void test_catalog_discovers_edition_data(void) {
    bzTTCatalog_t *roc = NULL, *tft = NULL;
    bzTTMapEntry_t roc_first, tft_first;
    ASSERT_EQ_INT(BZ_TTCatalog_Discover(
        BZ_TABLETOP_CATALOG_ABI_VERSION, "build/tests", BZ_TT_EDITION_ROC, &roc), BZ_TT_CATALOG_OK);
    ASSERT_EQ_INT(BZ_TTCatalog_Discover(
        BZ_TABLETOP_CATALOG_ABI_VERSION, "build/tests", BZ_TT_EDITION_TFT, &tft), BZ_TT_CATALOG_OK);
    ASSERT_NOT_NULL(roc);
    ASSERT_NOT_NULL(tft);
    ASSERT(BZ_TTCatalog_Count(roc) > 0);
    ASSERT(BZ_TTCatalog_Count(tft) > 0);
    ASSERT(BZ_TTCatalog_Entry(roc, 0, &roc_first));
    ASSERT(BZ_TTCatalog_Entry(tft, 0, &tft_first));
    ASSERT_EQ_INT(roc_first.source, BZ_TT_MAP_SOURCE_CAMPAIGN);
    ASSERT_EQ_INT(tft_first.source, BZ_TT_MAP_SOURCE_CAMPAIGN);
    ASSERT(strcmp(roc_first.map_path, tft_first.map_path));
    BZ_TTCatalog_Release(roc);
    BZ_TTCatalog_Release(tft);
}

static void test_catalog_errors_are_explicit(void) {
    bzTTCatalog_t *catalog = (bzTTCatalog_t *)1;
    ASSERT_EQ_INT(BZ_TTCatalog_Discover(
        0, "build/tests", BZ_TT_EDITION_ROC, &catalog), BZ_TT_CATALOG_ERR_ARGUMENT);
    ASSERT_NULL(catalog);
    ASSERT_EQ_INT(BZ_TTCatalog_Discover(
        BZ_TABLETOP_CATALOG_ABI_VERSION, "build/tests/missing", BZ_TT_EDITION_ROC, &catalog),
        BZ_TT_CATALOG_ERR_DATA_DIRECTORY);
    ASSERT_NULL(catalog);
    ASSERT_STR_EQ(BZ_TTCatalog_ResultString(BZ_TT_CATALOG_ERR_NO_ARCHIVES),
                  "no readable Warcraft III archives were found");
}

void run_bz_tabletop_catalog_tests(void) {
    RUN_TEST(test_campaign_parser_contract);
    RUN_TEST(test_catalog_discovers_edition_data);
    RUN_TEST(test_catalog_errors_are_explicit);
}

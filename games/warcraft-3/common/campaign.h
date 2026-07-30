#ifndef __wc3_campaign_h__
#define __wc3_campaign_h__

#include "common/shared.h"

#define WC3_MAX_CAMPAIGNS 16
#define WC3_MAX_CAMPAIGN_MISSIONS 128

typedef struct {
    UINAME header;
    UINAME name;
    PATHSTR map_path;
} WC3CAMPAIGNMISSION;
typedef WC3CAMPAIGNMISSION *LPWC3CAMPAIGNMISSION;
typedef const WC3CAMPAIGNMISSION *LPCWC3CAMPAIGNMISSION;

typedef struct {
    playerRace_t race;
    UINAME key;
    UINAME header;
    UINAME name;
    UINAME background;
    WC3CAMPAIGNMISSION missions[WC3_MAX_CAMPAIGN_MISSIONS];
    DWORD num_missions;
} WC3CAMPAIGN;
typedef WC3CAMPAIGN *LPWC3CAMPAIGN;
typedef const WC3CAMPAIGN *LPCWC3CAMPAIGN;

typedef struct {
    WC3CAMPAIGN campaigns[WC3_MAX_CAMPAIGNS];
    DWORD campaign_count;
    DWORD campaign_order[WC3_MAX_CAMPAIGNS];
    DWORD campaign_order_count;
} WC3CAMPAIGNCATALOG;
typedef WC3CAMPAIGNCATALOG *LPWC3CAMPAIGNCATALOG;
typedef const WC3CAMPAIGNCATALOG *LPCWC3CAMPAIGNCATALOG;

void WC3_CampaignCatalogReset(LPWC3CAMPAIGNCATALOG catalog);
BOOL WC3_CampaignCatalogParse(LPWC3CAMPAIGNCATALOG catalog, LPSTR text);
void WC3_CampaignCatalogFinalize(LPWC3CAMPAIGNCATALOG catalog);

#endif

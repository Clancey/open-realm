/*
 * ui/screens/single_player.c — Single player menu screen.
 */

#include "../ui_local.h"
#include "../ui_screen.h"
#include "../generated/single_player_menu.h"
#include "common/campaign.h"
#include <stdlib.h>

typedef enum {
    SINGLE_PLAYER_VIEW_MAIN,
    SINGLE_PLAYER_VIEW_CAMPAIGN_SELECT,
} singlePlayerView_t;

static SinglePlayerMenu_t single_player;
static WC3CAMPAIGNCATALOG campaign_data;
#define campaigns campaign_data.campaigns
#define campaign_count campaign_data.campaign_count
#define campaign_order campaign_data.campaign_order
#define campaign_order_count campaign_data.campaign_order_count
static uiMapListState_t campaign_list;
static LPFRAMEDEF campaign_list_frame;
static DWORD campaign_background_model = 0;
static singlePlayerView_t current_view = SINGLE_PLAYER_VIEW_MAIN;

static BOOL SinglePlayerMenu_LoadScreen(void) {
    if (SinglePlayerMenu_Load(&single_player)) {
        UI_EnsureFDF("UI\\FrameDef\\Glue\\MapListBox.fdf");
        return true;
    }
    return false;
}

static LPCWC3CAMPAIGN SinglePlayer_FindCampaign(LPCSTR name) {
    if (!name) {
        return NULL;
    }
    FOR_LOOP(i, campaign_count) {
        if (!strcasecmp(name, campaigns[i].key)) {
            return &campaigns[i];
        }
    }
    return NULL;
}

static BOOL SinglePlayer_LoadCampaignFile(LPCSTR file_name) {
    void *buffer = NULL;
    int size = uiimport.FS_ReadFile(file_name, &buffer);
    if (size <= 0 || !buffer) {
        return false;
    }
    LPSTR text = uiimport.MemAlloc(size + 1);
    if (!text) {
        uiimport.FS_FreeFile(buffer);
        return false;
    }
    memcpy(text, buffer, (size_t)size);
    text[size] = '\0';
    uiimport.FS_FreeFile(buffer);

    BOOL parsed = WC3_CampaignCatalogParse(&campaign_data, text);
    uiimport.MemFree(text);
    return parsed;
}

static void SinglePlayer_FinalizeCampaignOrder(void) {
    WC3_CampaignCatalogFinalize(&campaign_data);
}

static BOOL SinglePlayer_ExpansionEnabled(void) {
    LPCSTR value = uiimport.Cvar_String("fs_expansion", "0");
    return value && atoi(value) != 0;
}

static void SinglePlayer_LoadCampaignData(void) {
    LPCSTR campaign_file;
    BOOL const expansion = SinglePlayer_ExpansionEnabled();

    WC3_CampaignCatalogReset(&campaign_data);

    campaign_file = expansion ? Theme_String("CampaignFile", "Default") : NULL;
    if (campaign_file && strcmp(campaign_file, "CampaignFile") &&
        SinglePlayer_LoadCampaignFile(campaign_file)) {
        SinglePlayer_FinalizeCampaignOrder();
        return;
    }
    if ((expansion && SinglePlayer_LoadCampaignFile("UI\\CampaignStrings_exp.txt")) ||
        SinglePlayer_LoadCampaignFile("UI\\CampaignStrings.txt")) {
        SinglePlayer_FinalizeCampaignOrder();
    }
}

static LPCWC3CAMPAIGN SinglePlayer_DefaultCampaign(void) {
    if (campaign_order_count && campaign_order[0] < campaign_count) {
        return &campaigns[campaign_order[0]];
    }
    return campaign_count ? &campaigns[0] : NULL;
}

static LPCSTR SinglePlayer_FirstMissionMap(LPCWC3CAMPAIGN campaign) {
    if (!campaign) {
        return NULL;
    }
    FOR_LOOP(i, campaign->num_missions) {
        if (campaign->missions[i].map_path[0]) {
            return campaign->missions[i].map_path;
        }
    }
    return NULL;
}

static void SinglePlayer_SetHidden(LPFRAMEDEF frame, BOOL hidden) {
    if (frame) {
        UI_SetHidden(frame, hidden);
    }
}

static BOOL SinglePlayer_HasStaticCampaignButtons(void) {
    return single_player.HumanButton ||
           single_player.OrcButton ||
           single_player.UndeadButton ||
           single_player.NightElfButton ||
           single_player.TutorialButton;
}

static void SinglePlayer_SetView(singlePlayerView_t view) {
    BOOL const show_campaign = view == SINGLE_PLAYER_VIEW_CAMPAIGN_SELECT;

    current_view = view;

    SinglePlayer_SetHidden(single_player.SinglePlayerMenu, view != SINGLE_PLAYER_VIEW_MAIN);
    SinglePlayer_SetHidden(single_player.MainPanel, false);
    SinglePlayer_SetHidden(single_player.ProfilePanel, true);

    SinglePlayer_SetHidden(single_player.CampaignMenu, !show_campaign);
    SinglePlayer_SetHidden(single_player.CampaignBackdrop_2, true);
    SinglePlayer_SetHidden(single_player.CampaignSelectFrame, false);
    SinglePlayer_SetHidden(single_player.MissionSelectFrame, true);
    SinglePlayer_SetHidden(single_player.SlidingDoors, true);
    SinglePlayer_SetHidden(campaign_list_frame, !show_campaign || SinglePlayer_HasStaticCampaignButtons());
}

static void SinglePlayer_SetCampaignBackdrop(LPCWC3CAMPAIGN campaign) {
    if (single_player.CampaignBackdrop_2 && campaign && campaign->background[0]) {
        campaign_background_model = UI_LoadModel(campaign->background, true);
        single_player.CampaignBackdrop_2->Portrait.model = campaign_background_model;
        fprintf(stderr, "[UI] Campaign backdrop: skin=\"%s\" model_idx=%u\n",
                campaign->background, (unsigned)campaign_background_model);
    }
}

static void SinglePlayer_DrawCampaignBackdrop(void) {
    LPRENDERER renderer = uiimport.GetRenderer();
    LPCMODEL model = UI_GetModel(campaign_background_model);

    if (renderer && renderer->RenderFrame && model) {
        renderEntity_t entity = {0};
        entity.model = model;
        entity.scale = 1.0f;
        entity.flags = RF_NO_SHADOW | RF_NO_FOGOFWAR | RF_PORTRAIT_LIGHTING;
        renderer->SetEntityAnimFrame(model, "Stand", &entity);

        viewDef_t viewdef = {0};
        viewdef.viewport = (RECT){0, 0, 1, 1};
        viewdef.rdflags = RDF_NOWORLDMODEL | RDF_NOFRUSTUMCULL | RDF_NOFOG | RDF_USE_ENTITY_CAMERA;
        viewdef.num_entities = 1;
        viewdef.entities = &entity;

        renderer->RenderFrame(&viewdef);
    }
}

static void SinglePlayer_LaunchCampaign(LPCWC3CAMPAIGN campaign) {
    char command[256];
    LPCSTR map_path = SinglePlayer_FirstMissionMap(campaign);

    if (!map_path || !*map_path) {
        return;
    }
    snprintf(command, sizeof(command), "map \"%s\"", map_path);
    UI_MenuCommandLocal(command);
}

static void SinglePlayer_PopulateCampaignList(void) {
    memset(&campaign_list, 0, sizeof(campaign_list));
    FOR_LOOP(i, campaign_order_count) {
        DWORD const campaign_index = campaign_order[i];
        uiMapListItem_t *item;
        LPCWC3CAMPAIGN campaign;

        if (campaign_index >= campaign_count) {
            continue;
        }
        campaign = &campaigns[campaign_index];
        if (!SinglePlayer_FirstMissionMap(campaign)) {
            continue;
        }
        item = &campaign_list.items[campaign_list.count++];
        if (campaign->header[0] && campaign->name[0]) {
            snprintf(item->name, sizeof(item->name), "%.80s: %.46s", campaign->header, campaign->name);
        } else {
            snprintf(item->name, sizeof(item->name), "%s", campaign->name[0] ? campaign->name : campaign->key);
        }
        snprintf(item->path, sizeof(item->path), "%s", campaign->key);
        item->players = 1;
    }
}

static void SinglePlayer_CreateCampaignList(void) {
    LPFRAMEDEF template_frame;

    if (campaign_list_frame || SinglePlayer_HasStaticCampaignButtons() || !single_player.CampaignSelectFrame) {
        return;
    }

    template_frame = UI_FindFrame("MapListBox");
    if (!template_frame) {
        return;
    }

    campaign_list_frame = UI_CloneFrameTree(template_frame, single_player.CampaignSelectFrame);
    if (!campaign_list_frame) {
        return;
    }

    SinglePlayer_PopulateCampaignList();
    UI_SetSize(campaign_list_frame, 0.34f, 0.11f);
    UI_SetPoint(campaign_list_frame,
                FRAMEPOINT_BOTTOMLEFT,
                single_player.BackButton,
                FRAMEPOINT_TOPLEFT,
                -0.14f,
                0.04f);
    UI_BindMapList(campaign_list_frame, &campaign_list, single_player.DifficultySelectLabel, 4, "menu_single_player_campaign_select %u");
}

static void SinglePlayer_BindMainMenu(void) {
    if (!single_player.SinglePlayerMenu) {
        return;
    }

    UI_SetOnClick(single_player.CampaignButton, "menu_single_player_campaign");
    UI_SetOnClick(single_player.LoadSavedButton, "");
    UI_SetOnClick(single_player.ViewReplayButton, "");
    UI_SetOnClick(single_player.SkirmishButton, "");
    UI_SetOnClick(single_player.ProfileButton, "");
    UI_SetOnClick(single_player.CancelButton, "menu_main");
    if (single_player.ProfileNameText) {
        UI_SetText(single_player.ProfileNameText, "Player");
    }
}

static void SinglePlayer_BindCampaignMenu(void) {
    LPFRAMEDEF DifficultyMenu;
    LPFRAMEDEF DifficultyTitle;

    if (!single_player.CampaignMenu) {
        return;
    }

    UI_SetOnClick(single_player.BackButton, "menu_game");
    UI_SetOnClick(single_player.HumanButton, "menu_single_player_campaign_human");
    UI_SetOnClick(single_player.OrcButton, "menu_single_player_campaign_orc");
    UI_SetOnClick(single_player.UndeadButton, "menu_single_player_campaign_undead");
    UI_SetOnClick(single_player.NightElfButton, "menu_single_player_campaign_night_elf");
    UI_SetOnClick(single_player.TutorialButton, "menu_single_player_campaign_tutorial");

    DifficultyMenu = single_player.DifficultySelect
        ? UI_FindChildFrame(single_player.DifficultySelect, "CampaignPopupMenuMenu")
        : NULL;
    DifficultyTitle = single_player.DifficultySelect
        ? UI_FindChildFrame(single_player.DifficultySelect, "CampaignPopupMenuTitleTextTemplate")
        : NULL;

    if (DifficultyMenu) {
        UI_MenuClearItems(DifficultyMenu);
        UI_MenuAddItem(DifficultyMenu, UI_GetString("EASY"), 0);
        UI_MenuAddItem(DifficultyMenu, UI_GetString("NORMAL"), 1);
        UI_MenuAddItem(DifficultyMenu, UI_GetString("HARD"), 2);
        UI_SetOnClick(DifficultyMenu, "menu_single_player_difficulty %u");
        UI_SetHidden(DifficultyMenu, true);
    }
    if (DifficultyTitle) {
        UI_SetText(DifficultyTitle, "NORMAL");
    }
}

static void SinglePlayerMenu_Init(void) {
    uiimport.Printf("SinglePlayerMenu_Init\n");
    UI_PreloadGlueSceneModels();
    SinglePlayer_LoadCampaignData();
    campaign_list_frame = NULL;
    memset(&campaign_list, 0, sizeof(campaign_list));

    if (single_player.WarCraftIIILogo) {
        single_player.WarCraftIIILogo->Portrait.model = UI_LoadModel("CampaignLogo", true);
    }

    SinglePlayer_BindMainMenu();
    SinglePlayer_BindCampaignMenu();
    SinglePlayer_CreateCampaignList();
    SinglePlayer_SetCampaignBackdrop(SinglePlayer_DefaultCampaign());
    SinglePlayer_SetView(SINGLE_PLAYER_VIEW_MAIN);
}

static void SinglePlayerMenu_Shutdown(void) {
}

static void SinglePlayerMenu_Refresh(int msec) {
    (void)msec;
}

static void SinglePlayerMenu_Draw(void) {
    if (current_view == SINGLE_PLAYER_VIEW_CAMPAIGN_SELECT) {
        SinglePlayer_DrawCampaignBackdrop();
        if (single_player.CampaignMenu) {
            UI_DrawFrame(single_player.CampaignMenu);
        }
        return;
    }

    UI_DrawGlueScene("SinglePlayer Stand");
    if (single_player.SinglePlayerMenu) {
        UI_DrawFrame(single_player.SinglePlayerMenu);
    }
}

static void SinglePlayerMenu_KeyEvent(int key, BOOL down) {
    (void)key;
    (void)down;
}

void SinglePlayerMenu_ShowMain(void) {
    SinglePlayer_SetView(SINGLE_PLAYER_VIEW_MAIN);
}

void SinglePlayerMenu_ShowCampaign(void) {
    SinglePlayer_SetCampaignBackdrop(SinglePlayer_DefaultCampaign());
    SinglePlayer_SetView(SINGLE_PLAYER_VIEW_CAMPAIGN_SELECT);
}

void SinglePlayerMenu_LaunchCampaign(LPCSTR name) {
    LPCWC3CAMPAIGN campaign = SinglePlayer_FindCampaign(name);
    SinglePlayer_SetCampaignBackdrop(campaign);
    SinglePlayer_LaunchCampaign(campaign);
}

void SinglePlayerMenu_LaunchCampaignIndex(DWORD index) {
    DWORD campaign_index;

    if (index >= campaign_list.count || index >= campaign_order_count) {
        return;
    }
    campaign_list.selected = index;
    campaign_index = campaign_order[index];
    if (campaign_index < campaign_count) {
        SinglePlayer_SetCampaignBackdrop(&campaigns[campaign_index]);
        SinglePlayer_LaunchCampaign(&campaigns[campaign_index]);
    }
}

uiScreen_t singlePlayerMenuScreen = {
    .name = "single-player",
    .load = SinglePlayerMenu_LoadScreen,
    .init = SinglePlayerMenu_Init,
    .shutdown = SinglePlayerMenu_Shutdown,
    .refresh = SinglePlayerMenu_Refresh,
    .draw = SinglePlayerMenu_Draw,
    .key_event = SinglePlayerMenu_KeyEvent,
};

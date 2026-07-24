#include "common.h"

#ifndef _WIN32
#include <strings.h>
#endif
#include <ctype.h>

struct world_state world = { 0 };
static PATHSTR cm_loaded_map = { 0 };

void CM_ReadPathMap(HANDLE archive);

void PF_TextRemoveComments(LPSTR buffer);
BOMStatus PF_TextRemoveBom(LPSTR buffer);

DWORD SFileReadStringLength(HANDLE file) {
    DWORD filePosition = SFileSetFilePointer(file, 0, 0, FILE_CURRENT);
    DWORD stringLength = 1;
    while (true) {
        BYTE ch = 0;
        SFileReadFile(file, &ch, 1, NULL, NULL);
        if (ch == 0) {
            break;
        } else {
            stringLength++;
        }
    }
    SFileSetFilePointer(file, filePosition, 0, FILE_BEGIN);
    return stringLength;
}

#ifdef GAME_WORLD
void SFileReadString(HANDLE file, LPSTR *lppString) {
    DWORD stringLength = SFileReadStringLength(file);
    *lppString = MemAlloc(stringLength);
    SFileReadFile(file, *lppString, stringLength, NULL, NULL);
}

static DWORD SFileBytesRemaining(HANDLE file) {
    DWORD position = SFileSetFilePointer(file, 0, 0, FILE_CURRENT);
    DWORD size = SFileGetFileSize(file, NULL);

    if (position >= size) {
        return 0;
    }
    return size - position;
}

static BOOL CM_ReadInfoInto(HANDLE archive, LPMAPINFO info, BOOL setup_only) {
    HANDLE file;

    if (!archive || !info) {
        return false;
    }
    if (!SFileOpenFileEx(archive, "war3map.w3i", SFILE_OPEN_FROM_MPQ, &file)) {
        return false;
    }
    SFileReadFile(file, &info->fileFormat, 4, NULL, NULL);
    SFileReadFile(file, &info->numberOfSaves, sizeof(DWORD), NULL, NULL);
    SFileReadFile(file, &info->editorVersion, sizeof(DWORD), NULL, NULL);
    if (info->fileFormat >= 28) {
        SFileReadFile(file, &info->gameVersionMajor, sizeof(DWORD), NULL, NULL);
        SFileReadFile(file, &info->gameVersionMinor, sizeof(DWORD), NULL, NULL);
        SFileReadFile(file, &info->gameVersionPatch, sizeof(DWORD), NULL, NULL);
        SFileReadFile(file, &info->gameVersionBuild, sizeof(DWORD), NULL, NULL);
    }
    SFileReadString(file, &info->mapName);
    SFileReadString(file, &info->mapAuthor);
    SFileReadString(file, &info->mapDescription);
    SFileReadString(file, &info->playersRecommended);
    SFileReadFile(file, &info->cameraBounds, sizeof(mapCameraBounds_t), NULL, NULL);
    SFileReadFile(file, &info->playableArea, sizeof(size2_t), NULL, NULL);
    SFileReadFile(file, &info->flags, sizeof(DWORD), NULL, NULL);
    SFileReadFile(file, &info->mainGroundType, sizeof(char), NULL, NULL);
    SFileReadFile(file, &info->campaignBackgroundNumber, sizeof(DWORD), NULL, NULL);
    if (info->fileFormat >= 25) {
        SFileReadString(file, &info->loadingScreenModel);
    }
    SFileReadString(file, &info->loadingScreenText);
    SFileReadString(file, &info->loadingScreenTitle);
    SFileReadString(file, &info->loadingScreenSubtitle);
    if (info->fileFormat >= 25) {
        SFileReadFile(file, &info->gameDataSet, sizeof(DWORD), NULL, NULL);
    } else {
        SFileReadFile(file, &info->loadingScreenNumber, sizeof(DWORD), NULL, NULL);
    }
    if (info->fileFormat >= 25) {
        SFileReadString(file, &info->prologueScreenModel);
    }
    SFileReadString(file, &info->prologueScreenText);
    SFileReadString(file, &info->prologueScreenTitle);
    SFileReadString(file, &info->prologueScreenSubtitle);
    if (info->fileFormat >= 25) {
        SFileReadFile(file, &info->fogStyle, sizeof(DWORD), NULL, NULL);
        SFileReadFile(file, &info->fogStartZ, sizeof(FLOAT), NULL, NULL);
        SFileReadFile(file, &info->fogEndZ, sizeof(FLOAT), NULL, NULL);
        SFileReadFile(file, &info->fogDensity, sizeof(FLOAT), NULL, NULL);
        SFileReadFile(file, &info->fogColor, sizeof(COLOR32), NULL, NULL);
        SFileReadFile(file, &info->weatherID, sizeof(DWORD), NULL, NULL);
        SFileReadString(file, &info->soundEnvironment);
        SFileReadFile(file, &info->lightEnvironmentTileset, sizeof(BYTE), NULL, NULL);
        SFileReadFile(file, &info->waterColor, sizeof(COLOR32), NULL, NULL);
    }
    if (info->fileFormat >= 28) {
        SFileReadFile(file, &info->scriptType, sizeof(DWORD), NULL, NULL);
    }
    if (info->fileFormat >= 31) {
        SFileReadFile(file, &info->supportedModes, sizeof(DWORD), NULL, NULL);
        SFileReadFile(file, &info->gameDataVersion, sizeof(DWORD), NULL, NULL);
    }
    if (info->fileFormat >= 32) {
        SFileReadFile(file, &info->defaultZoomOverride, sizeof(DWORD), NULL, NULL);
        SFileReadFile(file, &info->maximumZoomOverride, sizeof(DWORD), NULL, NULL);
    }
    if (info->fileFormat >= 33) {
        SFileReadFile(file, &info->minimumZoomOverride, sizeof(DWORD), NULL, NULL);
    }

    DWORD num_players = 0;
    SFileReadFile(file, &num_players, sizeof(DWORD), NULL, NULL);
    FOR_LOOP(i, num_players) {
        DWORD playerNumber = 0;
        mapPlayer_t scratch = { 0 };
        mapPlayer_t *player;

        SFileReadFile(file, &playerNumber, sizeof(DWORD), NULL, NULL);
        player = playerNumber < MAX_PLAYERS ? info->players + playerNumber : &scratch;
        player->used = true;
        SFileReadFile(file, &player->playerType, sizeof(playerType_t), NULL, NULL);
        SFileReadFile(file, &player->playerRace, sizeof(playerRace_t), NULL, NULL);
        SFileReadFile(file, &player->flags, sizeof(DWORD), NULL, NULL);
        SFileReadString(file, &player->playerName);
        SFileReadFile(file, &player->startingPosition, sizeof(VECTOR2), NULL, NULL);
        SFileReadFile(file, &player->allyLowPrioritiesFlags, sizeof(DWORD), NULL, NULL);
        SFileReadFile(file, &player->allyHighPrioritiesFlags, sizeof(DWORD), NULL, NULL);
        if (info->fileFormat >= 31) {
            SFileReadFile(file, &player->enemyLowPrioritiesFlags, sizeof(DWORD), NULL, NULL);
            SFileReadFile(file, &player->enemyHighPrioritiesFlags, sizeof(DWORD), NULL, NULL);
        }
        SAFE_DELETE(scratch.playerName, MemFree);
    }

    SFileReadFile(file, &info->num_teams, sizeof(DWORD), NULL, NULL);
    info->teams = MemAlloc(sizeof(mapTeam_t) * info->num_teams);
    FOR_LOOP(i, info->num_teams) {
        mapTeam_t *force = &info->teams[i];
        SFileReadFile(file, &force->flags, sizeof(DWORD), NULL, NULL);
        SFileReadFile(file, &force->playerMasks, sizeof(DWORD), NULL, NULL);
        SFileReadString(file, &force->name);
    }

    if (setup_only || SFileBytesRemaining(file) <= 1) {
        SFileCloseFile(file);
        return true;
    }
    SFileReadFile(file, &info->num_upgradeAvailabilities, sizeof(DWORD), NULL, NULL);
    info->upgradeAvailabilities = MemAlloc(sizeof(mapUpgradeAvailability_t) * info->num_upgradeAvailabilities);
    FOR_LOOP(i, info->num_upgradeAvailabilities) {
        mapUpgradeAvailability_t *upgrade = &info->upgradeAvailabilities[i];
        SFileReadFile(file, &upgrade->playerFlags, sizeof(DWORD), NULL, NULL);
        SFileReadFile(file, &upgrade->upgradeID, sizeof(DWORD), NULL, NULL);
        SFileReadFile(file, &upgrade->levelOfTheUpgrade, sizeof(DWORD), NULL, NULL);
        SFileReadFile(file, &upgrade->availability, sizeof(upgradeAvailability_t), NULL, NULL);
    }

    if (SFileBytesRemaining(file) <= 1) {
        SFileCloseFile(file);
        return true;
    }
    SFileReadFile(file, &info->num_techAvailabilities, sizeof(DWORD), NULL, NULL);
    info->techAvailabilities = MemAlloc(sizeof(mapTechAvailability_t) * info->num_techAvailabilities);
    FOR_LOOP(i, info->num_techAvailabilities) {
        mapTechAvailability_t *tech = &info->techAvailabilities[i];
        SFileReadFile(file, &tech->playerFlags, sizeof(DWORD), NULL, NULL);
        SFileReadFile(file, &tech->techID, sizeof(DWORD), NULL, NULL);
    }

    if (SFileBytesRemaining(file) <= 1) {
        SFileCloseFile(file);
        return true;
    }
    SFileReadFile(file, &info->num_randomUnits, sizeof(DWORD), NULL, NULL);
    info->randomUnits = MemAlloc(sizeof(mapRandomUnitTable_t) * info->num_randomUnits);
    FOR_LOOP(i, info->num_randomUnits) {
        mapRandomUnitTable_t *table = &info->randomUnits[i];
        SFileReadFile(file, &table->tableNumber, sizeof(DWORD), NULL, NULL);
        SFileReadString(file, &table->tableName);
        SFileReadFile(file, &table->num_positions, sizeof(DWORD), NULL, NULL);
        if (table->num_positions > 0) {
            table->positionTypes = MemAlloc(sizeof(mapRandomGroupPositionType_t) * table->num_positions);
            SFileReadFile(file,
                          table->positionTypes,
                          sizeof(mapRandomGroupPositionType_t) * table->num_positions,
                          NULL,
                          NULL);
        }
        SFileReadFile(file, &table->num_units, sizeof(DWORD), NULL, NULL);
        if (table->num_units > 0) {
            table->units = MemAlloc(sizeof(mapRandomUnit_t) * table->num_units);
        }
        FOR_LOOP(j, table->num_units) {
            mapRandomUnit_t *unit = &table->units[j];
            SFileReadFile(file, &unit->chance, sizeof(DWORD), NULL, NULL);
            if (table->num_positions > 0) {
                unit->itemIDs = MemAlloc(sizeof(DWORD) * table->num_positions);
                SFileReadFile(file, unit->itemIDs, sizeof(DWORD) * table->num_positions, NULL, NULL);
            }
        }
    }

    if (info->fileFormat >= 25 && SFileBytesRemaining(file) > 1) {
        SFileReadFile(file, &info->num_randomItems, sizeof(DWORD), NULL, NULL);
        info->randomItems = MemAlloc(sizeof(mapRandomItemTable_t) * info->num_randomItems);
        FOR_LOOP(i, info->num_randomItems) {
            mapRandomItemTable_t *table = &info->randomItems[i];
            SFileReadFile(file, &table->tableNumber, sizeof(DWORD), NULL, NULL);
            SFileReadString(file, &table->tableName);
            SFileReadFile(file, &table->num_sets, sizeof(DWORD), NULL, NULL);
            if (table->num_sets > 0) {
                table->sets = MemAlloc(sizeof(mapRandomItemSet_t) * table->num_sets);
            }
            FOR_LOOP(j, table->num_sets) {
                mapRandomItemSet_t *set = &table->sets[j];
                SFileReadFile(file, &set->num_items, sizeof(DWORD), NULL, NULL);
                if (set->num_items > 0) {
                    set->items = MemAlloc(sizeof(mapRandomItem_t) * set->num_items);
                    SFileReadFile(file, set->items, sizeof(mapRandomItem_t) * set->num_items, NULL, NULL);
                }
            }
        }
    }

    SFileCloseFile(file);
    return true;
}

static void CM_ReadInfo(HANDLE archive) {
    CM_ReadInfoInto(archive, &world.info, false);
}
#endif

static void CM_FreeObjectOverrides(DWORD count, unitData_t *objects) {
    if (!objects) return;
    FOR_LOOP(i, count) {
        FOR_LOOP(j, objects[i].numbeOfModifications)
            SAFE_DELETE(objects[i].modifications[j].data, MemFree);
        SAFE_DELETE(objects[i].modifications, MemFree);
    }
    MemFree(objects);
}

static void MapInfo_Release(LPMAPINFO mapInfo) {
    mapTrigStr_t *string = mapInfo ? mapInfo->strings : NULL;

    if (!mapInfo) {
        return;
    }
    FOR_LOOP(i, MAX_PLAYERS) {
        SAFE_DELETE(mapInfo->players[i].playerName, MemFree);
    }
    FOR_LOOP(i, mapInfo->num_teams) {
        SAFE_DELETE(mapInfo->teams[i].name, MemFree);
    }
    FOR_LOOP(i, mapInfo->num_randomUnits) {
        FOR_LOOP(j, mapInfo->randomUnits[i].num_units) {
            SAFE_DELETE(mapInfo->randomUnits[i].units[j].itemIDs, MemFree);
        }
        SAFE_DELETE(mapInfo->randomUnits[i].units, MemFree);
        SAFE_DELETE(mapInfo->randomUnits[i].positionTypes, MemFree);
        SAFE_DELETE(mapInfo->randomUnits[i].tableName, MemFree);
    }
    FOR_LOOP(i, mapInfo->num_randomItems) {
        FOR_LOOP(j, mapInfo->randomItems[i].num_sets) {
            SAFE_DELETE(mapInfo->randomItems[i].sets[j].items, MemFree);
        }
        SAFE_DELETE(mapInfo->randomItems[i].sets, MemFree);
        SAFE_DELETE(mapInfo->randomItems[i].tableName, MemFree);
    }
    SAFE_DELETE(mapInfo->mapName, MemFree);
    SAFE_DELETE(mapInfo->mapAuthor, MemFree);
    SAFE_DELETE(mapInfo->mapDescription, MemFree);
    SAFE_DELETE(mapInfo->playersRecommended, MemFree);
    SAFE_DELETE(mapInfo->loadingScreenModel, MemFree);
    SAFE_DELETE(mapInfo->loadingScreenText, MemFree);
    SAFE_DELETE(mapInfo->loadingScreenTitle, MemFree);
    SAFE_DELETE(mapInfo->loadingScreenSubtitle, MemFree);
    SAFE_DELETE(mapInfo->prologueScreenModel, MemFree);
    SAFE_DELETE(mapInfo->prologueScreenText, MemFree);
    SAFE_DELETE(mapInfo->prologueScreenTitle, MemFree);
    SAFE_DELETE(mapInfo->prologueScreenSubtitle, MemFree);
    SAFE_DELETE(mapInfo->soundEnvironment, MemFree);
    SAFE_DELETE(mapInfo->teams, MemFree);
    SAFE_DELETE(mapInfo->upgradeAvailabilities, MemFree);
    SAFE_DELETE(mapInfo->techAvailabilities, MemFree);
    SAFE_DELETE(mapInfo->randomUnits, MemFree);
    SAFE_DELETE(mapInfo->randomItems, MemFree);
    CM_FreeObjectOverrides(mapInfo->num_originalUnits, mapInfo->originalUnits);
    CM_FreeObjectOverrides(mapInfo->num_userCreatedUnits, mapInfo->userCreatedUnits);
    CM_FreeObjectOverrides(mapInfo->num_originalDestructables, mapInfo->originalDestructables);
    CM_FreeObjectOverrides(mapInfo->num_userCreatedDestructables, mapInfo->userCreatedDestructables);
    while (string) {
        mapTrigStr_t *next = string->next;
        MemFree(string);
        string = next;
    }
    SAFE_DELETE(mapInfo->mapscript, MemFree);
    memset(mapInfo, 0, sizeof(*mapInfo));
}

#define CM_PLACEMENT_ROC_VERSION 7
#define CM_PLACEMENT_TFT_VERSION 8
#define CM_DOO_MAGIC MAKEFOURCC('W', '3', 'd', 'o')
#define CM_DROPPABLE_ITEM_DISK_SIZE 8
#define CM_INVENTORY_ITEM_DISK_SIZE 8
#define CM_MODIFIED_ABILITY_DISK_SIZE 12

typedef struct {
    DWORD version;
    DWORD subversion;
    DWORD count;
    BOOL tft;
} cmPlacementHeader_t;

#ifdef GAME_WORLD
static BOOL CM_ReadPlacementHeader(HANDLE file, LPCSTR filename, cmPlacementHeader_t *header) {
    DWORD magic;

    if (!file || !header) {
        return false;
    }
    memset(header, 0, sizeof(*header));
    if (!SFileReadFile(file, &magic, sizeof(magic), NULL, NULL) ||
        !SFileReadFile(file, &header->version, sizeof(header->version), NULL, NULL) ||
        !SFileReadFile(file, &header->subversion, sizeof(header->subversion), NULL, NULL) ||
        !SFileReadFile(file, &header->count, sizeof(header->count), NULL, NULL)) {
        fprintf(stderr, "CM_ReadPlacementHeader: short %s header\n", filename);
        return false;
    }
    if (magic != CM_DOO_MAGIC) {
        fprintf(stderr, "CM_ReadPlacementHeader: invalid %s magic 0x%08x\n", filename, (unsigned)magic);
        return false;
    }
    if (header->version != CM_PLACEMENT_ROC_VERSION &&
        header->version != CM_PLACEMENT_TFT_VERSION) {
        fprintf(stderr,
                "CM_ReadPlacementHeader: unsupported %s version %u subversion %u\n",
                filename,
                (unsigned)header->version,
                (unsigned)header->subversion);
        return false;
    }
    header->tft = header->version == CM_PLACEMENT_TFT_VERSION;
    return true;
}

static BOOL CM_ReadCount(HANDLE file, DWORD elem_size, DWORD *count, LPCSTR context) {
    DWORD max_count;

    if (!file || !count || elem_size == 0) {
        return false;
    }
    *count = 0;
    if (!SFileReadFile(file, count, sizeof(*count), NULL, NULL)) {
        fprintf(stderr, "CM_ReadCount: short count for %s\n", context);
        return false;
    }
    max_count = SFileBytesRemaining(file) / elem_size;
    if (*count > max_count) {
        fprintf(stderr,
                "CM_ReadCount: invalid %s count %u max=%u remaining=%u elem=%u\n",
                context,
                (unsigned)*count,
                (unsigned)max_count,
                (unsigned)SFileBytesRemaining(file),
                (unsigned)elem_size);
        return false;
    }
    return true;
}

static void CM_FreeDroppedItemSets(DWORD num_sets, droppableItemSet_t *sets) {
    if (!sets) {
        return;
    }
    FOR_LOOP(i, num_sets) {
        SAFE_DELETE(sets[i].droppableItems, MemFree);
    }
    MemFree(sets);
}

static void CM_FreeDoodadPlacementData(LPDOODAD doodad) {
    if (!doodad) {
        return;
    }
    CM_FreeDroppedItemSets(doodad->num_droppedItemSets, doodad->droppableItemSets);
    SAFE_DELETE(doodad->inventoryItems, MemFree);
    SAFE_DELETE(doodad->modifiedAbilities, MemFree);
    SAFE_DELETE(doodad->diffAvailUnits, MemFree);
}

static BOOL CM_ReadDroppableItems(HANDLE file, DWORD *num, droppableItem_t **data, LPCSTR context) {
    DWORD count;

    if (!num || !data) {
        return false;
    }
    *num = 0;
    *data = NULL;
    if (!CM_ReadCount(file, CM_DROPPABLE_ITEM_DISK_SIZE, &count, context)) {
        return false;
    }
    *num = count;
    if (count > 0) {
        *data = MemAlloc(count * sizeof(**data));
    }
    FOR_LOOP(i, count) {
        DWORD chance;

        SFileReadFile(file, &(*data)[i].itemID, sizeof(DWORD), NULL, NULL);
        SFileReadFile(file, &chance, sizeof(chance), NULL, NULL);
        (*data)[i].chanceToDrop = (int)chance;
    }
    return true;
}

static BOOL CM_ReadDroppedItemSets(HANDLE file, DWORD *num_sets, droppableItemSet_t **sets, LPCSTR context) {
    DWORD count;

    if (!num_sets || !sets) {
        return false;
    }
    *num_sets = 0;
    *sets = NULL;
    if (!CM_ReadCount(file, sizeof(DWORD), &count, context)) {
        return false;
    }
    *num_sets = count;
    if (count > 0) {
        *sets = MemAlloc(count * sizeof(**sets));
    }
    FOR_LOOP(i, count) {
        droppableItemSet_t *set = &(*sets)[i];
        char item_context[128];
        DWORD num_items;

        snprintf(item_context, sizeof(item_context), "%s item set %u", context, (unsigned)i);
        if (!CM_ReadDroppableItems(file, &num_items, &set->droppableItems, item_context)) {
            CM_FreeDroppedItemSets(count, *sets);
            *sets = NULL;
            *num_sets = 0;
            return false;
        }
        set->num_droppableItems = (int)num_items;
    }
    return true;
}

static void CM_ReadDoodads(HANDLE archive) {
    HANDLE file;
    cmPlacementHeader_t header;

    if (!SFileOpenFileEx(archive, "war3map.doo", SFILE_OPEN_FROM_MPQ, &file)) {
        fprintf(stderr, "CM_ReadDoodads: missing war3map.doo\n");
        return;
    }
    if (!CM_ReadPlacementHeader(file, "war3map.doo", &header)) {
        SFileCloseFile(file);
        return;
    }

    FOR_LOOP(index, header.count) {
        DWORD count;
        LPDOODAD doodad = MemAlloc(sizeof(DOODAD));
        char context[128];

        snprintf(context, sizeof(context), "war3map.doo doodad %u", (unsigned)index);
        doodad->player = PLAYER_NEUTRAL_PASSIVE;
        doodad->hitPoints = (DWORD)-1;
        doodad->manaPoints = (DWORD)-1;
        doodad->droppedItemSetPtr = (DWORD)-1;
        doodad->goldAmount = 12500;
        doodad->targetAcquisition = -1.0f;
        SFileReadFile(file, &doodad->doodID, sizeof(DWORD), NULL, NULL);
        SFileReadFile(file, &doodad->variation, sizeof(DWORD), NULL, NULL);
        SFileReadFile(file, &doodad->position, sizeof(VECTOR3), NULL, NULL);
        SFileReadFile(file, &doodad->angle, sizeof(FLOAT), NULL, NULL);
        SFileReadFile(file, &doodad->scale, sizeof(VECTOR3), NULL, NULL);
        SFileReadFile(file, &doodad->flags, sizeof(BYTE), NULL, NULL);
        SFileReadFile(file, &doodad->treeLife, sizeof(BYTE), NULL, NULL);
        if (header.tft) {
            SFileReadFile(file, &doodad->droppedItemSetPtr, sizeof(DWORD), NULL, NULL);
            if (!CM_ReadDroppedItemSets(file, &count, &doodad->droppableItemSets, context)) {
                CM_FreeDoodadPlacementData(doodad);
                MemFree(doodad);
                break;
            }
            doodad->num_droppedItemSets = count;
        }
        SFileReadFile(file, &doodad->unitID, sizeof(DWORD), NULL, NULL);
        
        ADD_TO_LIST(doodad, world.doodads);
    }

    SFileCloseFile(file);
}

static BOOL CM_ReadInventoryItems(HANDLE file, DWORD *num, inventoryItem_t **data, LPCSTR context) {
    DWORD count;

    if (!num || !data) {
        return false;
    }
    *num = 0;
    *data = NULL;
    if (!CM_ReadCount(file, CM_INVENTORY_ITEM_DISK_SIZE, &count, context)) {
        return false;
    }
    *num = count;
    if (count > 0) {
        *data = MemAlloc(count * sizeof(**data));
    }
    FOR_LOOP(i, count) {
        SFileReadFile(file, &(*data)[i].slot, sizeof(DWORD), NULL, NULL);
        SFileReadFile(file, &(*data)[i].itemID, sizeof(DWORD), NULL, NULL);
    }
    return true;
}

static BOOL CM_ReadModifiedAbilities(HANDLE file, DWORD *num, modifiedAbility_t **data, LPCSTR context) {
    DWORD count;

    if (!num || !data) {
        return false;
    }
    *num = 0;
    *data = NULL;
    if (!CM_ReadCount(file, CM_MODIFIED_ABILITY_DISK_SIZE, &count, context)) {
        return false;
    }
    *num = count;
    if (count > 0) {
        *data = MemAlloc(count * sizeof(**data));
    }
    FOR_LOOP(i, count) {
        SFileReadFile(file, &(*data)[i].abilityID, sizeof(DWORD), NULL, NULL);
        SFileReadFile(file, &(*data)[i].active, sizeof(DWORD), NULL, NULL);
        SFileReadFile(file, &(*data)[i].level, sizeof(DWORD), NULL, NULL);
    }
    return true;
}

static BOOL CM_ReadUnit(HANDLE file, struct Doodad *unit, cmPlacementHeader_t const *header, DWORD index) {
    char context[128];
    char array_context[256];

    snprintf(context, sizeof(context), "war3mapUnits.doo unit %u", (unsigned)index);

    SFileReadFile(file, &unit->doodID, sizeof(DWORD), NULL, NULL);
    SFileReadFile(file, &unit->variation, sizeof(DWORD), NULL, NULL);
    SFileReadFile(file, &unit->position, sizeof(VECTOR3), NULL, NULL);
    SFileReadFile(file, &unit->angle, sizeof(FLOAT), NULL, NULL);
    SFileReadFile(file, &unit->scale, sizeof(VECTOR3), NULL, NULL);
    SFileReadFile(file, &unit->flags, sizeof(BYTE), NULL, NULL);
    SFileReadFile(file, &unit->player, sizeof(DWORD), NULL, NULL);
    SFileReadFile(file, &unit->unknown1, sizeof(BYTE), NULL, NULL);
    SFileReadFile(file, &unit->unknown2, sizeof(BYTE), NULL, NULL);
    SFileReadFile(file, &unit->hitPoints, sizeof(DWORD), NULL, NULL); // (-1 = use default)
    SFileReadFile(file, &unit->manaPoints, sizeof(DWORD), NULL, NULL); // (-1 = use default, 0 = unit doesn't have mana)
    if (header->tft) {
        SFileReadFile(file, &unit->droppedItemSetPtr, sizeof(DWORD), NULL, NULL);
    } else {
        unit->droppedItemSetPtr = (DWORD)-1;
    }
    if (!CM_ReadDroppedItemSets(file, &unit->num_droppedItemSets, &unit->droppableItemSets, context)) {
        return false;
    }

    SFileReadFile(file, &unit->goldAmount, sizeof(DWORD), NULL, NULL); // (default = 12500)
    SFileReadFile(file, &unit->targetAcquisition, sizeof(FLOAT), NULL, NULL); // (-1 = normal, -2 = camp)

    // war3mapUnits.doo stores only the file-format hero fields here.  The
    // runtime doodadHero_t has extra state, so do not read sizeof(hero).
    SFileReadFile(file, &unit->hero.level, sizeof(DWORD), NULL, NULL);
    if (header->tft) {
        SFileReadFile(file, &unit->hero.str, sizeof(DWORD), NULL, NULL);
        SFileReadFile(file, &unit->hero.agi, sizeof(DWORD), NULL, NULL);
        SFileReadFile(file, &unit->hero.intel, sizeof(DWORD), NULL, NULL);
    }
    unit->hero.xp = 0;
    unit->hero.suspend_xp = false;

    snprintf(array_context, sizeof(array_context), "%s inventory", context);
    if (!CM_ReadInventoryItems(file, &unit->num_inventoryItems, &unit->inventoryItems, array_context)) {
        return false;
    }
    snprintf(array_context, sizeof(array_context), "%s abilities", context);
    if (!CM_ReadModifiedAbilities(file, &unit->num_modifiedAbilities, &unit->modifiedAbilities, array_context)) {
        return false;
    }

    SFileReadFile(file, &unit->randomUnitFlag, sizeof(DWORD), NULL, NULL); // "r" (for uDNR units and iDNR items)

    switch ((LONG)unit->randomUnitFlag) {
        case -1:
            break;
        case 0:
            SFileReadFile(file, &unit->levelOfRandomItem, sizeof(DWORD), NULL, NULL);
            break;
        case 1:
            SFileReadFile(file, &unit->randomUnitGroupNumber, sizeof(DWORD), NULL, NULL);
            SFileReadFile(file, &unit->randomUnitPositionNumber, sizeof(DWORD), NULL, NULL);
            break;
        case 2:
            snprintf(array_context, sizeof(array_context), "%s random unit choices", context);
            if (!CM_ReadDroppableItems(file, &unit->num_diffAvailUnits, &unit->diffAvailUnits, array_context)) {
                return false;
            }
            break;
        default:
            fprintf(stderr,
                    "CM_ReadUnit: invalid random unit flag %d in %s\n",
                    (int)unit->randomUnitFlag,
                    context);
            return false;
    }
    SFileReadFile(file, &unit->color, sizeof(COLOR32), NULL, NULL);
    SFileReadFile(file, &unit->waygate, sizeof(DWORD), NULL, NULL);
    SFileReadFile(file, &unit->unitID, sizeof(DWORD), NULL, NULL);
    return true;
}

static void CM_ReadUnitDoodads(HANDLE archive) {
    HANDLE file;
    cmPlacementHeader_t header;

    if (!SFileOpenFileEx(archive, "war3mapUnits.doo", SFILE_OPEN_FROM_MPQ, &file)) {
        fprintf(stderr, "CM_ReadUnitDoodads: missing war3mapUnits.doo\n");
        return;
    }
    if (!CM_ReadPlacementHeader(file, "war3mapUnits.doo", &header)) {
        SFileCloseFile(file);
        return;
    }

    FOR_LOOP(index, header.count) {
        LPDOODAD doodad = MemAlloc(sizeof(DOODAD));
        if (!CM_ReadUnit(file, doodad, &header, index)) {
            CM_FreeDoodadPlacementData(doodad);
            MemFree(doodad);
            break;
        }
        ADD_TO_LIST(doodad, world.doodads);
    }

    SFileCloseFile(file);
}

static BOOL CM_ReadWar3MapVertex(HANDLE file, LPWAR3MAPVERTEX vert) {
    WORD water_and_edge;
    BYTE flags;
    BYTE variation;
    BYTE cliff_and_layer;

    if (!file || !vert) {
        return false;
    }
    memset(vert, 0, sizeof(*vert));
    if (!SFileReadFile(file, &vert->accurate_height, sizeof(vert->accurate_height), NULL, NULL)) {
        return false;
    }
    if (!SFileReadFile(file, &water_and_edge, sizeof(water_and_edge), NULL, NULL)) {
        return false;
    }
    if (!SFileReadFile(file, &flags, sizeof(flags), NULL, NULL)) {
        return false;
    }
    if (!SFileReadFile(file, &variation, sizeof(variation), NULL, NULL)) {
        return false;
    }
    if (!SFileReadFile(file, &cliff_and_layer, sizeof(cliff_and_layer), NULL, NULL)) {
        return false;
    }

    vert->waterlevel = water_and_edge & 0x3FFF;
    vert->mapedge = (water_and_edge & 0x4000) != 0;
    vert->ground = flags & 0x0F;
    vert->ramp = (flags & 0x10) != 0;
    vert->blight = (flags & 0x20) != 0;
    vert->water = (flags & 0x40) != 0;
    vert->boundary = (flags & 0x80) != 0;
    vert->cliffVariation = (variation >> 5) & 0x07;
    vert->groundVariation = variation & 0x1F;
    vert->cliff = (cliff_and_layer >> 4) & 0x0F;
    vert->level = cliff_and_layer & 0x0F;
    return true;
}

static void CM_ReadHeightmap(HANDLE archive) {
    world.map = MemAlloc(sizeof(WAR3MAP));
    HANDLE file;
    if (!SFileOpenFileEx(archive, "war3map.w3e", SFILE_OPEN_FROM_MPQ, &file)) {
        return;
    }
    SFileReadFile(file, &world.map->header, 4, NULL, NULL);
    SFileReadFile(file, &world.map->version, 4, NULL, NULL);
    SFileReadFile(file, &world.map->tileset, 1, NULL, NULL);
    SFileReadFile(file, &world.map->custom, 4, NULL, NULL);
    SFileReadArray(file, world.map, grounds, 4, MemAlloc);
    SFileReadArray(file, world.map, cliffs, 4, MemAlloc);
    SFileReadFile(file, &world.map->width, 4, NULL, NULL);
    SFileReadFile(file, &world.map->height, 4, NULL, NULL);
    SFileReadFile(file, &world.map->center, 8, NULL, NULL);
    DWORD const num_vertices = world.map->width * world.map->height;
    int const vertexblocksize = sizeof(WAR3MAPVERTEX) * num_vertices;
    world.map->vertices = MemAlloc(vertexblocksize);
    FOR_LOOP(i, num_vertices) {
        if (!CM_ReadWar3MapVertex(file, (LPWAR3MAPVERTEX)world.map->vertices + i)) {
            break;
        }
    }
    SFileCloseFile(file);
}
#endif

#define CM_MAX_OBJECT_OVERRIDE_COUNT MAX_GAME_ENTITIES
#define CM_MAX_OBJECT_MODIFICATION_COUNT MAX_GAME_ENTITIES

static DWORD CM_ObjectBytesRemaining(HANDLE file) {
    DWORD position = SFileSetFilePointer(file, 0, 0, FILE_CURRENT);
    DWORD size = SFileGetFileSize(file, NULL);
    return position < size ? size - position : 0;
}

static BOOL CM_ReadObjectBytes(HANDLE file, void *out, DWORD size) {
    DWORD read = 0;
    return SFileReadFile(file, out, size, &read, NULL) && read == size;
}

static BOOL CM_ReadObjectStringLength(HANDLE file, DWORD *length) {
    DWORD position = SFileSetFilePointer(file, 0, 0, FILE_CURRENT);
    DWORD limit = MIN(CM_ObjectBytesRemaining(file), MAX_TRIGSTR_LENGTH);
    BYTE value;
    *length = 0;
    for (DWORD i = 0; i < limit; i++) {
        if (!CM_ReadObjectBytes(file, &value, 1)) break;
        if (!value) {
            *length = i + 1;
            break;
        }
    }
    SFileSetFilePointer(file, position, 0, FILE_BEGIN);
    return *length != 0;
}

static BOOL CM_ReadObjectCount(HANDLE file, DWORD elem_size, DWORD limit, DWORD *count, LPCSTR context) {
    DWORD remaining;
    if (!CM_ReadObjectBytes(file, count, sizeof(*count))) {
        fprintf(stderr, "CM_ReadObjectCount: short count for %s\n", context);
        return false;
    }
    remaining = CM_ObjectBytesRemaining(file);
    if (*count <= limit && *count <= remaining / elem_size) return true;
    fprintf(stderr, "CM_ReadObjectCount: invalid %s count %u limit=%u remaining=%u elem=%u\n",
            context, (unsigned)*count, (unsigned)limit, (unsigned)remaining, (unsigned)elem_size);
    return false;
}

static BOOL CM_ReadModification(HANDLE file, unitModification_t *mod, LPCSTR context) {
    DWORD strlength = 0;
    if (!CM_ReadObjectBytes(file, &mod->modID, 4) ||
        !CM_ReadObjectBytes(file, &mod->type, 4)) {
        fprintf(stderr, "CM_ReadModification: short header for %s\n", context);
        return false;
    }
    switch (mod->type) {
        case mod_int:
        case mod_real:
        case mod_unreal:
            mod->data = MemAlloc(4);
            if (!mod->data) return false;
            if (!CM_ReadObjectBytes(file, mod->data, 4)) return false;
            break;
        case mod_bool:
        case mod_char:
            mod->data = MemAlloc(1);
            if (!mod->data) return false;
            if (!CM_ReadObjectBytes(file, mod->data, 1)) return false;
            break;
        case mod_string:
        case mod_unitList:
        case mod_itemList:
        case mod_regenType:
        case mod_attackType:
        case mod_weaponType:
        case mod_targetType:
        case mod_moveType:
        case mod_defenseType:
        case mod_pathingTexture:
        case mod_upgradeList:
        case mod_stringList:
        case mod_abilityList:
        case mod_heroAbilityList:
        case mod_missileArt:
        case mod_attributeType:
        case mod_attackBits:
            if (!CM_ReadObjectStringLength(file, &strlength)) {
                fprintf(stderr, "CM_ReadModification: unterminated string for %s\n", context);
                return false;
            }
            mod->data = MemAlloc(strlength);
            if (!mod->data) return false;
            if (!CM_ReadObjectBytes(file, mod->data, strlength)) return false;
            break;
        default:
            fprintf(stderr, "CM_ReadModification: invalid type %u for %s\n", (unsigned)mod->type, context);
            return false;
    }
    return true;
}

static BOOL CM_ReadUnitsOverrides(HANDLE file, LPCSTR identity, DWORD *numUnits, unitData_t **out) {
    unitData_t *units;
    DWORD count, total_modifications = 0;
    DWORD unknown;
    char context[128];
    *numUnits = 0; *out = NULL;
    if (!CM_ReadObjectCount(file, 12, CM_MAX_OBJECT_OVERRIDE_COUNT, &count, identity)) return false;
    units = count ? MemAlloc(count * sizeof(*units)) : NULL;
    if (count && !units) return false;
    if (units) memset(units, 0, count * sizeof(*units));
    FOR_LOOP(i, count) {
        unitData_t *unit = units + i;
        snprintf(context, sizeof(context), "%s object %u", identity, (unsigned)i);
        if (!CM_ReadObjectBytes(file, &unit->originalUnitID, 4) ||
            !CM_ReadObjectBytes(file, &unit->newUnitID, 4) ||
            !CM_ReadObjectCount(file, 13, CM_MAX_OBJECT_MODIFICATION_COUNT,
                                &unit->numbeOfModifications, context))
            goto malformed;
        if (unit->numbeOfModifications > CM_MAX_OBJECT_MODIFICATION_COUNT - total_modifications)
            goto malformed;
        total_modifications += unit->numbeOfModifications;
        if (unit->numbeOfModifications) {
            unit->modifications = MemAlloc(unit->numbeOfModifications * sizeof(*unit->modifications));
            if (!unit->modifications) {
                unit->numbeOfModifications = 0;
                goto malformed;
            }
            memset(unit->modifications, 0, unit->numbeOfModifications * sizeof(*unit->modifications));
        }
        FOR_LOOP(j, unit->numbeOfModifications) {
            snprintf(context, sizeof(context), "%s object %u modification %u",
                     identity, (unsigned)i, (unsigned)j);
            if (!CM_ReadModification(file, &unit->modifications[j], context) ||
                !CM_ReadObjectBytes(file, &unknown, 4))
                goto malformed;
        }
    }
    *numUnits = count; *out = units;
    return true;
malformed:
    fprintf(stderr, "CM_ReadUnitsOverrides: malformed %s\n", context);
    CM_FreeObjectOverrides(count, units);
    return false;
}

typedef struct {
    LPCSTR identity;
    DWORD *num_original, *num_custom;
    unitData_t **original, **custom;
} mapOverrides_t;

static void CM_ReadOverrides(HANDLE archive, const mapOverrides_t *overrides) {
    unitData_t *original = NULL, *custom = NULL;
    DWORD num_original = 0, num_custom = 0;
    DWORD version;
    HANDLE file;
    if (!SFileOpenFileEx(archive, overrides->identity, SFILE_OPEN_FROM_MPQ, &file)) {
        *overrides->num_original = *overrides->num_custom = 0;
        *overrides->original = *overrides->custom = NULL;
        return;
    }
    if (!CM_ReadObjectBytes(file, &version, 4) ||
        !CM_ReadUnitsOverrides(file, overrides->identity, &num_original, &original) ||
        !CM_ReadUnitsOverrides(file, overrides->identity, &num_custom, &custom)) {
        fprintf(stderr, "CM_ReadOverrides: malformed %s\n", overrides->identity);
        CM_FreeObjectOverrides(num_original, original);
        CM_FreeObjectOverrides(num_custom, custom);
        num_original = num_custom = 0; original = custom = NULL;
    }
    *overrides->num_original = num_original; *overrides->num_custom = num_custom;
    *overrides->original = original; *overrides->custom = custom;
    SFileCloseFile(file);
}

void CM_ReadUnits(HANDLE archive) {
    mapOverrides_t units = {
        .identity = "war3map.w3u",
        .num_original = &world.info.num_originalUnits,
        .num_custom = &world.info.num_userCreatedUnits,
        .original = &world.info.originalUnits,
        .custom = &world.info.userCreatedUnits,
    };
    mapOverrides_t destructables = {
        .identity = "war3map.w3b",
        .num_original = &world.info.num_originalDestructables,
        .num_custom = &world.info.num_userCreatedDestructables,
        .original = &world.info.originalDestructables,
        .custom = &world.info.userCreatedDestructables,
    };
    CM_ReadOverrides(archive, &units);
    CM_ReadOverrides(archive, &destructables);
}

LPSTR FS_ReadArchiveFileIntoString(HANDLE archive, LPCSTR filename) {
    HANDLE file;
    if (!SFileOpenFileEx(archive, filename, SFILE_OPEN_FROM_MPQ, &file)) {
        return NULL;
    }
    DWORD fileSize = SFileGetFileSize(file, NULL);
    LPSTR buffer = MemAlloc(fileSize + 1);
    SFileReadFile(file, buffer, fileSize, NULL, NULL);
    SFileCloseFile(file);
    buffer[fileSize] = '\0';
    return buffer;
}

void removeTrailingWhitespace(char *s) {
    size_t len;

    if (!s) {
        return;
    }
    len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
}

static LPCSTR CM_SkipSpace(LPCSTR text) {
    while (text && (*text == ' ' || *text == '\t' || *text == '\r')) {
        text++;
    }
    return text;
}

static void CM_ReadLine(LPCSTR *cursor, LPSTR out, DWORD out_size) {
    DWORD len = 0;
    LPCSTR p;

    if (!cursor || !*cursor || !out || out_size == 0) {
        return;
    }
    p = *cursor;
    while (*p && *p != '\n' && *p != '\r') {
        if (len + 1 < out_size) {
            out[len++] = *p;
        }
        p++;
    }
    out[len] = '\0';
    while (*p == '\n' || *p == '\r') {
        p++;
    }
    *cursor = p;
}

static void CM_AppendTrigStringText(mapTrigStr_t *entry, LPCSTR line) {
    DWORD len;
    DWORD add;

    if (!entry || !line) {
        return;
    }
    len = (DWORD)strlen(entry->text);
    add = (DWORD)strlen(line);
    if (len + add >= sizeof(entry->text)) {
        add = sizeof(entry->text) - len - 1;
    }
    if (add) {
        memcpy(entry->text + len, line, add);
        entry->text[len + add] = '\0';
    }
}

static void CM_ReadStringsInto(HANDLE archive, LPMAPINFO info) {
    LPSTR buffer = FS_ReadArchiveFileIntoString(archive, "war3map.wts");
    LPCSTR cursor;
    mapTrigStr_t *entry = NULL;
    BOOL reading_data = false;
    char line[MAX_TRIGSTR_LENGTH];

    if (!info) {
        SAFE_DELETE(buffer, MemFree);
        return;
    }
    if (!buffer) {
        return;
    }
    PF_TextRemoveBom(buffer);
    cursor = buffer;
    while (*cursor) {
        LPCSTR trimmed;

        CM_ReadLine(&cursor, line, sizeof(line));
        trimmed = CM_SkipSpace(line);
        if (!reading_data) {
            if (!strncmp(trimmed, "STRING ", strlen("STRING "))) {
                SAFE_DELETE(entry, MemFree);
                entry = MemAlloc(sizeof(*entry));
                memset(entry, 0, sizeof(*entry));
                sscanf(trimmed, "STRING %d", &entry->id);
            } else if (entry && *trimmed == '{') {
                reading_data = true;
            }
            continue;
        }
        if (*trimmed == '}') {
            ADD_TO_LIST(entry, info->strings);
            entry = NULL;
            reading_data = false;
            continue;
        }
        removeTrailingWhitespace(line);
        CM_AppendTrigStringText(entry, line);
    }
    if (entry) {
        if (reading_data) {
            ADD_TO_LIST(entry, info->strings);
        } else {
            MemFree(entry);
        }
    }
    MemFree(buffer);
}

void CM_ReadStrings(HANDLE archive) {
    CM_ReadStringsInto(archive, &world.info);
}

void CM_ReadMapScript(HANDLE archive) {
    world.info.mapscript = FS_ReadArchiveFileIntoString(archive, "war3map.j");
//    SFileExtractFile(archive, "war3map.j", "/Users/igor/Desktop/Human02.j", 0);
}

bool CM_LoadMap(LPCSTR mapFilename) {
    bool loaded;

    snprintf(cm_loaded_map, sizeof(cm_loaded_map), "%s", mapFilename ? mapFilename : "");
    loaded = CM_LoadMapFormat(mapFilename);
    if (!loaded) {
        cm_loaded_map[0] = '\0';
    }
    return loaded;
}

BOOL CM_IsMapLoaded(LPCSTR mapFilename) {
#ifdef WOW
    return cm_loaded_map[0] && mapFilename && !strcmp(cm_loaded_map, mapFilename);
#else
    return world.map && mapFilename && !strcmp(cm_loaded_map, mapFilename);
#endif
}

LPDOODAD CM_GetDoodads(void) {
    return world.doodads;
}

DWORD CM_GetLocalPlayerNumber(void) {
    FOR_LOOP(i, MAX_PLAYERS) {
        LPCMAPPLAYER player = world.info.players + i;
        if (player->playerType == kPlayerTypeHuman)
            return i;
    }
    return 0;
}

LPCMAPINFO CM_GetMapInfo(void) {
    return &world.info;
}

void CM_ReleaseModel(void) {
    MapInfo_Release(&world.info);
}

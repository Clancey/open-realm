#include "g_local.h"
#include "g_metadata.h"
#include "g_metadata_schema.h"
#include <pthread.h>

typedef struct sheet_tail_cache_entry_s {
    sheetRow_t *rows;
    sheetRow_t *tail;
    struct sheet_tail_cache_entry_s *next;
} sheet_tail_cache_entry_t;

static sheet_tail_cache_entry_t *sheet_tail_cache = NULL;

sheetRow_t *G_SheetTail(sheetRow_t *rows)
{
    sheet_tail_cache_entry_t *entry;
    sheet_tail_cache_entry_t *new_entry;
    sheetRow_t *tail;

    if (!rows) {
        return NULL;
    }

    for (entry = sheet_tail_cache; entry; entry = entry->next) {
        if (entry->rows == rows) {
            return entry->tail;
        }
    }

    tail = rows;
    while (tail->next) {
        tail = tail->next;
    }

    new_entry = (sheet_tail_cache_entry_t *)malloc(sizeof(*new_entry));
    if (!new_entry) {
        return tail;
    }
    new_entry->rows = rows;
    new_entry->tail = tail;
    new_entry->next = sheet_tail_cache;
    sheet_tail_cache = new_entry;
    return tail;
}

LPCSTR config_files[] = {
    "Units\\OrcAbilityStrings.txt",
    "Units\\HumanUnitFunc.txt",
    "Units\\OrcUpgradeFunc.txt",
    "Units\\CommandFunc.txt",
    "Units\\UndeadUpgradeFunc.txt",
    "Units\\CommonAbilityStrings.txt",
    "Units\\CommandStrings.txt",
    "Units\\UndeadUnitFunc.txt",
    "Units\\OrcUpgradeStrings.txt",
    "Units\\CommonAbilityFunc.txt",
    "Units\\CampaignUnitStrings.txt",
    "Units\\HumanAbilityFunc.txt",
    "Units\\ItemFunc.txt",
    "Units\\NeutralAbilityFunc.txt",
    "Units\\Telemetry.txt",
    "Units\\ItemStrings.txt",
    "Units\\NightElfUnitStrings.txt",
    "Units\\UnitGlobalStrings.txt",
    "Units\\UndeadAbilityFunc.txt",
    "Units\\ItemAbilityFunc.txt",
    "Units\\HumanUpgradeFunc.txt",
    "Units\\CampaignUnitFunc.txt",
    "Units\\NeutralUnitStrings.txt",
    "Units\\NeutralAbilityStrings.txt",
    "Units\\UndeadAbilityStrings.txt",
    "Units\\OrcUnitStrings.txt",
    "Units\\NightElfUpgradeStrings.txt",
    "Units\\OrcUnitFunc.txt",
    "Units\\NightElfUnitFunc.txt",
    "Units\\HumanUpgradeStrings.txt",
    "Units\\ItemAbilityStrings.txt",
    "Units\\HumanUnitStrings.txt",
    "Units\\NightElfUpgradeFunc.txt",
    "Units\\NeutralUnitFunc.txt",
    "Units\\HumanAbilityStrings.txt",
    "Units\\OrcAbilityFunc.txt",
    "Units\\NightElfAbilityStrings.txt",
    "Units\\UndeadUnitStrings.txt",
    "Units\\NightElfAbilityFunc.txt",
    "Units\\MiscData.txt",
    "Units\\UndeadUpgradeStrings.txt",
    NULL
};

LPCSTR profile_files[] = {
    "Units\\CampaignUnitFunc.txt",
    "Units\\CampaignUnitStrings.txt",
    "Units\\HumanUnitFunc.txt",
    "Units\\HumanUnitStrings.txt",
    "Units\\NeutralUnitFunc.txt",
    "Units\\NeutralUnitStrings.txt",
    "Units\\NightElfUnitFunc.txt",
    "Units\\NightElfUnitStrings.txt",
    "Units\\OrcUnitFunc.txt",
    "Units\\OrcUnitStrings.txt",
    "Units\\UndeadUnitFunc.txt",
    "Units\\UndeadUnitStrings.txt",
    // optionals
    "Units\\UnitSkin.txt",
    "Units\\UnitWeaponsFunc.txt",
    "Units\\UnitWeaponsSkin.txt",
    NULL
};

static sheetRow_t *abilityConfigs = NULL;
static sheetRow_t *abilityConfigsTail = NULL;
static sheetRow_t *commandFuncConfig = NULL;
static sheetRow_t *commandStringsConfig = NULL;
static sheetRow_t *abilityConfigTables[64];
static DWORD abilityConfigTableCount = 0;

sheetRow_t *Doodads = NULL;

static void AppendSheetRows(sheetRow_t **head, sheetRow_t **tail, sheetRow_t *rows)
{
    sheetRow_t *rows_tail;

    if (!rows) {
        return;
    }

    rows_tail = G_SheetTail(rows);

    if (*tail) {
        (*tail)->next = rows;
    } else {
        *head = rows;
    }
    *tail = rows_tail;
}

void G_SetConfigTable(sheetMetaData_t *metadatas, LPCSTR slk, sheetRow_t *table) {
    for (sheetMetaData_t *d = metadatas; d->id; d++) {
        if (!strcmp(d->slk, slk)) {
            d->table = table;
        }
    }
}

sheetMetaData_t *G_FindMetaData(sheetMetaData_t *metadatas, LPCSTR name) {
    for (sheetMetaData_t *d = metadatas; d->id; d++) {
        if (!strcmp(d->id, name)) {
            return d;
        }
    }
    return NULL;
}

/* A field code that no metadata entry maps is a programming error, not missing
 * unit data: the accessor silently resolves to NULL/0. That is how stat bugs
 * hid for so long ("umpc" mana, "udfc" armor, "uinc"/"ustc"/"uagc" attributes).
 * Warn once per code so any such gap surfaces the first time it is read. */
static void warn_unregistered_field(LPCSTR name) {
    static char seen[64][8];
    static DWORD count;

    for (DWORD i = 0; i < count; i++) {
        if (!strcmp(seen[i], name)) {
            return;
        }
    }
    if (count < 64) {
        strncpy(seen[count], name, sizeof(seen[0]) - 1);
        count++;
    }
    fprintf(stderr,
            "WARNING: unit-data field code '%s' is not registered in the metadata "
            "table; it silently reads as 0. Add it to UnitsMetaData[] (g_metadata.h).\n",
            name);
}

LPCSTR UnitStringFieldBase(sheetMetaData_t *metadatas, DWORD unit_id, LPCSTR name) {
    sheetMetaData_t *metadata = G_FindMetaData(metadatas, name);
    if (!metadata) {
        warn_unregistered_field(name);
        return NULL;
    }
    if (metadata->table) {
        return FS_FindSheetCell(metadata->table, GetClassName(unit_id), metadata->field);
    }
    return NULL;
}

LPCSTR UnitStringField(sheetMetaData_t *metadatas, DWORD unit_id, LPCSTR name) {
    const metadataMapSnapshot_t *snapshot = G_MetadataMapAcquire();
    unit_id = G_MetadataMapClass(snapshot, unit_id);
    G_MetadataMapRelease(snapshot);
    return UnitStringFieldBase(metadatas, unit_id, name);
}

LONG UnitIntegerFieldBase(sheetMetaData_t *metadatas, DWORD unit_id, LPCSTR name) {
    LPCSTR str = UnitStringFieldBase(metadatas, unit_id, name);
    return str ? atoi(str) : 0;
}

LONG UnitIntegerField(sheetMetaData_t *metadatas, DWORD unit_id, LPCSTR name) {
    LPCSTR str = UnitStringField(metadatas, unit_id, name);
    return str ? atoi(str) : 0;
}

BOOL UnitBooleanFieldBase(sheetMetaData_t *metadatas, DWORD unit_id, LPCSTR name) {
    LPCSTR str = UnitStringFieldBase(metadatas, unit_id, name);
    return str && (atoi(str) != 0 || !strcmp(str, "TRUE"));
}

BOOL UnitBooleanField(sheetMetaData_t *metadatas, DWORD unit_id, LPCSTR name) {
    LPCSTR str = UnitStringField(metadatas, unit_id, name);
    return str && (atoi(str) != 0 || !strcmp(str, "TRUE"));
}

FLOAT UnitRealFieldBase(sheetMetaData_t *metadatas, DWORD unit_id, LPCSTR name) {
    LPCSTR str = UnitStringFieldBase(metadatas, unit_id, name);
    return str ? atof(str) : 0;
}

FLOAT UnitRealField(sheetMetaData_t *metadatas, DWORD unit_id, LPCSTR name) {
    LPCSTR str = UnitStringField(metadatas, unit_id, name);
    return str ? atof(str) : 0;
}


void InitUnitData(void) {
    sheetRow_t *Profile = NULL;
    sheetRow_t *profileTail = NULL;

    abilityConfigs = NULL;
    abilityConfigsTail = NULL;
    commandFuncConfig = NULL;
    commandStringsConfig = NULL;
    abilityConfigTableCount = 0;
    Doodads = NULL;
    
    for (LPCSTR *config = config_files; *config; config++) {
        sheetRow_t *current = FS_ParseINI(*config);
        if (current) {
            if (abilityConfigTableCount < sizeof(abilityConfigTables) / sizeof(*abilityConfigTables)) {
                abilityConfigTables[abilityConfigTableCount++] = current;
            }
            AppendSheetRows(&abilityConfigs, &abilityConfigsTail, current);
            if (!strcmp(*config, "Units\\CommandFunc.txt")) {
                commandFuncConfig = current;
            } else if (!strcmp(*config, "Units\\CommandStrings.txt")) {
                commandStringsConfig = current;
            }
        }
    }
    for (LPCSTR *config = profile_files; *config; config++) {
        sheetRow_t *current = FS_ParseINI(*config);
        if (current) {
            AppendSheetRows(&Profile, &profileTail, current);
        }
    }
    sheetRow_t *DestructableData = FS_ParseSLK("Units\\DestructableData.slk");
    Doodads = FS_ParseSLK("Doodads\\Doodads.slk");
    G_ApplyMetaDataSchema(UnitsMetaData, FS_ParseSLK("Units\\UnitMetaData.slk"));
    G_SetConfigTable(UnitsMetaData, "Profile", Profile);
    G_SetConfigTable(UnitsMetaData, "UnitAbilities", FS_ParseSLK("Units\\UnitAbilities.slk"));
    G_SetConfigTable(UnitsMetaData, "UnitBalance", FS_ParseSLK("Units\\UnitBalance.slk"));
    G_SetConfigTable(UnitsMetaData, "UnitData", FS_ParseSLK("Units\\UnitData.slk"));
    G_SetConfigTable(UnitsMetaData, "UnitUI", FS_ParseSLK("Units\\UnitUI.slk"));
    G_SetConfigTable(UnitsMetaData, "UnitWeapons", FS_ParseSLK("Units\\UnitWeapons.slk"));
    
    G_SetConfigTable(DestructableMetaData, "DestructableData", DestructableData);
    
    G_SetConfigTable(ItemsMetaData, "ItemData", FS_ParseSLK("Units\\ItemData.slk"));
}

void ShutdownUnitData(void) {
}

static LPCSTR FindConfigField(sheetRow_t *sheet, LPCSTR row, LPCSTR column) {
    FOR_EACH_LIST(sheetRow_t const, srow, sheet) {
        if (strcmp(srow->name, row)) {
            continue;
        }
        FOR_EACH_LIST(sheetField_t const, scolumn, srow->fields) {
            if (!strcasecmp(scolumn->name, column)) {
                return scolumn->value;
            }
        }
    }
    return NULL;
}

LPCSTR FindConfigValue(LPCSTR category, LPCSTR field) {
    LPCSTR value;

    if (!strncmp(category, "Cmd", 3)) {
        if (commandFuncConfig) {
            value = FindConfigField(commandFuncConfig, category, field);
            if (value) {
                return value;
            }
        }
        if (commandStringsConfig) {
            value = FindConfigField(commandStringsConfig, category, field);
            if (value) {
                return value;
            }
        }
    }

    FOR_LOOP(i, abilityConfigTableCount) {
        value = FindConfigField(abilityConfigTables[i], category, field);
        if (value) {
            return value;
        }
    }
    return NULL;
}

LPCSTR GetClassName(DWORD class_id) {
    /* Asset metadata may resolve on concurrent Swift readers; keep the legacy
     * transient buffer contract while preventing cross-thread row corruption. */
    static _Thread_local char classname[5] = { 0 };
    memcpy(classname, &class_id, 4);
    return classname;
}
typedef struct {
    DWORD original_id, class_id;
} metadataClassAlias_t;

struct metadataMapSnapshot_s {
    int refs;
    uint64_t token;
    DWORD count;
    metadataClassAlias_t aliases[];
};

static pthread_mutex_t metadata_map_lock = PTHREAD_MUTEX_INITIALIZER;
static metadataMapSnapshot_t *metadata_map;
static uint64_t metadata_map_token;

/* Publish only class aliases so worker metadata reads never touch mutable world.info storage. */
void G_MetadataPublishMap(LPCMAPINFO mapinfo) {
    metadataMapSnapshot_t *snapshot = NULL, *old;
    DWORD count = mapinfo ? mapinfo->num_userCreatedUnits : 0;
    size_t alias_bytes, allocation;
    if (mapinfo && (!count || mapinfo->userCreatedUnits) &&
        !__builtin_mul_overflow((size_t)count, sizeof(*snapshot->aliases), &alias_bytes) &&
        !__builtin_add_overflow(sizeof(*snapshot), alias_bytes, &allocation)) {
        snapshot = calloc(1, allocation);
        if (snapshot) {
            snapshot->refs = 1;
            snapshot->count = count;
            for (DWORD i = 0; i < count; i++) {
                snapshot->aliases[i].original_id = mapinfo->userCreatedUnits[i].originalUnitID;
                snapshot->aliases[i].class_id = mapinfo->userCreatedUnits[i].newUnitID;
            }
        }
    }
    pthread_mutex_lock(&metadata_map_lock);
    if (snapshot) {
        if (!++metadata_map_token) metadata_map_token = 1;
        snapshot->token = metadata_map_token;
    }
    old = metadata_map;
    metadata_map = snapshot;
    if (old && !--old->refs) free(old);
    pthread_mutex_unlock(&metadata_map_lock);
    if (mapinfo && !snapshot)
        fprintf(stderr, "G_Metadata: unable to publish map class aliases\n");
}

const metadataMapSnapshot_t *G_MetadataMapAcquire(void) {
    pthread_mutex_lock(&metadata_map_lock);
    metadataMapSnapshot_t *snapshot = metadata_map;
    if (snapshot) snapshot->refs++;
    pthread_mutex_unlock(&metadata_map_lock);
    return snapshot;
}

void G_MetadataMapRelease(const metadataMapSnapshot_t *snapshot_const) {
    metadataMapSnapshot_t *snapshot = (metadataMapSnapshot_t *)snapshot_const;
    if (!snapshot) return;
    pthread_mutex_lock(&metadata_map_lock);
    if (!--snapshot->refs) free(snapshot);
    pthread_mutex_unlock(&metadata_map_lock);
}

uint64_t G_MetadataMapToken(const metadataMapSnapshot_t *snapshot) {
    return snapshot ? snapshot->token : 0;
}

DWORD G_MetadataMapClass(const metadataMapSnapshot_t *snapshot, DWORD class_id) {
    if (!snapshot) return class_id;
    for (DWORD i = 0; i < snapshot->count; i++)
        if (snapshot->aliases[i].class_id == class_id)
            return snapshot->aliases[i].original_id;
    return class_id;
}

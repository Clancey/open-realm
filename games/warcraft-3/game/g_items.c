#include "g_local.h"

static FLOAT G_MiscVectorValue(LPCSTR name, DWORD index) {
    LPCSTR value = FS_FindSheetCell(game.config.misc, "Misc", name);
    if (!value) {
        return 0;
    }

    for (DWORD i = 0; i < index; i++) {
        value = strchr(value, ',');
        if (!value) {
            return 0;
        }
        value++;
    }

    return atof(value);
}

/* Item scale must be a complete finite positive authored value before replacing map/JASS state. */
BOOL G_ParseItemScale(LPCSTR text, LPFLOAT value) {
    LPSTR end;
    FLOAT parsed;
    if (!text || !text[0]) return false;
    parsed = strtof(text, &end);
    if (end == text || *end || !isfinite(parsed) || parsed <= 0) return false;
    *value = parsed;
    return true;
}

void SP_SpawnItem(LPEDICT self) {
    PATHSTR model_filename;
    LPCSTR scale = UnitStringField(ItemsMetaData, self->class_id, "isca");
    FLOAT authored_scale;
    strlcpy(model_filename, ITEM_FILE(self->class_id), sizeof(model_filename));
    self->s.model = G_RegisterModel(model_filename);
    self->s.shadow = G_LoadShadowTexture(FS_FindSheetCell(game.config.misc, "Misc", "ItemShadowFile"), false);
    self->s.shadow_rect = ShadowPackRect(
        G_MiscVectorValue("ItemShadowOffset", 0),
        G_MiscVectorValue("ItemShadowOffset", 1),
        G_MiscVectorValue("ItemShadowSize", 0),
        G_MiscVectorValue("ItemShadowSize", 1));
    self->movetype = MOVETYPE_NONE;
    /* TFT authors item model scale; malformed values and ROC's absent column preserve placed/JASS scale. */
    if (G_ParseItemScale(scale, &authored_scale)) self->s.scale = authored_scale;
    else if (scale)
        fprintf(stderr, "SP_SpawnItem: class %.4s has invalid authored scale '%s'\n",
                (const char *)&self->class_id, scale);
}

#ifndef G_METADATA_SCHEMA_H
#define G_METADATA_SCHEMA_H

/* Expansion archives replace UnitMetaData.slk and may move fields between backing sheets. */
static void G_ApplyMetaDataSchema(sheetMetaData_t *metadatas, const sheetRow_t *schema) {
    if (!schema) {
        fprintf(stderr, "G_Metadata: Units\\UnitMetaData.slk unavailable\n");
        for (sheetMetaData_t *meta = metadatas; meta->id; meta++)
            meta->slk = "", meta->table = NULL;
        return;
    }
    for (sheetMetaData_t *meta = metadatas; meta->id; meta++) {
        const sheetRow_t *row;
        LPCSTR field = NULL, slk = NULL;
        meta->table = NULL;
        for (row = schema; row && strcmp(row->name, meta->id); row = row->next);
        if (!row)
            continue;
        for (const sheetField_t *cell = row->fields; cell; cell = cell->next) {
            if (!strcasecmp(cell->name, "field")) field = cell->value;
            else if (!strcasecmp(cell->name, "slk")) slk = cell->value;
        }
        if (!field || !field[0] || !slk || !slk[0]) {
            fprintf(stderr, "G_Metadata: UnitMetaData row %s has no field/slk mapping\n", meta->id);
            meta->slk = "";
            continue;
        }
        meta->slk = slk;
    }
}

#endif

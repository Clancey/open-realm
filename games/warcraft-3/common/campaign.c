#include "campaign.h"

#include <ctype.h>
#include <stddef.h>
#include <stdlib.h>
#ifndef _WIN32
#include <strings.h>
#endif

typedef enum {
    WC3_CAMPAIGN_FIELD_HEADER,
    WC3_CAMPAIGN_FIELD_NAME,
    WC3_CAMPAIGN_FIELD_BACKGROUND,
    WC3_CAMPAIGN_FIELD_CURSOR,
} wc3CampaignFieldType_t;

typedef struct {
    LPCSTR name;
    size_t offset;
    DWORD size;
    wc3CampaignFieldType_t type;
} wc3CampaignField_t;

static wc3CampaignField_t const campaign_fields[] = {
    { "Header", offsetof(WC3CAMPAIGN, header), sizeof(UINAME), WC3_CAMPAIGN_FIELD_HEADER },
    { "Name", offsetof(WC3CAMPAIGN, name), sizeof(UINAME), WC3_CAMPAIGN_FIELD_NAME },
    { "Background", offsetof(WC3CAMPAIGN, background), sizeof(UINAME), WC3_CAMPAIGN_FIELD_BACKGROUND },
    { "Cursor", offsetof(WC3CAMPAIGN, race), sizeof(playerRace_t), WC3_CAMPAIGN_FIELD_CURSOR },
};

static char *WC3_CampaignTrim(char *text) {
    text += strspn(text, " \t\r\n");
    for (char *end = text + strlen(text); end > text && isspace((unsigned char)end[-1]); )
        *--end = '\0';
    return text;
}

static void WC3_CampaignStripComment(char *line) {
    BOOL quoted = false;
    for (char *p = line; *p; p++) {
        if (*p == '"') quoted = !quoted;
        if (!quoted && p[0] == '/' && p[1] == '/') {
            *p = '\0';
            return;
        }
    }
}

static BOOL WC3_CampaignReadQuoted(char **cursor, LPSTR out, DWORD out_size) {
    char *p = *cursor + strspn(*cursor, " \t\r\n");
    if (*p != '"')
        return false;
    char *end = strchr(++p, '"');
    if (!end)
        return false;
    size_t len = MIN((size_t)(end - p), out_size ? (size_t)out_size - 1 : 0);
    if (out_size)
        memcpy(out, p, len), out[len] = '\0';
    p = end + 1 + strspn(end + 1, " \t\r\n");
    if (*p == ',')
        p++;
    *cursor = p;
    return true;
}

static LPWC3CAMPAIGN WC3_CampaignFind(LPWC3CAMPAIGNCATALOG catalog, LPCSTR key) {
    if (!catalog || !key || !*key)
        return NULL;
    FOR_LOOP(i, catalog->campaign_count)
        if (!strcasecmp(catalog->campaigns[i].key, key))
            return &catalog->campaigns[i];
    return NULL;
}

static LPWC3CAMPAIGN WC3_CampaignEnsure(LPWC3CAMPAIGNCATALOG catalog, LPCSTR key) {
    LPWC3CAMPAIGN campaign = WC3_CampaignFind(catalog, key);
    if (campaign)
        return campaign;
    if (!catalog || !key || !*key || catalog->campaign_count >= WC3_MAX_CAMPAIGNS)
        return NULL;
    campaign = &catalog->campaigns[catalog->campaign_count++];
    memset(campaign, 0, sizeof(*campaign));
    snprintf(campaign->key, sizeof(campaign->key), "%s", key);
    campaign->race = kPlayerRaceNone;
    return campaign;
}

static void WC3_CampaignAddOrder(LPWC3CAMPAIGNCATALOG catalog, LPCSTR key) {
    LPWC3CAMPAIGN campaign = WC3_CampaignEnsure(catalog, key);
    if (!campaign || catalog->campaign_order_count >= WC3_MAX_CAMPAIGNS)
        return;
    FOR_LOOP(i, catalog->campaign_order_count)
        if (catalog->campaign_order[i] == (DWORD)(campaign - catalog->campaigns))
            return;
    catalog->campaign_order[catalog->campaign_order_count++] = (DWORD)(campaign - catalog->campaigns);
}

static void WC3_CampaignParseOrder(LPWC3CAMPAIGNCATALOG catalog, char *value) {
    UINAME key;
    char *cursor = value;
    while (WC3_CampaignReadQuoted(&cursor, key, sizeof(key)))
        if (key[0])
            WC3_CampaignAddOrder(catalog, key);
}

static BOOL WC3_CampaignIndexedKey(LPCSTR key, LPCSTR prefix, DWORD *index) {
    size_t prefix_len = strlen(prefix);
    char *end;
    unsigned long value;
    if (strncasecmp(key, prefix, prefix_len))
        return false;
    value = strtoul(key + prefix_len, &end, 10);
    if (*end || value >= WC3_MAX_CAMPAIGN_MISSIONS)
        return false;
    *index = (DWORD)value;
    return true;
}

static void WC3_CampaignSetMissionCount(LPWC3CAMPAIGN campaign, DWORD index) {
    if (campaign && campaign->num_missions <= index)
        campaign->num_missions = index + 1;
}

static void WC3_CampaignParseMission(LPWC3CAMPAIGN campaign, DWORD index, char *value) {
    char *cursor = value;
    UINAME header, name;
    PATHSTR path;
    LPWC3CAMPAIGNMISSION mission;
    if (!campaign || index >= WC3_MAX_CAMPAIGN_MISSIONS)
        return;
    mission = &campaign->missions[index];
    if (WC3_CampaignReadQuoted(&cursor, header, sizeof(header)) &&
        WC3_CampaignReadQuoted(&cursor, name, sizeof(name)) &&
        WC3_CampaignReadQuoted(&cursor, path, sizeof(path))) {
        snprintf(mission->header, sizeof(mission->header), "%s", header);
        snprintf(mission->name, sizeof(mission->name), "%s", name);
        snprintf(mission->map_path, sizeof(mission->map_path), "%s", path);
    } else {
        cursor = value;
        if (WC3_CampaignReadQuoted(&cursor, name, sizeof(name)))
            snprintf(mission->name, sizeof(mission->name), "%s", name);
    }
    WC3_CampaignSetMissionCount(campaign, index);
}

static void WC3_CampaignParseFile(LPWC3CAMPAIGN campaign, DWORD index, char *value) {
    PATHSTR file;
    char *cursor = value;
    LPWC3CAMPAIGNMISSION mission;
    if (!campaign || index >= WC3_MAX_CAMPAIGN_MISSIONS ||
        !WC3_CampaignReadQuoted(&cursor, file, sizeof(file)) || !file[0])
        return;
    mission = &campaign->missions[index];
    if (strchr(file, '\\') || strchr(file, '/'))
        snprintf(mission->map_path, sizeof(mission->map_path), "%s", file);
    else
        snprintf(mission->map_path, sizeof(mission->map_path), "Maps\\Campaign\\%.*s.w3m",
                 (int)(sizeof(mission->map_path) - 19), file);
    WC3_CampaignSetMissionCount(campaign, index);
}

static void WC3_CampaignParseField(LPWC3CAMPAIGN campaign, char *key, char *value) {
    DWORD index;
    if (!campaign)
        return;
    for (DWORD i = 0; i < sizeof(campaign_fields) / sizeof(campaign_fields[0]); i++) {
        wc3CampaignField_t const *field = &campaign_fields[i];
        if (strcasecmp(key, field->name))
            continue;
        if (field->type == WC3_CAMPAIGN_FIELD_CURSOR)
            campaign->race = (playerRace_t)atoi(value);
        else {
            char *cursor = value;
            WC3_CampaignReadQuoted(&cursor, (LPSTR)campaign + field->offset, field->size);
        }
        return;
    }
    if (WC3_CampaignIndexedKey(key, "Mission", &index))
        WC3_CampaignParseMission(campaign, index, value);
    else if (WC3_CampaignIndexedKey(key, "File", &index))
        WC3_CampaignParseFile(campaign, index, value);
}

void WC3_CampaignCatalogReset(LPWC3CAMPAIGNCATALOG catalog) {
    if (catalog)
        memset(catalog, 0, sizeof(*catalog));
}

/* Parse the archive-authored keyed campaign format into bounded copied values. */
BOOL WC3_CampaignCatalogParse(LPWC3CAMPAIGNCATALOG catalog, LPSTR text) {
    UINAME section = "";
    LPWC3CAMPAIGN campaign = NULL;
    char *cursor = text;
    if (!catalog || !text)
        return false;
    while (*cursor) {
        char *line = cursor;
        while (*cursor && *cursor != '\n' && *cursor != '\r')
            cursor++;
        if (*cursor) {
            *cursor++ = '\0';
            while (*cursor == '\n' || *cursor == '\r')
                cursor++;
        }
        if ((unsigned char)line[0] == 0xef && (unsigned char)line[1] == 0xbb &&
            (unsigned char)line[2] == 0xbf)
            line += 3;
        WC3_CampaignStripComment(line);
        char *key = WC3_CampaignTrim(line);
        if (!*key)
            continue;
        if (*key == '[') {
            char *end = strchr(key + 1, ']');
            if (!end)
                continue;
            *end = '\0';
            snprintf(section, sizeof(section), "%s", WC3_CampaignTrim(key + 1));
            campaign = strcasecmp(section, "Index") ? WC3_CampaignEnsure(catalog, section) : NULL;
            continue;
        }
        char *eq = strchr(key, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *value = WC3_CampaignTrim(eq + 1);
        key = WC3_CampaignTrim(key);
        if (!strcasecmp(section, "Index") && !strcasecmp(key, "CampaignList"))
            WC3_CampaignParseOrder(catalog, value);
        else
            WC3_CampaignParseField(campaign, key, value);
    }
    return catalog->campaign_count > 0;
}

void WC3_CampaignCatalogFinalize(LPWC3CAMPAIGNCATALOG catalog) {
    if (!catalog || catalog->campaign_order_count)
        return;
    FOR_LOOP(i, catalog->campaign_count)
        if (catalog->campaigns[i].num_missions &&
            catalog->campaign_order_count < WC3_MAX_CAMPAIGNS)
            catalog->campaign_order[catalog->campaign_order_count++] = i;
}

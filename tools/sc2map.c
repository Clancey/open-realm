#include "common/mpq.h"
#include "games/starcraft-2/common/sc2_dds.h"
#include "games/starcraft-2/common/sc2_map.h"
#include "games/starcraft-2/visionos/sc2_tabletop_assets_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SC2MAP_MAX_ARCHIVES 16
#define SC2MAP_DATA_ARCHIVES 11

static HANDLE sc2map_archives[SC2MAP_MAX_ARCHIVES];
static LPCSTR sc2map_data_dir = "";
static LPCSTR const sc2map_data_archives[SC2MAP_DATA_ARCHIVES] = {
    "Battle.net/Battle.net.MPQ",
    "Campaigns/Liberty.SC2Campaign/Base.SC2Data",
    "Campaigns/Liberty.SC2Campaign/Base.SC2Maps",
    "Campaigns/Liberty.SC2Campaign/base.SC2Assets",
    "Campaigns/LibertyStory.SC2Campaign/Base.SC2Data",
    "Mods/Challenges.SC2Mod",
    "Mods/Core.SC2Mod/Base.SC2Data",
    "Mods/Core.SC2Mod/base.SC2Assets",
    "Mods/Liberty.SC2Mod/Base.SC2Data",
    "Mods/Liberty.SC2Mod/base.SC2Assets",
    "Mods/LibertyMulti.SC2Mod/Base.SC2Data",
};

static void usage(void) {
    fprintf(stderr,
            "Usage:\n"
            "  sc2map [-data <StarCraft2-data-dir>] [-mpq <archive>]... [--asset-inventory] <map.SC2Map|map-dir>\n"
            "  sc2map [-data <StarCraft2-data-dir>] [-mpq <archive>]... [--asset-inventory] -map <map.SC2Map|map-dir>\n"
            "\n"
            "Examples:\n"
            "  sc2map -mpq build/tests/test-sc2.SC2Maps Maps/Test/Tiny.SC2Map\n"
            "  sc2map -data data/StarCraft2 Maps/Campaign/TRaynor01.SC2Map\n"
            "  sc2map games/starcraft-2/tests/resources-src/Maps/Test/Tiny.SC2Map\n");
}

static HANDLE sc2map_mem_alloc(long size) {
    void *mem = calloc(1, (size_t)(size ? size : 1));

    if (!mem) {
        fprintf(stderr, "sc2map: out of memory allocating %ld bytes\n", size);
        exit(1);
    }
    return mem;
}

static void sc2map_mem_free(HANDLE mem) {
    free(mem);
}

static HANDLE sc2map_read_disk_file(LPCSTR filename, LPDWORD size) {
    FILE *file;
    long file_size;
    LPBYTE data;
    struct stat st;

    if (size) *size = 0;
    if (!filename || !*filename)
        return NULL;
    if (stat(filename, &st) != 0 || !S_ISREG(st.st_mode))
        return NULL;
    file = fopen(filename, "rb");
    if (!file)
        return NULL;
    if (fseek(file, 0, SEEK_END) != 0 || (file_size = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    data = sc2map_mem_alloc(file_size ? file_size : 1);
    if (file_size > 0 && fread(data, 1, file_size, file) != (size_t)file_size) {
        fclose(file);
        sc2map_mem_free(data);
        return NULL;
    }
    fclose(file);
    if (size) *size = (DWORD)file_size;
    return data;
}

static HANDLE sc2map_read_archive_file(LPCSTR filename, LPDWORD size) {
    char path[MAX_PATHLEN];

    if (size) *size = 0;
    if (!filename || !*filename)
        return NULL;
    for (int i = SC2MAP_MAX_ARCHIVES - 1; i >= 0; i--) {
        HANDLE file;
        DWORD file_size;
        LPBYTE data;

        if (!sc2map_archives[i])
            continue;
        snprintf(path, sizeof(path), "%s", filename);
        for (char *p = path; *p; p++) if (*p == '/') *p = '\\';
        if (!SFileOpenFileEx(sc2map_archives[i], path, SFILE_OPEN_FROM_MPQ, &file)) {
            snprintf(path, sizeof(path), "%s", filename);
            for (char *p = path; *p; p++) if (*p == '\\') *p = '/';
            if (!SFileOpenFileEx(sc2map_archives[i], path, SFILE_OPEN_FROM_MPQ, &file))
                continue;
        }
        file_size = SFileGetFileSize(file, NULL);
        data = sc2map_mem_alloc(file_size + 1);
        if (!SFileReadFile(file, data, file_size, NULL, NULL)) {
            SFileCloseFile(file);
            sc2map_mem_free(data);
            return NULL;
        }
        SFileCloseFile(file);
        data[file_size] = 0;
        if (size) *size = file_size;
        return data;
    }
    return NULL;
}

static HANDLE sc2map_read_file(LPCSTR filename, LPDWORD size) {
    HANDLE data = sc2map_read_archive_file(filename, size);

    if (data)
        return data;
    return sc2map_read_disk_file(filename, size);
}

static BOOL sc2map_add_archive(LPCSTR filename) {
    FOR_LOOP(i, SC2MAP_MAX_ARCHIVES) {
        if (sc2map_archives[i])
            continue;
        if (!SFileOpenArchive(filename, 0, 0, &sc2map_archives[i])) {
            fprintf(stderr, "sc2map: cannot open archive %s\n", filename);
            return false;
        }
        return true;
    }
    fprintf(stderr, "sc2map: too many archives\n");
    return false;
}

static LPCSTR sc2map_basename(LPCSTR path) {
    LPCSTR base = path;
    if (!path) return "";
    for (LPCSTR p = path; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    return base;
}

static int sc2map_compare_archive_paths(void const *a, void const *b) {
    LPCSTR pa = *(LPCSTR const *)a, pb = *(LPCSTR const *)b;
    int cmp = strcasecmp(sc2map_basename(pa), sc2map_basename(pb));
    return cmp ? cmp : strcasecmp(pa, pb);
}

/* Mirror FS_AddDataDirectory's sorted archive order for the proven retail fileset. */
static BOOL sc2map_add_data_archives(LPCSTR data_dir) {
    char paths[SC2MAP_DATA_ARCHIVES][MAX_PATHLEN];
    LPCSTR sorted[SC2MAP_DATA_ARCHIVES];
    if (!data_dir || !*data_dir) return false;
    FOR_LOOP(i, SC2MAP_DATA_ARCHIVES) {
        snprintf(paths[i], sizeof(paths[i]), "%s/%s", data_dir, sc2map_data_archives[i]);
        sorted[i] = paths[i];
    }
    qsort(sorted, SC2MAP_DATA_ARCHIVES, sizeof(sorted[0]), sc2map_compare_archive_paths);
    FOR_LOOP(i, SC2MAP_DATA_ARCHIVES)
        if (!sc2map_add_archive(sorted[i]))
            return false;
    return true;
}

static LPCSTR sc2map_cvar_string(LPCSTR name, LPCSTR fallback) {
    if (name && !strcmp(name, "data"))
        return sc2map_data_dir && *sc2map_data_dir ? sc2map_data_dir : fallback;
    return fallback;
}

static void sc2map_close_archives(void) {
    FOR_LOOP(i, SC2MAP_MAX_ARCHIVES) {
        if (sc2map_archives[i]) {
            SFileCloseArchive(sc2map_archives[i]);
            sc2map_archives[i] = NULL;
        }
    }
}

static uint32_t sc2map_availability_flags(sc2Map_t const *map) {
    uint32_t flags = 0;
    if (map->terrain_layer_status[SC2_TERRAIN_LAYER_HEIGHT_MAP] == SC2_LAYER_STATUS_OK)
        flags |= BZ_SC2A_TERRAIN_HAS_HMAP;
    if (map->terrain_layer_status[SC2_TERRAIN_LAYER_SYNC_HEIGHT_MAP] == SC2_LAYER_STATUS_OK)
        flags |= BZ_SC2A_TERRAIN_HAS_SMAP;
    if (map->terrain_layer_status[SC2_TERRAIN_LAYER_CELL_FLAGS] == SC2_LAYER_STATUS_OK)
        flags |= BZ_SC2A_TERRAIN_HAS_LFCT;
    if (map->terrain_layer_status[SC2_TERRAIN_LAYER_SYNC_CLIFF_LEVEL] == SC2_LAYER_STATUS_OK)
        flags |= BZ_SC2A_TERRAIN_HAS_CLIF;
    if (map->terrain_layer_status[SC2_TERRAIN_LAYER_TEXTURE_MASKS] == SC2_LAYER_STATUS_OK)
        flags |= BZ_SC2A_TERRAIN_HAS_MASK;
    if (map->t3Terrain.fog_enabled) flags |= BZ_SC2A_TERRAIN_HAS_FOG;
    if (map->lighting.enabled) flags |= BZ_SC2A_TERRAIN_HAS_LIGHTING;
    return flags;
}

static uint32_t sc2map_malformed_flags(sc2Map_t const *map) {
    static sc2TerrainLayerId_t const layers[] = {
        SC2_TERRAIN_LAYER_HEIGHT_MAP, SC2_TERRAIN_LAYER_SYNC_HEIGHT_MAP,
        SC2_TERRAIN_LAYER_CELL_FLAGS, SC2_TERRAIN_LAYER_SYNC_CLIFF_LEVEL,
        SC2_TERRAIN_LAYER_TEXTURE_MASKS,
    };
    uint32_t flags = 0;
    FOR_LOOP(i, sizeof(layers) / sizeof(layers[0]))
        if (map->terrain_layer_status[layers[i]] == SC2_LAYER_STATUS_MALFORMED)
            flags |= 1u << i;
    return flags;
}

static uint32_t sc2map_unsupported_flags(sc2Map_t const *map) {
    uint32_t flags = BZ_SC2A_TERRAIN_UNSUPPORTED_VERTEX_COLOR | BZ_SC2A_TERRAIN_UNSUPPORTED_PAINTED_PATHING;
    if (map->terrain_layer_status[SC2_TERRAIN_LAYER_WATER] == SC2_LAYER_STATUS_UNSUPPORTED)
        flags |= BZ_SC2A_TERRAIN_UNSUPPORTED_WATER;
    if (map->terrain_layer_status[SC2_TERRAIN_LAYER_SYNC_PATHING_INFO] == SC2_LAYER_STATUS_UNSUPPORTED)
        flags |= BZ_SC2A_TERRAIN_UNSUPPORTED_PATHING;
    if (map->terrain_layer_status[SC2_TERRAIN_LAYER_FLUFF_DOODAD] == SC2_LAYER_STATUS_UNSUPPORTED)
        flags |= BZ_SC2A_TERRAIN_UNSUPPORTED_FLUFF_DOODAD;
    if (map->terrain_layer_status[SC2_TERRAIN_LAYER_HARD_TILE] == SC2_LAYER_STATUS_UNSUPPORTED)
        flags |= BZ_SC2A_TERRAIN_UNSUPPORTED_HARD_TILE;
    return flags;
}

static LPCSTR sc2map_dds_format(LPCSC2DDSIMAGE dds) {
    if (!dds) return "none";
    switch (dds->format) {
    case SC2_DDS_FORMAT_DXT1: return "DXT1";
    case SC2_DDS_FORMAT_DXT3: return "DXT3";
    case SC2_DDS_FORMAT_DXT5: return "DXT5";
    case SC2_DDS_FORMAT_RGB: return dds->channels == SC2_DDS_CHANNELS_BGR ? "BGR8" : "RGB8";
    case SC2_DDS_FORMAT_RGBA: return dds->channels == SC2_DDS_CHANNELS_BGRA ? "BGRA8" : "RGBA8";
    default: return "none";
    }
}

static int sc2map_inventory_image(FILE *out, DWORD texture, LPCSTR channel, LPCSTR identity, BOOL required,
                                  char seen[][BZ_SC2A_MAX_IDENTITY], DWORD *seen_count) {
    DWORD size = 0, duplicate = UINT32_MAX;
    LPBYTE data;
    sc2DdsImage_t dds;
    sc2DdsResult_t result;
    if (!identity || !*identity) {
        fprintf(out, "Image: texture=%u channel=%s identity=\"\" status=empty\n", texture, channel);
        return required ? 1 : 0;
    }
    FOR_LOOP(i, *seen_count)
        if (!strcmp(seen[i], identity)) { duplicate = i; break; }
    if (*seen_count < BZ_SC2A_MAX_TERRAIN_TEXTURES * 2) {
        snprintf(seen[*seen_count], BZ_SC2A_MAX_IDENTITY, "%s", identity);
        (*seen_count)++;
    }
    if (!sc2_tta_path_is_confined(identity)) {
        fprintf(out, "Image: texture=%u channel=%s identity=\"%s\" status=path-confinement\n",
                texture, channel, identity);
        return 1;
    }
    data = sc2map_read_file(identity, &size);
    if (!data) {
        /* The renderer only ever loads the diffuse texture (r_sc2_load_terrain_textures());
         * a normal map that cannot be resolved is the same non-fatal case sc2_map.c's own
         * sc2_resolve_terrain_textures() already tolerates by falling back to diffuse. Only a
         * missing *required* (diffuse) image is treated as a hard failure here. */
        fprintf(out, "Image: texture=%u channel=%s identity=\"%s\" status=not-found\n",
                texture, channel, identity);
        return required ? 1 : 0;
    }
    result = SC2_DdsParseResult(data, size, &dds);
    if (result != SC2_DDS_OK) {
        fprintf(out, "Image: texture=%u channel=%s identity=\"%s\" status=%s bytes=%u\n",
                texture, channel, identity, SC2_DdsResultString(result), size);
        sc2map_mem_free(data);
        return 1;
    }
    fprintf(out, "Image: texture=%u channel=%s identity=\"%s\" status=ok format=%s width=%u height=%u "
                 "mips=%u bytes=%u duplicate_of=%d\n",
            texture, channel, identity, sc2map_dds_format(&dds), dds.width, dds.height,
            dds.mipLevelCount, dds.mipLevels[dds.mipLevelCount - 1].offset +
            dds.mipLevels[dds.mipLevelCount - 1].size, duplicate == UINT32_MAX ? -1 : (int)duplicate);
    sc2map_mem_free(data);
    return 0;
}

static int sc2map_asset_inventory(FILE *out) {
    sc2Map_t const *map = SC2_MapCurrent();
    char seen[BZ_SC2A_MAX_TERRAIN_TEXTURES * 2][BZ_SC2A_MAX_IDENTITY] = {{0}};
    DWORD seen_count = 0, mask_layers = sc2_map_mask_layer_count(map);
    uint32_t available = sc2map_availability_flags(map);
    uint32_t malformed = sc2map_malformed_flags(map);
    uint32_t unsupported = sc2map_unsupported_flags(map);
    int failures = malformed ? 1 : 0;
    fprintf(out, "AssetInventory: cells=%ux%u hmap=%ux%u mask=%ux%ux%u textures=%u cliff_sets=%u "
                 "cliff_cells=%u objects=%u availability=0x%08x malformed=0x%08x unsupported=0x%08x "
                 "fog=%u lighting=%u\n",
            sc2_map_cell_width(map), sc2_map_cell_height(map),
            map->t3HeightMap ? map->t3HeightMap->width : 0, map->t3HeightMap ? map->t3HeightMap->height : 0,
            map->t3TextureMasks ? map->t3TextureMasks->width : 0,
            map->t3TextureMasks ? map->t3TextureMasks->height : 0, mask_layers,
            map->t3Terrain.num_terrain_textures, map->t3Terrain.num_cliff_sets,
            map->t3Terrain.num_cliff_cells, map->num_objects, available, malformed, unsupported,
            map->t3Terrain.fog_enabled, map->lighting.enabled);
    fprintf(out, "Catalog: units=%u actors=%u models=%u footprints=%u unresolved_models=%u\n",
            map->catalog.units, map->catalog.actors, map->catalog.models, map->catalog.footprints,
            map->catalog.unresolved_models);
    FOR_LOOP(i, SC2_TERRAIN_LAYER_COUNT)
        fprintf(out, "Layer: name=%s status=%s\n", SC2_MapTerrainLayerName((sc2TerrainLayerId_t)i),
                SC2_MapLayerStatusName(map->terrain_layer_status[i]));
    FOR_LOOP(i, map->t3Terrain.num_terrain_textures) {
        sc2TerrainTexture_t const *texture = &map->t3Terrain.terrain_textures[i];
        fprintf(out, "Texture: index=%u diffuse=\"%s\" normal=\"%s\"\n", i, texture->diffuse, texture->normal);
        failures += sc2map_inventory_image(out, i, "diffuse", texture->diffuse, true, seen, &seen_count);
        failures += sc2map_inventory_image(out, i, "normal", texture->normal, false, seen, &seen_count);
    }
    FOR_LOOP(i, map->t3Terrain.num_cliff_sets)
        fprintf(out, "CliffSet: index=%u name=\"%s\" mesh=\"%s\"\n", i,
                map->t3Terrain.cliff_sets[i].name, map->t3Terrain.cliff_sets[i].mesh);
    return failures ? 1 : 0;
}

int main(int argc, char **argv) {
    LPCSTR map = NULL;
    BOOL asset_inventory = false;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-mpq")) {
            if (++i >= argc || !sc2map_add_archive(argv[i])) {
                usage();
                sc2map_close_archives();
                return 1;
            }
        } else if (!strcmp(argv[i], "-data")) {
            if (++i >= argc) {
                usage();
                sc2map_close_archives();
                return 1;
            }
            sc2map_data_dir = argv[i];
        } else if (!strcmp(argv[i], "-map")) {
            if (++i >= argc) {
                usage();
                sc2map_close_archives();
                return 1;
            }
            map = argv[i];
        } else if (!strcmp(argv[i], "--asset-inventory")) {
            asset_inventory = true;
        } else if (argv[i][0] != '-' && !map) {
            map = argv[i];
        } else {
            usage();
            sc2map_close_archives();
            return 1;
        }
    }
    if (!map) {
        usage();
        sc2map_close_archives();
        return 1;
    }
    if (*sc2map_data_dir && !sc2map_add_data_archives(sc2map_data_dir)) {
        sc2map_close_archives();
        return 1;
    }

    SC2_MapSetHost(&(sc2MapHost_t){
        .read_file = sc2map_read_file,
        .free_file = sc2map_mem_free,
        .mem_alloc = sc2map_mem_alloc,
        .mem_free = sc2map_mem_free,
        .cvar_string = sc2map_cvar_string,
    });
    if (!SC2_MapLoad(map)) {
        fprintf(stderr, "sc2map: failed to load %s\n", map);
        sc2map_close_archives();
        return 1;
    }
    SC2_MapDump(stdout, map);
    int result = asset_inventory ? sc2map_asset_inventory(stdout) : 0;
    SC2_MapShutdown();
    sc2map_close_archives();
    return result;
}

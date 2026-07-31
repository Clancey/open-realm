/*
 * r_tabletop_null.c - headless renderer backend for the shared tabletop
 * client (see platform/tabletop/client), linked by every native host
 * (visionOS today; Android/Meta Quest later).
 *
 * Provides R_GetAPI() (and an R_StdoutGetAPI() alias - see below) so
 * client/cl_main.c's CL_Init()/CL_GetRendererAPI() link and run unmodified.
 * This backend creates no window, opens no GL context, and issues no
 * drawing: every renderer entry point either returns a harmless placeholder
 * value or is an explicit, named no-op. Unlike renderer/r_stdout.c (a
 * diagnostic tool that intentionally prints one line per call), this
 * backend never logs per-frame or per-call - only Init/Shutdown/RegisterMap
 * log, because those are one-time-per-session/one-time-per-map lifecycle
 * events, not per-frame noise.
 *
 * This file also owns the transport's Init/Shutdown pairing: re.Init() and
 * re.Shutdown() are the one seam guaranteed to bracket exactly one client
 * session (see client/cl_main.c's CL_Init()/CL_Shutdown()), so it is the
 * natural, symmetric place to bring the tabletop transport up and down
 * alongside the client it serves.
 *
 * MODEL/TEXTURE/FONT (see common/common.h's KNOWN_AS(model, MODEL) etc.)
 * are opaque handles as far as any non-renderer code is concerned - this
 * file never dereferences their fields, only hands out placeholder
 * allocations and frees them back, so it needs no dependency on the real
 * (SDL/OpenGL-only) renderer/r_local.h struct layouts.
 */
#include <stdio.h>

#include "client/tr_public.h"
#include "common/common.h"
#include "platform/bridge/bz_tabletop_transport.h"

static refImport_t ri;
static size2_t const null_window_size = { 0, 0 };

static void RNull_Init(DWORD width, DWORD height) {
    (void)width;
    (void)height;
    fprintf(stderr, "R_GetAPI: headless tabletop renderer active "
                     "(no window, no GL context, no drawing)\n");
    BZ_TT_Init();
}

static void RNull_Shutdown(void) {
    BZ_TT_Shutdown();
    fprintf(stderr, "R_GetAPI: headless renderer shutdown\n");
}

static void RNull_RegisterMap(LPCSTR mapFileName) {
    fprintf(stderr, "R_GetAPI: register_map \"%s\" (no-op: no renderer resources to build)\n",
            mapFileName ? mapFileName : "");
}

static void RNull_RenderFrame(viewDef_t const *viewdef) { (void)viewdef; }
static void RNull_SetFogOfWarData(DWORD width, DWORD height, BYTE const *data) {
    (void)width; (void)height; (void)data; /* the transport reads cl.fow.* directly */
}

static LPTEXTURE RNull_LoadTexture(LPCSTR fileName) {
    (void)fileName;
    return (LPTEXTURE)ri.MemAlloc(1);
}

static LPMODEL RNull_LoadModel(LPCSTR fileName) {
    (void)fileName;
    return (LPMODEL)ri.MemAlloc(1);
}

static LPFONT RNull_LoadFont(LPCSTR fileName, DWORD size) {
    (void)fileName; (void)size;
    return (LPFONT)ri.MemAlloc(1);
}

static size2_t RNull_GetWindowSize(void) { return null_window_size; }
static size2_t RNull_GetTextureSize(LPCTEXTURE texture) { (void)texture; return null_window_size; }

static void RNull_ReleaseTexture(LPTEXTURE texture) { if (texture) ri.MemFree(texture); }
static void RNull_ReleaseModel(LPMODEL model) { if (model) ri.MemFree(model); }

static void RNull_BeginFrame(void) {}
static void RNull_EndFrame(void) {}
static void RNull_DrawChar(int x, int y, int c) { (void)x; (void)y; (void)c; }
static void RNull_DrawFill(LPCRECT rect, COLOR32 color) { (void)rect; (void)color; }
static void RNull_DrawSelectionRect(LPCRECT rect, COLOR32 color) { (void)rect; (void)color; }
static void RNull_DrawPic(LPCTEXTURE texture, float x, float y) { (void)texture; (void)x; (void)y; }
static void RNull_DrawImage(LPCTEXTURE texture, LPCRECT screen, LPCRECT uv, COLOR32 color) {
    (void)texture; (void)screen; (void)uv; (void)color;
}
static void RNull_DrawImageEx(LPCDRAWIMAGE drawImage) { (void)drawImage; }
static void RNull_DrawBackdrop(LPCDRAWBACKDROP drawBackdrop) { (void)drawBackdrop; }
static void RNull_DrawMinimap(LPCRECT screen) { (void)screen; }
static void RNull_DrawLoadingIndicator(LPCRECT rect, DWORD time, COLOR32 color) {
    (void)rect; (void)time; (void)color;
}
static void RNull_DrawSprite(LPCMODEL model, LPCSTR anim, float x, float y) {
    (void)model; (void)anim; (void)x; (void)y;
}

static bool RNull_SetEntityAnimFrame(LPCMODEL model, LPCSTR anim, renderEntity_t *entity) {
    (void)model; (void)anim;
    if (!entity) return false;
    entity->frame = 0;
    entity->oldframe = 0;
    return true;
}

static void RNull_DrawText(LPCDRAWTEXT drawText) { (void)drawText; }
static VECTOR2 RNull_GetTextSize(LPCDRAWTEXT drawText) { (void)drawText; return MAKE(VECTOR2, 0, 0); }

static bool RNull_GetModelInfo(LPMODEL model, LPMODELINFO info) {
    (void)model;
    if (!info) return false;
    memset(info, 0, sizeof(*info));
    return true;
}

static void RNull_DrawBoundingBox(LPCBOX3 box, LPCMATRIX4 modelMatrix, LPCMATRIX4 vpMatrix, COLOR32 color) {
    (void)box; (void)modelMatrix; (void)vpMatrix; (void)color;
}

static FLOAT RNull_GetHeightAtPoint(float x, float y) { (void)x; (void)y; return 0.0f; }

static bool RNull_TraceEntity(viewDef_t const *viewdef, float x, float y, LPDWORD number) {
    (void)viewdef; (void)x; (void)y; (void)number;
    return false;
}

static bool RNull_TraceLocation(viewDef_t const *viewdef, float x, float y, LPVECTOR3 point) {
    (void)viewdef; (void)x; (void)y;
    if (point) *point = MAKE(VECTOR3, 0, 0, 0);
    return false;
}

static bool RNull_TraceMinimap(float x, float y, LPVECTOR2 outWorld) {
    (void)x; (void)y;
    if (outWorld) *outWorld = MAKE(VECTOR2, 0, 0);
    return false;
}

static DWORD RNull_EntitiesInRect(viewDef_t const *viewdef, LPCRECT rect, DWORD max, LPDWORD array) {
    (void)viewdef; (void)rect; (void)max; (void)array;
    return 0;
}

static refExport_t RNull_BuildExport(void) {
    return (refExport_t) {
        .Init = RNull_Init,
        .Shutdown = RNull_Shutdown,
        .RegisterMap = RNull_RegisterMap,
        .RenderFrame = RNull_RenderFrame,
        .SetFogOfWarData = RNull_SetFogOfWarData,
        .LoadTexture = RNull_LoadTexture,
        .LoadModel = RNull_LoadModel,
        .LoadFont = RNull_LoadFont,
        .GetWindowSize = RNull_GetWindowSize,
        .GetTextureSize = RNull_GetTextureSize,
        .ReleaseTexture = RNull_ReleaseTexture,
        .ReleaseModel = RNull_ReleaseModel,
        .BeginFrame = RNull_BeginFrame,
        .EndFrame = RNull_EndFrame,
        .DrawChar = RNull_DrawChar,
        .DrawFill = RNull_DrawFill,
        .DrawSelectionRect = RNull_DrawSelectionRect,
        .DrawPic = RNull_DrawPic,
        .DrawImage = RNull_DrawImage,
        .DrawImageEx = RNull_DrawImageEx,
        .DrawBackdrop = RNull_DrawBackdrop,
        .DrawMinimap = RNull_DrawMinimap,
        .DrawLoadingIndicator = RNull_DrawLoadingIndicator,
        .DrawSprite = RNull_DrawSprite,
        .SetEntityAnimFrame = RNull_SetEntityAnimFrame,
        .DrawText = RNull_DrawText,
        .GetTextSize = RNull_GetTextSize,
        .GetModelInfo = RNull_GetModelInfo,
        .DrawBoundingBox = RNull_DrawBoundingBox,
        .GetHeightAtPoint = RNull_GetHeightAtPoint,
        .TraceEntity = RNull_TraceEntity,
        .TraceLocation = RNull_TraceLocation,
        .TraceMinimap = RNull_TraceMinimap,
        .EntitiesInRect = RNull_EntitiesInRect,
    };
}

refExport_t R_GetAPI(refImport_t imp) {
    ri = imp;
    return RNull_BuildExport();
}

/* client/cl_main.c's CL_GetRendererAPI() calls R_StdoutGetAPI() by direct
 * symbol reference whenever cvar r_module is "stdout"/"text" (a desktop
 * diagnostic option, see renderer/r_stdout.c) - the symbol must resolve at
 * link time even though this build never sets that cvar. This tabletop
 * build has exactly one renderer backend regardless of r_module, so this is
 * an explicit, logged alias rather than a silent divergence from the
 * requested module. */
refExport_t R_StdoutGetAPI(refImport_t imp) {
    fprintf(stderr, "R_StdoutGetAPI: r_module=stdout is not available in the headless "
                     "tabletop build; using the headless renderer instead\n");
    return R_GetAPI(imp);
}

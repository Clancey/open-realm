#include "r_local.h"

static LPTEXTURE g_textures = NULL;

int R_RegisterTextureFile(char const *textureFileName) {
    LPTEXTURE tex = (LPTEXTURE)R_LoadTexture(textureFileName);
    if (tex) {
        FOR_LOOP(i, TEX_COUNT) if (tex == tr.texture[i]) return tex->texid;
        ADD_TO_LIST(tex, g_textures);
        return tex->texid;
    } else {
        return -1;
    }
}

/* Model-owned registry entries must be unlinked before their texture allocation is released. */
void R_UnregisterTextureFile(int texid) {
    LPTEXTURE *link = &g_textures;
    while (*link) {
        LPTEXTURE texture = *link;
        if (texture->texid != texid) { link = &texture->next; continue; }
        *link = texture->next; texture->next = NULL; R_ReleaseTexture(texture); return;
    }
}

struct texture const* R_FindTextureByID(DWORD textureID) {
    for (LPCTEXTURE tex = g_textures; tex; tex = tex->next) {
        if (tex->texid == textureID)
            return tex;
    }
    FOR_LOOP(i, TEX_COUNT)
        if (tr.texture[i] && tr.texture[i]->texid == textureID) return tr.texture[i];
    return NULL;
}

void R_BindTexture(LPCTEXTURE texture, DWORD unit) {
    R_Call(glActiveTexture, GL_TEXTURE0 + unit);
    R_Call(glBindTexture, GL_TEXTURE_2D, texture ? texture->texid : tr.texture[TEX_WHITE]->texid);
}

void R_SetTextureWrap(LPCTEXTURE texture, bool wrapS, bool wrapT) {
    if (!texture) {
        return;
    }
    R_Call(glBindTexture, GL_TEXTURE_2D, texture->texid);
    R_Call(glTexParameteri, GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrapS ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    R_Call(glTexParameteri, GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrapT ? GL_REPEAT : GL_CLAMP_TO_EDGE);
}

LPTEXTURE R_AllocateTexture(DWORD width, DWORD height) {
    LPTEXTURE texture = ri.MemAlloc(sizeof(TEXTURE));
    R_Call(glGenTextures, 1, &texture->texid);
    R_Call(glBindTexture, GL_TEXTURE_2D, texture->texid);
    texture->width = width;
    texture->height = height;
    return texture;
}

void R_ReleaseTexture(LPTEXTURE texture) {
    if (!texture) {
        return;
    }
    FOR_LOOP(i, TEX_COUNT) {
        /* Missing assets share renderer-owned placeholders; cache eviction must not free a built-in
           used by other slots. */
        if (texture == tr.texture[i])
            return;
    }
    R_Call(glDeleteTextures, 1, &texture->texid);
    texture->texid = 0;
    ri.MemFree(texture);
}

void R_LoadTextureMipLevel(LPCTEXTURE pTexture, DWORD level, LPCCOLOR32 pPixels, DWORD width, DWORD height) {
    if (width == 0 || height == 0)
        return;
    R_Call(glBindTexture, GL_TEXTURE_2D, pTexture->texid);
#if __linux__
    R_Call(glTexImage2D, GL_TEXTURE_2D, level, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pPixels);
#else
    R_Call(glTexImage2D, GL_TEXTURE_2D, level, GL_RGBA, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, pPixels);
#endif
    if (level > 0) {
        R_Call(glTexParameteri, GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, level);
        R_Call(glTexParameteri, GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    } else {
        R_Call(glTexParameteri, GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        R_Call(glTexParameteri, GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        R_Call(glTexParameteri, GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        R_Call(glTexParameteri, GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
}

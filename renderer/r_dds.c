#include "r_local.h"

/* The engine-agnostic validator is linked once into each desktop renderer module. */
#include "games/starcraft-2/common/sc2_dds.h"

#define GL_COMPRESSED_RGB_S3TC_DXT1_EXT  0x83F0
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3

LPCTEXTURE dds = NULL;

static void DDS_RejectOnce(sc2DdsResult_t result) {
    static DWORD warned;
    DWORD bit = 1u << (DWORD)result;
    if (!(warned & bit)) {
        fprintf(stderr, "R_LoadTextureDDS: %s\n", SC2_DdsResultString(result));
        warned |= bit;
    }
}

static GLint DDS_CompressedGLFormat(sc2DdsFormat_t format) {
    switch (format) {
    case SC2_DDS_FORMAT_DXT1: return GL_COMPRESSED_RGB_S3TC_DXT1_EXT;
    case SC2_DDS_FORMAT_DXT3: return GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
    case SC2_DDS_FORMAT_DXT5: return GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
    default:                 return 0; /* unreachable: SC2_DdsParse only ever emits these three */
    }
}

static void DDS_UncompressedGLFormat(sc2DdsChannels_t channels, GLint *internalFormat, GLenum *format) {
    switch (channels) {
    case SC2_DDS_CHANNELS_BGRA: *internalFormat = GL_RGBA; *format = GL_BGRA; return;
    case SC2_DDS_CHANNELS_RGBA: *internalFormat = GL_RGBA; *format = GL_RGBA; return;
    case SC2_DDS_CHANNELS_BGR:  *internalFormat = GL_RGB;  *format = GL_BGR;  return;
    case SC2_DDS_CHANNELS_RGB:  *internalFormat = GL_RGB;  *format = GL_RGB;  return;
    default:                    *internalFormat = GL_RGB;  *format = GL_RGB;  return; /* unreachable */
    }
}

LPTEXTURE R_LoadTextureDDS(HANDLE data, DWORD filesize) {
    BYTE const *buf = data;
    sc2DdsImage_t image;
    LPTEXTURE texture;

    sc2DdsResult_t result = SC2_DdsParseResult(buf, filesize, &image);
    /* Validation precedes GL allocation, preserving the old log-once behavior without leaking a
     * partially allocated texture when a malformed or unsupported file is encountered. */
    if (result != SC2_DDS_OK) {
        DDS_RejectOnce(result);
        return NULL;
    }

    texture = ri.MemAlloc(sizeof(TEXTURE));
    R_Call(glGenTextures, 1, &texture->texid);
    R_Call(glBindTexture, GL_TEXTURE_2D, texture->texid);
    R_Call(glTexParameteri, GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    R_Call(glTexParameteri, GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    R_Call(glTexParameteri, GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    if (image.blockBytes) {
        GLint format = DDS_CompressedGLFormat(image.format);

        R_Call(glTexParameteri, GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        R_Call(glTexParameteri, GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, image.mipLevelCount - 1);
        R_Call(glTexParameteri, GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        FOR_LOOP(i, image.mipLevelCount) {
            sc2DdsMipLevel_t const *level = &image.mipLevels[i];
            R_Call(glCompressedTexImage2D, GL_TEXTURE_2D, i, format, level->width, level->height, 0,
                   level->size, buf + image.dataOffset + level->offset);
        }
    } else {
        GLint internalFormat;
        GLenum format;

        DDS_UncompressedGLFormat(image.channels, &internalFormat, &format);
        R_Call(glTexParameteri, GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        R_Call(glTexParameteri, GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, image.mipLevelCount - 1);
        R_Call(glTexParameteri, GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
               image.mipLevelCount > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
        /* The parser publishes byte-tight rows; default GL alignment 4 overreads odd-width RGB24. */
        R_Call(glPixelStorei, GL_UNPACK_ALIGNMENT, 1);
        FOR_LOOP(i, image.mipLevelCount) {
            sc2DdsMipLevel_t const *level = &image.mipLevels[i];
            R_Call(glTexImage2D, GL_TEXTURE_2D, i, internalFormat, level->width, level->height, 0,
                   format, GL_UNSIGNED_BYTE, buf + image.dataOffset + level->offset);
        }
        R_Call(glPixelStorei, GL_UNPACK_ALIGNMENT, 4);
    }

    if (!dds) dds = texture;
    texture->width = image.width;
    texture->height = image.height;
    return texture;
}

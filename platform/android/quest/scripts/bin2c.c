/*
 * bin2c.c - tiny standalone host tool that turns a compiled SPIR-V binary
 * into a C 32-bit-word-array header, so the layer-3 Vulkan renderer can
 * embed its shaders without depending on Android's AAssetManager (see
 * docs/quest-tabletop.md's "Shader build pipeline"). Deliberately a plain
 * host C program (built with `cc`, not the NDK) so
 * platform/android/quest/scripts/build-shaders.sh can run it on the build
 * machine during a normal CMake configure/build, matching this repo's
 * "commit source, not opaque binaries" rule for reproducible shader
 * compilation.
 *
 * Emits `static const uint32_t <name>[]` (NOT `unsigned char[]`): SPIR-V is
 * defined as a stream of 32-bit words (SPIR-V spec section 2.2.1, "Layout
 * of a Module"), and VkShaderModuleCreateInfo::pCode requires a pointer
 * whose alignment is at least alignof(uint32_t) (Vulkan 1.3 spec, section
 * "vkCreateShaderModule": "pCode must be a valid pointer to an array of
 * codeSize/4 uint32_t values"). A `static const unsigned char[]` array's
 * element type has no alignment requirement beyond 1, so reinterpret-
 * casting its address to `const uint32_t *` (as an earlier version of this
 * tool's generated header + bz_quest_vk.c's call site did) is not
 * guaranteed valid by the C standard, even though it happens to work on
 * every ABI this project currently targets. Emitting the words as an
 * actual `uint32_t` array sidesteps the question entirely: the C compiler
 * guarantees `alignof(uint32_t)` alignment for any object of that type,
 * static or not.
 *
 * Endianness: this tool assumes the input SPIR-V file's words are encoded
 * little-endian (byte order 0x03,0x02,0x23,0x07 for the magic number
 * 0x07230203) - true for every host `glslc` build in this pipeline, and
 * also true of the arm64 Android target this ships to, so no byte-swap is
 * ever required in this project. Per the SPIR-V spec's magic-number
 * section, a reversed byte order in the file (0x07,0x23,0x02,0x03) would
 * mean the *entire module* is byte-swapped relative to a little-endian
 * reader and every word would need swapping before use - this tool
 * deliberately does not attempt to support or silently "fix" that case
 * (it would indicate a broken/foreign toolchain producing the input, not
 * something to paper over): it hard-fails instead, per this repo's "no
 * silent fallbacks" rule.
 *
 * Usage: bin2c <input.spv> <output.h> <array_name>
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <input.spv> <output.h> <array_name>\n", argv[0]);
        return 1;
    }

    FILE *in = fopen(argv[1], "rb");
    if (!in) {
        fprintf(stderr, "bin2c: failed to open input '%s'\n", argv[1]);
        return 1;
    }
    fseek(in, 0, SEEK_END);
    long size = ftell(in);
    if (size <= 0 || (size % 4) != 0) {
        fprintf(stderr,
                "bin2c: '%s' is not a valid SPIR-V binary (size=%ld must be positive and a "
                "multiple of 4)\n",
                argv[1], size);
        fclose(in);
        return 1;
    }
    fseek(in, 0, SEEK_SET);

    unsigned char *bytes = (unsigned char *)malloc((size_t)size);
    if (!bytes) {
        fprintf(stderr, "bin2c: out of memory reading '%s'\n", argv[1]);
        fclose(in);
        return 1;
    }
    if (fread(bytes, 1, (size_t)size, in) != (size_t)size) {
        fprintf(stderr, "bin2c: short read on '%s'\n", argv[1]);
        fclose(in);
        free(bytes);
        return 1;
    }
    fclose(in);

    /* Repack the raw bytes into native 32-bit words, explicitly assuming
     * little-endian byte order within each word - see this file's top
     * comment. Doing this via shifts (rather than a `uint32_t *` cast over
     * `bytes`) also sidesteps any alignment/strict-aliasing concern on the
     * *reading* side, in addition to fixing the *embedded* array's
     * alignment guarantee described above. */
    const uint32_t wordCount = (uint32_t)(size / 4);
    uint32_t *words = (uint32_t *)malloc((size_t)wordCount * sizeof(uint32_t));
    if (!words) {
        fprintf(stderr, "bin2c: out of memory converting '%s'\n", argv[1]);
        free(bytes);
        return 1;
    }
    for (uint32_t i = 0; i < wordCount; i++) {
        const unsigned char *b = &bytes[i * 4];
        words[i] = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
                   ((uint32_t)b[3] << 24);
    }
    free(bytes);

    /* SPIR-V magic number, required to be the first word, little-endian
     * byte order 0x03,0x02,0x23,0x07 per the SPIR-V spec section 2.3
     * "Physical Layout of a SPIR-V Module and Instruction" -
     * https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#PhysicalLayout
     * Only the little-endian encoding is accepted - see this file's top
     * comment on why a byte-swapped module is a hard failure, not a
     * silently handled case. */
    static const uint32_t kSpirvMagic = 0x07230203u;
    if (wordCount < 1 || words[0] != kSpirvMagic) {
        fprintf(stderr,
                "bin2c: '%s' does not start with the little-endian SPIR-V magic number "
                "(got 0x%08x, expected 0x%08x) - refusing to embed\n",
                argv[1], wordCount ? words[0] : 0u, kSpirvMagic);
        free(words);
        return 1;
    }

    FILE *out = fopen(argv[2], "w");
    if (!out) {
        fprintf(stderr, "bin2c: failed to open output '%s'\n", argv[2]);
        free(words);
        return 1;
    }
    fprintf(out, "/* Generated by platform/android/quest/scripts/bin2c.c from %s - do not edit. */\n",
            argv[1]);
    fprintf(out, "#include <stdint.h>\n");
    /* uint32_t, not unsigned char: see this file's top comment -
     * VkShaderModuleCreateInfo::pCode requires uint32_t alignment, which a
     * byte array does not guarantee. */
    fprintf(out, "static const uint32_t %s[] = {\n", argv[3]);
    for (uint32_t i = 0; i < wordCount; i++) {
        fprintf(out, "0x%08xu,%s", words[i], ((i + 1) % 8 == 0) ? "\n" : " ");
    }
    fprintf(out, "\n};\n");
    /* Byte length (matches VkShaderModuleCreateInfo::codeSize's unit), not
     * word count - callers must not confuse the two. */
    fprintf(out, "static const uint32_t %s_len = %uu;\n", argv[3], (unsigned)size);
    fclose(out);
    free(words);
    return 0;
}

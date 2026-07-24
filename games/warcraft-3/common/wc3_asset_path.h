#ifndef WC3_ASSET_PATH_H
#define WC3_ASSET_PATH_H

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

enum {
    BZ_WC3_MODEL_MDL_TO_MDX,
    BZ_WC3_MODEL_STRIP_VARIATION,
};

typedef bool (*wc3_model_probe_t)(const char *identity, void *context);
typedef struct {
    const char *identity;
    uint32_t fallback;
    char *out;
    size_t cap;
} wc3ModelFallback_t;
typedef struct {
    const char *identity;
    wc3_model_probe_t probe;
    void *context;
    char *out;
    size_t cap;
} wc3ModelResolve_t;
typedef struct {
    const char *dir, *file;
    uint32_t variation, num_variations;
    wc3_model_probe_t probe;
    void *context;
    char *out;
    size_t cap;
} wc3SpawnModelResolve_t;

/* WC3 model tables may name text MDL files or append one variation digit absent from the archive model. */
static inline bool wc3_model_fallback_identity(const wc3ModelFallback_t *input) {
    size_t length, stem;
    if (!input || !input->identity || !input->out || input->cap < 5 ||
        (length = strlen(input->identity)) < 4) return false;
    input->out[0] = 0;
    stem = length - 4;
    if (input->fallback == BZ_WC3_MODEL_MDL_TO_MDX) {
        if (strcasecmp(input->identity + stem, ".mdl")) return false;
    } else if (input->fallback == BZ_WC3_MODEL_STRIP_VARIATION) {
        if (stem && isdigit((unsigned char)input->identity[stem - 1])) stem--;
    } else
        return false;
    if (stem > input->cap - 5) return false;
    memcpy(input->out, input->identity, stem);
    memcpy(input->out + stem, ".mdx", 5);
    return true;
}

/* Keep authored identity fallback order identical for spawn, desktop loading, and export. */
static inline bool wc3_resolve_model_identity(const wc3ModelResolve_t *input) {
    char candidate[512];
    if (!input || !input->identity || !input->probe || !input->out || !input->cap) return false;
    input->out[0] = 0;
    if (strlen(input->identity) >= input->cap) return false;
    memcpy(input->out, input->identity, strlen(input->identity) + 1);
    if (input->probe(input->out, input->context)) return true;
    for (uint32_t fallback = BZ_WC3_MODEL_MDL_TO_MDX;
         fallback <= BZ_WC3_MODEL_STRIP_VARIATION; fallback++) {
        wc3ModelFallback_t alternate = {
            .identity = input->identity, .fallback = fallback,
            .out = candidate, .cap = sizeof(candidate),
        };
        if (wc3_model_fallback_identity(&alternate) &&
            input->probe(candidate, input->context)) {
            if (strlen(candidate) >= input->cap) return false;
            memcpy(input->out, candidate, strlen(candidate) + 1);
            return true;
        }
    }
    return false;
}

/* numVar controls whether the table's file stem has authored numeric variants. */
static inline bool wc3_resolve_spawn_model_identity(const wc3SpawnModelResolve_t *input) {
    char identity[512];
    int length;
    wc3ModelResolve_t resolve;
    if (!input || !input->file || !*input->file || !input->out || !input->cap) return false;
    input->out[0] = 0;
    if (input->dir && *input->dir) {
        if (input->num_variations > 1)
            length = snprintf(identity, sizeof(identity), "%s\\%s\\%s%u.mdx",
                              input->dir, input->file, input->file, input->variation);
        else length = snprintf(identity, sizeof(identity), "%s\\%s\\%s.mdx",
                               input->dir, input->file, input->file);
    } else if (input->num_variations > 1)
        length = snprintf(identity, sizeof(identity), "%s%u.mdx", input->file, input->variation);
    else
        length = snprintf(identity, sizeof(identity), "%s.mdx", input->file);
    if (length < 0 || (size_t)length >= sizeof(identity)) return false;
    resolve = (wc3ModelResolve_t){
        .identity = identity, .probe = input->probe, .context = input->context, .out = input->out,
        .cap = input->cap,
    };
    return wc3_resolve_model_identity(&resolve);
}

#endif

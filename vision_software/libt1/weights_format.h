#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * Weight blobs live in reserved DDR and are passed to T1 by physical
 * address. The on-disk convention is one contiguous row-major file per
 * layer, copied into a 128-byte-aligned reserved-memory region during
 * boot before visionsoc_main starts.
 *
 * File size per layer:
 *   out_ch * in_ch * kH * kW bytes
 */
struct weights_layer_descriptor {
    uint32_t pa;
    uint32_t out_ch;
    uint32_t in_ch;
    uint32_t kH;
    uint32_t kW;
    int8_t scale;
    int8_t zero_pt;
};

/* Future extension point for a JSON or binary sidecar manifest loader. */
int weights_load_manifest(const char *path,
                          struct weights_layer_descriptor **out_layers,
                          size_t *out_count);

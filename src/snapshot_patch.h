#pragma once
#include <stddef.h>
#include <stdint.h>

#define SNAPSHOT_POLICY_VERSION 2u
typedef struct {
    uint32_t string_objects;
    uint32_t class_id;
    uint32_t layouts;
    uint32_t api_paths;
    uint32_t ad_fields;
    uint32_t ad_assets;
    uint32_t patched_objects;
} SnapshotReport;

/* Discover Dart string layouts in read-only ELF data; no client profile,
 * fixed file size, class ID, offset, or source SHA allowlist is used. */
int snapshot_patch_image(uint8_t *data, size_t length, SnapshotReport *report, int apply);

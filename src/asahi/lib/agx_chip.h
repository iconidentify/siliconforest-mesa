/*
 * Copyright 2026 The Asahi Linux Contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include "drm-uapi/asahi_drm.h"
#include "libagx_dgc.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Classify the DRM global parameters into the closest known architecture
 * identity. May return a recognized-but-unsupported G15/G16/G17 identity, or
 * AGX_CHIP_UNKNOWN when the generation or topology is out of range. This never
 * decides support; callers must gate on agx_chip_supported(). enum agx_chip is
 * defined in libagx_dgc.h, where the command encoders consume it.
 */
enum agx_chip
agx_chip_from_params(const struct drm_asahi_params_global *params);

/* True only for architectures with a complete, validated Mesa backend. */
bool
agx_chip_supported(enum agx_chip chip);

/* Human-readable identity, e.g. "G17X". Never NULL. */
const char *
agx_chip_name(enum agx_chip chip);

#ifdef __cplusplus
}
#endif

/*
 * Copyright 2026 The Asahi Linux Contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "drm-uapi/asahi_drm.h"
#include "libagx_dgc.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Classify the DRM global parameters into an exactly-supported command-stream
 * architecture identity, or AGX_CHIP_UNKNOWN when the generation or topology is
 * out of range. enum agx_chip is defined in libagx_dgc.h, where the command
 * encoders consume it; this header only adds host-side helpers on top of it.
 */
enum agx_chip
agx_chip_from_params(const struct drm_asahi_params_global *params);

#ifdef __cplusplus
}
#endif

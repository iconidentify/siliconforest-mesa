/*
 * Copyright 2026 The Asahi Linux Contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

enum agx_chip {
   AGX_CHIP_G13G,
   AGX_CHIP_G13X,
   AGX_CHIP_G14G,
   AGX_CHIP_G14X,
   AGX_CHIP_UNKNOWN,
};

#ifndef __OPENCL_VERSION__

#include "drm-uapi/asahi_drm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Return the exact supported command-stream architecture, or UNKNOWN. */
enum agx_chip
agx_chip_from_params(const struct drm_asahi_params_global *params);

#ifdef __cplusplus
}
#endif

#endif

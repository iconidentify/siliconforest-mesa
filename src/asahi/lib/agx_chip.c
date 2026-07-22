/*
 * Copyright 2026 The Asahi Linux Contributors
 * SPDX-License-Identifier: MIT
 */

#include "agx_chip.h"

enum agx_chip
agx_chip_from_params(const struct drm_asahi_params_global *params)
{
   if (params->num_clusters_total == 0 ||
       params->num_clusters_total > DRM_ASAHI_MAX_CLUSTERS)
      return AGX_CHIP_UNKNOWN;

   /* These are command-stream architecture identities, not an ordering. */
   switch (params->gpu_generation) {
   case 13:
      return params->num_clusters_total > 1 ? AGX_CHIP_G13X
                                            : AGX_CHIP_G13G;
   case 14:
      return params->num_clusters_total > 1 ? AGX_CHIP_G14X
                                            : AGX_CHIP_G14G;
   default:
      return AGX_CHIP_UNKNOWN;
   }
}

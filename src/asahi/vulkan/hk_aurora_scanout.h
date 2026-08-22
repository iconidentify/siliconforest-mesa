/*
 * Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 */
#ifndef HK_AURORA_SCANOUT_H
#define HK_AURORA_SCANOUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Private pNext ABI shared with Aurora's renamed DXVK UMD build. */
#define HK_AURORA_TIGHT_LINEAR_IMAGE_STYPE UINT32_C(0x7ff84158)
#define HK_AURORA_TIGHT_LINEAR_IMAGE_VERSION UINT32_C(1)

static inline bool
hk_aurora_tight_linear_stride(uint32_t width_px, uint32_t bytes_per_pixel,
                              uint64_t requested_stride_B,
                              uint32_t *stride_B_out)
{
   const uint64_t tight_stride_B =
      (uint64_t)width_px * (uint64_t)bytes_per_pixel;

   if (stride_B_out == NULL || width_px == 0 || bytes_per_pixel == 0 ||
       requested_stride_B != tight_stride_B ||
       requested_stride_B > UINT32_MAX || (requested_stride_B & 15u) != 0)
      return false;

   *stride_B_out = (uint32_t)requested_stride_B;
   return true;
}

#endif

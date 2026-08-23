/*
 * Copyright 2026 DjDeveloperr
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum hk_host_image_copy_footprint {
   HK_HOST_IMAGE_COPY_MEMCPY_LEVEL = 0,
   HK_HOST_IMAGE_COPY_LINEAR_RECT,
   HK_HOST_IMAGE_COPY_TILED_LEVEL,
};

struct hk_host_image_copy_span {
   uint64_t offset_B;
   uint64_t size_B;
};

struct hk_host_image_copy_span_info {
   uint64_t bo_size_B;
   uint64_t plane_offset_B;
   uint64_t layer_level_offset_B;
   uint64_t level_size_B;
   uint64_t row_stride_B;
   uint32_t x_el;
   uint32_t y_el;
   uint32_t width_el;
   uint32_t height_el;
   uint32_t block_size_B;
   enum hk_host_image_copy_footprint footprint;
};

static inline bool
hk_host_image_copy_u64_add(uint64_t a, uint64_t b, uint64_t *sum)
{
   if (UINT64_MAX - a < b)
      return false;

   *sum = a + b;
   return true;
}

static inline bool
hk_host_image_copy_u64_mul(uint64_t a, uint64_t b, uint64_t *product)
{
   if (a != 0 && b > UINT64_MAX / a)
      return false;

   *product = a * b;
   return true;
}

/* Return one conservative allocation-relative span for one mip level in one
 * layer. Memcpy copies and tiled writes cover the complete level. Linear
 * copies cover the bytes from the first touched byte through the last touched
 * row, including intervening row padding. */
static inline bool
hk_host_image_copy_span(const struct hk_host_image_copy_span_info *info,
                        struct hk_host_image_copy_span *span)
{
   uint64_t level_offset_B;
   if (info == NULL || span == NULL || info->level_size_B == 0 ||
       !hk_host_image_copy_u64_add(info->plane_offset_B,
                                   info->layer_level_offset_B,
                                   &level_offset_B))
      return false;

   uint64_t offset_B = level_offset_B;
   uint64_t size_B = info->level_size_B;
   uint64_t level_end_B;
   if (!hk_host_image_copy_u64_add(level_offset_B, info->level_size_B,
                                    &level_end_B))
      return false;

   switch (info->footprint) {
   case HK_HOST_IMAGE_COPY_MEMCPY_LEVEL:
   case HK_HOST_IMAGE_COPY_TILED_LEVEL:
      break;

   case HK_HOST_IMAGE_COPY_LINEAR_RECT: {
      if (info->width_el == 0 || info->height_el == 0 ||
          info->block_size_B == 0 || info->row_stride_B == 0)
         return false;

      uint64_t first_row_B, x_B, last_row_B, width_B, end_B;
      if (!hk_host_image_copy_u64_mul(info->y_el, info->row_stride_B,
                                      &first_row_B) ||
          !hk_host_image_copy_u64_mul(info->x_el, info->block_size_B, &x_B) ||
          !hk_host_image_copy_u64_add(level_offset_B, first_row_B,
                                      &offset_B) ||
          !hk_host_image_copy_u64_add(offset_B, x_B, &offset_B) ||
          !hk_host_image_copy_u64_mul(info->height_el - 1,
                                      info->row_stride_B, &last_row_B) ||
          !hk_host_image_copy_u64_mul(info->width_el, info->block_size_B,
                                      &width_B) ||
          !hk_host_image_copy_u64_add(offset_B, last_row_B, &end_B) ||
          !hk_host_image_copy_u64_add(end_B, width_B, &end_B))
         return false;

      size_B = end_B - offset_B;
      if (end_B > level_end_B)
         return false;
      break;
   }

   default:
      return false;
   }

   if (level_end_B > info->bo_size_B || offset_B > info->bo_size_B ||
       size_B > info->bo_size_B - offset_B)
      return false;

   span->offset_B = offset_B;
   span->size_B = size_B;
   return true;
}

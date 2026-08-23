/*
 * Copyright 2026 DjDeveloperr
 * SPDX-License-Identifier: MIT
 */

#include "../hk_host_image_copy.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned checks;

#define CHECK(condition)                                                       \
   do {                                                                        \
      checks++;                                                                \
      if (!(condition)) {                                                      \
         fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,   \
                 #condition);                                                  \
         exit(EXIT_FAILURE);                                                   \
      }                                                                        \
   } while (0)

#define CHECK_U64(actual, expected)                                            \
   do {                                                                        \
      uint64_t _actual = (actual);                                             \
      uint64_t _expected = (expected);                                         \
      checks++;                                                                \
      if (_actual != _expected) {                                              \
         fprintf(stderr,                                                       \
                 "%s:%d: got 0x%" PRIx64 ", expected 0x%" PRIx64 "\n", \
                 __FILE__, __LINE__, _actual, _expected);                      \
         exit(EXIT_FAILURE);                                                   \
      }                                                                        \
   } while (0)

static struct hk_host_image_copy_span
span_for(struct hk_host_image_copy_span_info info)
{
   struct hk_host_image_copy_span span;
   CHECK(hk_host_image_copy_span(&info, &span));
   return span;
}

static void
test_memcpy_and_tiled_cover_one_level_in_one_layer(void)
{
   struct hk_host_image_copy_span_info info = {
      .bo_size_B = 0x200000,
      .plane_offset_B = 0x40000,
      .layer_level_offset_B = 0x23000,
      .level_size_B = 0x5000,
      .footprint = HK_HOST_IMAGE_COPY_MEMCPY_LEVEL,
   };

   struct hk_host_image_copy_span span = span_for(info);
   CHECK_U64(span.offset_B, 0x63000);
   CHECK_U64(span.size_B, 0x5000);

   info.layer_level_offset_B += 0x18000;
   info.footprint = HK_HOST_IMAGE_COPY_TILED_LEVEL;
   span = span_for(info);
   CHECK_U64(span.offset_B, 0x7b000);
   CHECK_U64(span.size_B, 0x5000);
}

static void
test_linear_copy_covers_first_byte_through_last_row(void)
{
   const struct hk_host_image_copy_span span =
      span_for((struct hk_host_image_copy_span_info){
         .bo_size_B = 0x100000,
         .plane_offset_B = 0x10000,
         .layer_level_offset_B = 0x20000,
         .level_size_B = 0x18000,
         .row_stride_B = 0x400,
         .x_el = 7,
         .y_el = 3,
         .width_el = 19,
         .height_el = 5,
         .block_size_B = 4,
         .footprint = HK_HOST_IMAGE_COPY_LINEAR_RECT,
      });

   CHECK_U64(span.offset_B, 0x30c1c);
   CHECK_U64(span.size_B, 0x104c);
}

static void
test_invalid_or_overflowing_spans_fail_closed(void)
{
   struct hk_host_image_copy_span span;
   struct hk_host_image_copy_span_info info = {
      .bo_size_B = 0x10000,
      .plane_offset_B = 0xf000,
      .layer_level_offset_B = 0x800,
      .level_size_B = 0x1000,
      .footprint = HK_HOST_IMAGE_COPY_TILED_LEVEL,
   };

   CHECK(!hk_host_image_copy_span(&info, &span));

   info = (struct hk_host_image_copy_span_info){
      .bo_size_B = UINT64_MAX,
      .plane_offset_B = UINT64_MAX - 4,
      .layer_level_offset_B = 8,
      .level_size_B = 1,
      .footprint = HK_HOST_IMAGE_COPY_MEMCPY_LEVEL,
   };
   CHECK(!hk_host_image_copy_span(&info, &span));

   info = (struct hk_host_image_copy_span_info){
      .bo_size_B = 0x10000,
      .plane_offset_B = 0,
      .layer_level_offset_B = 0,
      .level_size_B = 0x10000,
      .row_stride_B = 256,
      .width_el = 1,
      .height_el = 0,
      .block_size_B = 4,
      .footprint = HK_HOST_IMAGE_COPY_LINEAR_RECT,
   };
   CHECK(!hk_host_image_copy_span(&info, &span));

   info = (struct hk_host_image_copy_span_info){
      .bo_size_B = 0x20000,
      .plane_offset_B = 0x1000,
      .layer_level_offset_B = 0x2000,
      .level_size_B = 0x1000,
      .row_stride_B = 0x400,
      .y_el = 3,
      .width_el = 1,
      .height_el = 2,
      .block_size_B = 4,
      .footprint = HK_HOST_IMAGE_COPY_LINEAR_RECT,
   };
   CHECK(!hk_host_image_copy_span(&info, &span));
}

int
main(void)
{
   test_memcpy_and_tiled_cover_one_level_in_one_layer();
   test_linear_copy_covers_first_byte_through_last_row();
   test_invalid_or_overflowing_spans_fail_closed();
   printf("hk host image copy: %u checks passed\n", checks);
   return EXIT_SUCCESS;
}

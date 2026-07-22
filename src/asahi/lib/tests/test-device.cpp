/*
 * Copyright 2026 The Asahi Linux Contributors
 * SPDX-License-Identifier: MIT
 */

#include "agx_chip.h"

#include <gtest/gtest.h>

static void
expect_chip(uint32_t generation, uint32_t clusters, enum agx_chip expected)
{
   const struct drm_asahi_params_global params = {
      .gpu_generation = generation,
      .num_clusters_total = clusters,
   };

   EXPECT_EQ(agx_chip_from_params(&params), expected);
}

TEST(Device, SelectSupportedChip)
{
   expect_chip(13, 1, AGX_CHIP_G13G);
   expect_chip(13, 2, AGX_CHIP_G13X);
   expect_chip(14, 1, AGX_CHIP_G14G);
   expect_chip(14, 2, AGX_CHIP_G14X);
}

TEST(Device, RejectUnsupportedGeneration)
{
   for (uint32_t generation : {0u, 12u, 15u, 16u, 17u, 18u, UINT32_MAX}) {
      for (uint32_t clusters : {1u, 2u, 8u}) {
         const struct drm_asahi_params_global params = {
            .gpu_generation = generation,
            .num_clusters_total = clusters,
         };
         EXPECT_EQ(agx_chip_from_params(&params), AGX_CHIP_UNKNOWN);
      }
   }
}

TEST(Device, RejectInvalidTopology)
{
   for (uint32_t generation : {13u, 14u}) {
      for (uint32_t clusters : {0u, DRM_ASAHI_MAX_CLUSTERS + 1u,
                                UINT32_MAX}) {
         const struct drm_asahi_params_global params = {
            .gpu_generation = generation,
            .num_clusters_total = clusters,
         };

         EXPECT_EQ(agx_chip_from_params(&params), AGX_CHIP_UNKNOWN);
      }
   }
}

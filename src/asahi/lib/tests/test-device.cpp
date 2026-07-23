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

TEST(Device, RecognizeUnsupportedGeneration)
{
   /* Post-M2 hardware must be named precisely, yet must not be "supported". */
   expect_chip(15, 1, AGX_CHIP_G15G);
   expect_chip(15, 2, AGX_CHIP_G15X);
   expect_chip(16, 1, AGX_CHIP_G16G);
   expect_chip(16, 2, AGX_CHIP_G16X);
   expect_chip(17, 1, AGX_CHIP_G17G);
   expect_chip(17, 2, AGX_CHIP_G17X);

   for (enum agx_chip chip : {AGX_CHIP_G15G, AGX_CHIP_G15X, AGX_CHIP_G16G,
                              AGX_CHIP_G16X, AGX_CHIP_G17G, AGX_CHIP_G17X})
      EXPECT_FALSE(agx_chip_supported(chip));
}

TEST(Device, RejectUnknownGeneration)
{
   for (uint32_t generation : {0u, 12u, 18u, 19u, UINT32_MAX}) {
      for (uint32_t clusters : {1u, 2u, 8u}) {
         const struct drm_asahi_params_global params = {
            .gpu_generation = generation,
            .num_clusters_total = clusters,
         };
         EXPECT_EQ(agx_chip_from_params(&params), AGX_CHIP_UNKNOWN);
      }
   }
}

TEST(Device, OnlyBaselineChipsSupported)
{
   EXPECT_TRUE(agx_chip_supported(AGX_CHIP_G13G));
   EXPECT_TRUE(agx_chip_supported(AGX_CHIP_G13X));
   EXPECT_TRUE(agx_chip_supported(AGX_CHIP_G14G));
   EXPECT_TRUE(agx_chip_supported(AGX_CHIP_G14X));
   EXPECT_FALSE(agx_chip_supported(AGX_CHIP_UNKNOWN));
}

TEST(Device, StaticChipParametersGen3)
{
   /* G15/M3: Gen3 ISA, archive gen 1, IFT gen 1, USC gen UNKNOWN. */
   for (enum agx_chip chip : {AGX_CHIP_G15G, AGX_CHIP_G15X}) {
      const struct agx_chip_info *info = agx_chip_get_info(chip);
      EXPECT_EQ(info->isa_gen, AGX_ISA_GEN3);
      EXPECT_EQ(info->archive_gen, 1);
      EXPECT_EQ(info->ift_gen, 1);
      EXPECT_EQ(info->usc_gen, AGX_GEN_UNKNOWN);
   }

   /* G16/M4: Gen3 ISA, archive gen 2, IFT gen 1, USC gen UNKNOWN. */
   for (enum agx_chip chip : {AGX_CHIP_G16G, AGX_CHIP_G16X}) {
      const struct agx_chip_info *info = agx_chip_get_info(chip);
      EXPECT_EQ(info->isa_gen, AGX_ISA_GEN3);
      EXPECT_EQ(info->archive_gen, 2);
      EXPECT_EQ(info->ift_gen, 1);
      EXPECT_EQ(info->usc_gen, AGX_GEN_UNKNOWN);
   }

   /* G17/M5: Gen3 ISA, archive gen 3, IFT gen 2, USC gen 3 (live G17S ioreg). */
   for (enum agx_chip chip : {AGX_CHIP_G17G, AGX_CHIP_G17X}) {
      const struct agx_chip_info *info = agx_chip_get_info(chip);
      EXPECT_EQ(info->isa_gen, AGX_ISA_GEN3);
      EXPECT_EQ(info->archive_gen, 3);
      EXPECT_EQ(info->ift_gen, 2);
   }

   /* USC gen 3 is captured for G17X only; G17G stays UNKNOWN. */
   EXPECT_EQ(agx_chip_get_info(AGX_CHIP_G17X)->usc_gen, 3);
   EXPECT_EQ(agx_chip_get_info(AGX_CHIP_G17G)->usc_gen, AGX_GEN_UNKNOWN);
}

TEST(Device, StaticChipParametersGen2)
{
   /* Baseline stays Gen2 with no post-M2 archive/IFT ladder. */
   for (enum agx_chip chip : {AGX_CHIP_G13G, AGX_CHIP_G13X, AGX_CHIP_G14G,
                              AGX_CHIP_G14X}) {
      const struct agx_chip_info *info = agx_chip_get_info(chip);
      EXPECT_EQ(info->isa_gen, AGX_ISA_GEN2);
      EXPECT_EQ(info->archive_gen, AGX_GEN_UNKNOWN);
      EXPECT_EQ(info->ift_gen, AGX_GEN_UNKNOWN);
   }

   /* Unknown/out-of-range is a valid, all-UNKNOWN descriptor, never NULL. */
   const struct agx_chip_info *unknown = agx_chip_get_info(AGX_CHIP_UNKNOWN);
   ASSERT_NE(unknown, nullptr);
   EXPECT_EQ(unknown->isa_gen, AGX_ISA_GEN_UNKNOWN);
}

TEST(Device, Gen3ChipsStayFailClosed)
{
   /*
    * Taxonomy invariant tied to fail-closed: every Gen3 identity we can now name
    * is still unsupported. Attaching parameters must never enable a chip.
    */
   for (enum agx_chip chip : {AGX_CHIP_G15G, AGX_CHIP_G15X, AGX_CHIP_G16G,
                              AGX_CHIP_G16X, AGX_CHIP_G17G, AGX_CHIP_G17X}) {
      EXPECT_EQ(agx_chip_get_info(chip)->isa_gen, AGX_ISA_GEN3);
      EXPECT_FALSE(agx_chip_supported(chip));
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

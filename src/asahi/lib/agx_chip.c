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

   /*
    * These are command-stream architecture identities, not an ordering. The
    * ">1 cluster -> X die" split mirrors the proven G13/G14 packaging. For the
    * G15/G16/G17 rows it only picks a precise *name* for an unsupported device;
    * agx_chip_supported() keeps them fail-closed regardless. Do NOT collapse
    * these into a "generation >= N" test: G17P (HAL200) deliberately does not
    * share the G17G/G17X (HAL300) contract.
    */
   switch (params->gpu_generation) {
   case 13:
      return params->num_clusters_total > 1 ? AGX_CHIP_G13X : AGX_CHIP_G13G;
   case 14:
      return params->num_clusters_total > 1 ? AGX_CHIP_G14X : AGX_CHIP_G14G;
   case 15:
      return params->num_clusters_total > 1 ? AGX_CHIP_G15X : AGX_CHIP_G15G;
   case 16:
      return params->num_clusters_total > 1 ? AGX_CHIP_G16X : AGX_CHIP_G16G;
   case 17:
      return params->num_clusters_total > 1 ? AGX_CHIP_G17X : AGX_CHIP_G17G;
   default:
      return AGX_CHIP_UNKNOWN;
   }
}

bool
agx_chip_supported(enum agx_chip chip)
{
   /*
    * Explicit allowlist: adding an identity to enum agx_chip must never enable
    * it by accident. Only architectures with a validated Mesa backend belong
    * here. G15/G16/G17 stay out until their command-stream, ISA, and firmware
    * contracts are implemented and tested.
    */
   switch (chip) {
   case AGX_CHIP_G13G:
   case AGX_CHIP_G13X:
   case AGX_CHIP_G14G:
   case AGX_CHIP_G14X:
      return true;
   default:
      return false;
   }
}

const char *
agx_chip_name(enum agx_chip chip)
{
   switch (chip) {
   case AGX_CHIP_G13G:
      return "G13G";
   case AGX_CHIP_G13X:
      return "G13X";
   case AGX_CHIP_G14G:
      return "G14G";
   case AGX_CHIP_G14X:
      return "G14X";
   case AGX_CHIP_G15G:
      return "G15G";
   case AGX_CHIP_G15X:
      return "G15X";
   case AGX_CHIP_G16G:
      return "G16G";
   case AGX_CHIP_G16X:
      return "G16X";
   case AGX_CHIP_G17G:
      return "G17G";
   case AGX_CHIP_G17X:
      return "G17X";
   case AGX_CHIP_UNKNOWN:
      return "unknown";
   }

   return "unknown";
}

/*
 * Grounded static parameters, keyed by identity. Every value is either VERIFIED
 * from captured evidence (see agx_chip.h header note) or an explicit UNKNOWN
 * sentinel -- nothing here is guessed, and none of it enables a chip.
 */
static const struct agx_chip_info agx_chip_infos[] = {
   /* Gen2 baseline: predates the post-M2 archive/IFT ladder. */
   [AGX_CHIP_G13G] = {.isa_gen = AGX_ISA_GEN2},
   [AGX_CHIP_G13X] = {.isa_gen = AGX_ISA_GEN2},
   [AGX_CHIP_G14G] = {.isa_gen = AGX_ISA_GEN2},
   [AGX_CHIP_G14X] = {.isa_gen = AGX_ISA_GEN2},

   /* G15/M3: Gen3, archive gen 1, IFT gen 1. USC gen not captured. */
   [AGX_CHIP_G15G] = {.isa_gen = AGX_ISA_GEN3, .archive_gen = 1, .ift_gen = 1},
   [AGX_CHIP_G15X] = {.isa_gen = AGX_ISA_GEN3, .archive_gen = 1, .ift_gen = 1},

   /* G16/M4: Gen3, archive gen 2, IFT gen 1. USC gen not captured. */
   [AGX_CHIP_G16G] = {.isa_gen = AGX_ISA_GEN3, .archive_gen = 2, .ift_gen = 1},
   [AGX_CHIP_G16X] = {.isa_gen = AGX_ISA_GEN3, .archive_gen = 2, .ift_gen = 1},

   /*
    * G17/M5 (HAL300): Gen3, archive gen 3, IFT gen 2. USC gen 3 is VERIFIED
    * only for G17X (the live G17S ioreg); G17G stays UNKNOWN. G17P (HAL200,
    * archive gen 2) is intentionally not a separate identity; no "gen >= 17".
    */
   [AGX_CHIP_G17G] = {.isa_gen = AGX_ISA_GEN3,
                      .archive_gen = 3,
                      /* usc_gen UNKNOWN: only the G17S/X part was captured */
                      .ift_gen = 2},
   [AGX_CHIP_G17X] = {.isa_gen = AGX_ISA_GEN3,
                      .archive_gen = 3,
                      .ift_gen = 2,
                      .usc_gen = 3},

   [AGX_CHIP_UNKNOWN] = {.isa_gen = AGX_ISA_GEN_UNKNOWN},
};

const struct agx_chip_info *
agx_chip_get_info(enum agx_chip chip)
{
   if ((unsigned)chip >= (sizeof(agx_chip_infos) / sizeof(agx_chip_infos[0])))
      return &agx_chip_infos[AGX_CHIP_UNKNOWN];

   return &agx_chip_infos[chip];
}

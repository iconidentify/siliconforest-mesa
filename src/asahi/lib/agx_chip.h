/*
 * Copyright 2026 The Asahi Linux Contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "drm-uapi/asahi_drm.h"
#include "libagx_dgc.h"

/*
 * Static, evidence-backed per-chip parameters. This is DATA and selectors only:
 * it never decides support (see agx_chip_supported()) and emits no command
 * stream, so it keeps every post-M2 chip fail-closed. Values are attached only
 * where grounded in captured evidence (static analysis of the macOS Metal
 * driver stack plus a live ioreg capture); anything not evidence-backed is left
 * at an UNKNOWN sentinel rather than guessed.
 */

/*
 * ISA / command-encoder generation. This is the single backend axis and is
 * independent of both the archive-metadata generation and the final hardware
 * capability tuple. VERIFIED (BINARY): G13/G14 instantiate
 * AGX::ESLInstructionEncoderGen2; G15/G16/G17 instantiate
 * AGX::ESLInstructionEncoderGen3.
 */
enum agx_isa_gen {
   AGX_ISA_GEN_UNKNOWN = 0,
   AGX_ISA_GEN2 = 2,
   AGX_ISA_GEN3 = 3,
};

/* A generation field that is not applicable (pre-M3) or not yet captured. */
#define AGX_GEN_UNKNOWN 0

/*
 * Per-chip static parameters we can justify from captured evidence.
 *
 * The archive-metadata generation (the Ria1/Ria2/Ria3 ladder) and the
 * intersection-function-table (IFT) generation are SEPARATE axes from ISA gen
 * and from the final hardware capability tuple; keep them distinct. Both are
 * AGX_GEN_UNKNOWN for the pre-M3 baseline, which predates the ladder.
 *
 * The G17P trap: G17P reports HAL200 and reuses archive generation 2, so it is
 * deliberately NOT an enum agx_chip identity. That is exactly why a
 * "generation >= N" dispatch is invalid -- this enum only carries the HAL300
 * G17G/G17X identities, which are archive generation 3.
 */
struct agx_chip_info {
   /* ISA/command-encoder generation. VERIFIED for every named identity. */
   enum agx_isa_gen isa_gen;

   /* Metallib archive-metadata generation, or AGX_GEN_UNKNOWN for pre-M3. */
   uint8_t archive_gen;

   /* Intersection-function-table generation, or AGX_GEN_UNKNOWN for pre-M3. */
   uint8_t ift_gen;

   /*
    * Unified Shader Core generation. Attached ONLY where captured: G17=3 is
    * VERIFIED from the live G17S ioreg (usc_gen=3). G13/G14/G15/G16 stay
    * AGX_GEN_UNKNOWN -- not captured, not guessed.
    */
   uint8_t usc_gen;
};

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

/*
 * Static, evidence-backed parameters for an identity. Never NULL; returns an
 * all-UNKNOWN descriptor for AGX_CHIP_UNKNOWN or any out-of-range value. Pure
 * data: it does not decide support, so callers must still gate on
 * agx_chip_supported().
 */
const struct agx_chip_info *
agx_chip_get_info(enum agx_chip chip);

#ifdef __cplusplus
}
#endif

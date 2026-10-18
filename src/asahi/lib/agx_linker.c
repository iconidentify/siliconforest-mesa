/*
 * Copyright 2024 Alyssa Rosenzweig
 * SPDX-License-Identifier: MIT
 */

#include "agx_linker.h"
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include "util/macros.h"
#include "util/ralloc.h"
#include "agx_abi.h"
#include "agx3_code_container_core.h"
#include "agx_compile.h"
#include "agx3_fltnorm_core.h"
#include "agx3_pixel_core.h"
#include "agx3_alu_core.h"
#include "agx_device.h"
#include "agx_pack.h"
#include "agx_scratch.h"

/*
 * When sample shading is used with a non-monolithic fragment shader, we
 * fast-link a program with the following structure:
 *
 *    Fragment prolog;
 *
 *    for (u16 sample_bit = 1; sample_bit < (1 << # of samples); ++sample_bit) {
 *       API fragment shader;
 *       Fragment epilog;
 *    }
 *
 * This means the prolog runs per-pixel but the fragment shader and epilog run
 * per-sample. To do this, we need to generate the loop on the fly. The
 * following binary sequences form the relevant loop.
 */

static_assert(AGX_ABI_FIN_SAMPLE_MASK == 2, "r1l known");

/* clang-format off */
static const uint8_t sample_loop_header[] = {
   /* mov_imm r0l, 0x0, 0b0 */
   0x62, 0x00, 0x00, 0x00,

   /* mov_imm r1l, 0x0, 0b0 */
   0x62, 0x04, 0x01, 0x00,
};

#define STOP                                                                   \
   /* stop */                                                                  \
   0x88, 0x00,                                                                 \
                                                                               \
   /* trap */                                                                  \
   0x08, 0x00, 0x08, 0x00, 0x08, 0x00, 0x08, 0x00,                             \
   0x08, 0x00, 0x08, 0x00, 0x08, 0x00, 0x08, 0x00,

static const uint8_t stop[] = {STOP};

static const uint8_t sample_loop_footer[] = {
   /* iadd r1l, 0, r1l, lsl 1 */
   0x0e, 0x04, 0x00, 0x20, 0x84, 0x00, 0x00, 0x00,

   /* while_icmp r0l, ult, r1h, 0, 1 */
   0x52, 0x2c, 0x42, 0x00, 0x00, 0x00,

   /* jmp_exec_any */
   0x00, 0xc0, 0x00, 0x00, 0x00, 0x00,

   /* pop_exec r0l, 1 */
   0x52, 0x0e, 0x00, 0x00, 0x00, 0x00,

   STOP
};

/* Offset in sample_loop_footer to the jmp_exec_any's target */
#define SAMPLE_LOOP_FOOTER_JMP_PATCH_OFFS (16)

/* Offset of the jmp_exec_any, for calculating the PC offsets */
#define SAMPLE_LOOP_FOOTER_JMP_OFFS (14)

/* Offset in sample_loop_footer to the while_icmp's sample count immediate. Bit
 * position in the byte given by the shift.
 */
#define SAMPLE_LOOP_FOOTER_COUNT_PATCH_OFFS (11)
#define SAMPLE_LOOP_FOOTER_COUNT_SHIFT (4)
/* clang-format on */


/*
 * Apple's AGX3 code container.
 *
 * On G17P the LDSHDR record names an entry point INSIDE a container, not the
 * base of an executable allocation, and the shader loader reads container
 * metadata at fixed negative offsets from that entry.  The m1n1 add3 witness
 * (proxyclient/experiments/g17p_add3_code.py) lays it out as:
 *
 *   +0x000  dword 0x340                 size of the leading block
 *   +0x040  0x0006 filler .. +0x340
 *   +0x100  helper record table
 *   +0x200  helper record table (identical)
 *   +0x340  dword block_size            == entry - 0x80, the CODE block size
 *   +0x380  constant program (0x40)     == entry - 0x40
 *   +0x3C0  main program                <- LDSHDR points here
 *
 * block_size = 0x40 (its own header) + 0x40 (constant program) +
 * align64(code).  For the witness's 56-byte main program that is
 * 0x40 + 0x40 + 0x40 = 0xC0, which is the dword the capture actually
 * contains, and the container ends at 0x340 + 0xC0 = 0x400 -- exactly the
 * 1 KiB closure the witness proved on hardware.  So this rule reproduces the
 * witness byte for byte rather than merely resembling it.
 *
 * Honeykrisp's compute path guarantees no preamble, so the constant program
 * is the witness's minimal terminating one.
 */
#define AGX3_CONTAINER_LEAD        0x340u
#define AGX3_CONTAINER_ENTRY       0x3C0u
#define AGX3_CONTAINER_BLOCK_HDR   0x40u
#define AGX3_CONTAINER_CONST_PROG  0x40u

/* "0f 00 54 <opcode> 00 x6 06 00 06 00 06 00" per record, then two terminals. */
static const uint8_t agx3_helper_terminal[16] = {
   0xf7, 0x03, 0xaa, 0x00, 0x8f, 0x02, 0x54, 0x01,
   0x06, 0x00, 0x06, 0x00, 0x06, 0x00, 0x06, 0x00,
};

static void
agx3_fill_helper_table(uint8_t *image, size_t base, unsigned positions)
{
   const uint8_t *terminal = agx3_helper_terminal;

   for (unsigned i = 0; i < positions; ++i) {
      uint8_t *record = image + base + i * 0x10;

      memset(record, 0, 0x10);
      record[0] = 0x0f;
      record[2] = 0x54;
      record[3] = (positions - i) * 16;
      for (unsigned h = 10; h < 16; h += 2) {
         record[h] = 0x06;
         record[h + 1] = 0x00;
      }
   }
   memcpy(image + base + positions * 16, terminal, 16);
   memcpy(image + base + (positions + 1) * 16, terminal, 16);
}

static void
agx3_put_le32(uint8_t *where, uint32_t value)
{
   where[0] = value;
   where[1] = value >> 8;
   where[2] = value >> 16;
   where[3] = value >> 24;
}

/* Total container bytes for `code_size` bytes of executable. */
static size_t
agx3_container_size(size_t code_size)
{
   return AGX3_CONTAINER_LEAD + AGX3_CONTAINER_BLOCK_HDR +
          AGX3_CONTAINER_CONST_PROG + ALIGN_POT(code_size, 64);
}

/* The container's leading block: dword 0x340, 0x0006 filler, two helper
 * tables. Identical in both captured Apple code images, so it is a fixed
 * prologue rather than anything workload-specific. */
void
agx3_build_container_lead_for_chip(uint8_t *image, enum agx_chip chip)
{
   /* G15S, like G16G, can enter ten helper slots under fragment pressure.
    * Six-slot tables leave the later entry points in padding; larger M3
    * renders then run past the lead block into the USC guard page.
    */
   const unsigned positions =
      agx_chip_uses_g15_g16_shader_abi(chip) ? 10 : 6;
   memset(image, 0, AGX3_CONTAINER_LEAD);
   agx3_put_le32(image, AGX3_CONTAINER_LEAD);
   for (size_t offset = 0x40; offset < AGX3_CONTAINER_LEAD; offset += 2) {
      image[offset] = 0x06;
      image[offset + 1] = 0x00;
   }
   agx3_fill_helper_table(image, 0x100, positions);
   agx3_fill_helper_table(image, 0x200, positions);
}

/* Put a helper terminal (a return) at every 16-byte slot in [from, to), so a
 * helper call that lands on one of them returns instead of executing
 * whatever the page happens to hold there.
 */
void
agx3_fill_helper_terminals(uint8_t *image, size_t from, size_t to)
{
   for (size_t offset = from; offset + 16 <= to; offset += 16)
      memcpy(image + offset, agx3_helper_terminal, 16);
}

void
agx3_build_container_lead(uint8_t *image)
{
   agx3_build_container_lead_for_chip(image, AGX_CHIP_G17P);
}

static void
agx3_build_container(uint8_t *image, size_t code_size, bool fragment, enum agx_chip chip)
{
   memset(image, 0, agx3_container_size(code_size));
   agx3_build_container_lead_for_chip(image, chip);

   agx3_put_le32(image + AGX3_CONTAINER_LEAD,
                 AGX3_CONTAINER_BLOCK_HDR + AGX3_CONTAINER_CONST_PROG +
                    ALIGN_POT(code_size, 64));

   uint8_t *constant = image + AGX3_CONTAINER_LEAD + AGX3_CONTAINER_BLOCK_HDR;
   agx3_build_container_constant_program(constant, fragment);
}

static bool
agx3_uses_code_container(const struct agx_device *dev)
{
   return agx_chip_uses_hal200_ioto(dev->chip);
}

enum agx_link_result
agx_fast_link_checked(struct agx_linked_shader *linked, struct agx_device *dev,
              bool fragment, struct agx_shader_part *main,
              struct agx_shader_part *prolog, struct agx_shader_part *epilog,
              unsigned nr_samples_shaded)
{
   /* Link one native invocation for each rasterization sample. The sample
    * count is draw-time state; retain separate main/epilog compilation and
    * unroll the small hardware-supported count when linking their binaries. */
   const bool native_samples = agx_native_graphics(dev) && nr_samples_shaded;
   uint8_t sample_setup[4][AGX3_ALU_MAX_LENGTH];
   uint8_t sample_setup_size[4] = {0};
   uint8_t sample_release[AGX3_PIXEL_MAX_LENGTH], sample_release_size = 0;
   uint8_t sample_entry[AGX3_PIXEL_MAX_LENGTH], sample_entry_size = 0;
   uint8_t sample_wait[AGX3_PIXEL_MAX_LENGTH], sample_wait_size = 0;
   uint8_t sample_zs_wait[AGX3_PIXEL_MAX_LENGTH], sample_zs_wait_size = 0;
   if (native_samples) {
      if (!fragment || !agx_chip_uses_g15_g16_shader_abi(dev->chip) ||
          (nr_samples_shaded != 1 && nr_samples_shaded != 2 &&
           nr_samples_shaded != 4)) {
         fprintf(stderr, "agx_fast_link: native sample loop rejected chip=%u samples=%u fragment=%u\n",
                 dev->chip, nr_samples_shaded, fragment);
         return AGX_LINK_INVALID;
      }
      if (!agx3_pixel_release(AGX_ISA_ENCODING_AGX3B, 0, true, 0,
                             sample_release, &sample_release_size))
         return AGX_LINK_INVALID;
      for (unsigned sample = 0; sample < nr_samples_shaded; ++sample) {
         if (!agx3_alu_mov16(AGX_ISA_ENCODING_AGX3B, AGX_ABI_FIN_SAMPLE_MASK,
                            1u << sample, 0, sample_setup[sample],
                            &sample_setup_size[sample]))
            return AGX_LINK_INVALID;
      }
   }

   uint8_t fused_colour[AGX3_COLOUR_LINK_SIZE];
   const bool colour_fused = agx_native_graphics(dev) && fragment &&
      !prolog && !nr_samples_shaded && main && epilog &&
      agx3_fuse_colour_parts(&main->info.agx3_colour_link,
                              &epilog->info.agx3_colour_link, fused_colour);
   const bool dedup_entry = agx_native_graphics(dev) && fragment &&
      !prolog && !nr_samples_shaded && !colour_fused && main && epilog &&
      main->info.agx3_colour_link.entry_emitted &&
      epilog->info.agx3_colour_link.entry_emitted &&
      epilog->info.agx3_colour_link.straight_line;
   if (dedup_entry) {
      const uint32_t off = epilog->info.agx3_colour_link.entry_offset;
      uint8_t gate[AGX3_PIXEL_MAX_LENGTH], gate_size;
      if ((off & 1) || off > epilog->info.main_size ||
          epilog->info.main_size - off < AGX3_PIXEL_MAX_LENGTH ||
          !agx3_pixel_colour_entry(AGX_ISA_ENCODING_AGX3B, gate, &gate_size) ||
          gate_size != AGX3_PIXEL_MAX_LENGTH ||
          memcmp((const uint8_t *)epilog->binary + epilog->info.main_offset + off,
                 gate, gate_size))
         return AGX_LINK_INVALID;
   }
   size_t size = 0;
   unsigned nr_gprs = 0, scratch_size = 0;
   unsigned staging_halfs = 0;
   bool staging_known = true;
   bool reads_tib = false, writes_sample_mask = false,
        disable_tri_merging = false, tag_write_disable = true;

   if (nr_samples_shaded && !native_samples) {
      size += sizeof(sample_loop_header);

      if (nr_samples_shaded > 1)
         size += sizeof(sample_loop_footer);
      else
         size += sizeof(stop);
   }

   struct agx_shader_part *parts[] = {prolog, main, epilog};

   for (unsigned i = 0; i < ARRAY_SIZE(parts); ++i) {
      struct agx_shader_part *part = parts[i];
      if (!part)
         continue;

      size += (size_t)part->info.main_size *
              (native_samples && i > 0 ? nr_samples_shaded : 1);

      nr_gprs = MAX2(nr_gprs, part->info.nr_gprs);
      staging_halfs = MAX2(staging_halfs, part->info.agx3_staging_halfs);
      staging_known &= part->info.agx3_staging_known;
      scratch_size = MAX2(scratch_size, part->info.scratch_size);
      reads_tib |= part->info.reads_tib;
      writes_sample_mask |= part->info.writes_sample_mask;
      disable_tri_merging |= part->info.disable_tri_merging;
      linked->uses_base_param |= part->info.uses_base_param;
      linked->uses_txf |= part->info.uses_txf;
      tag_write_disable &= part->info.tag_write_disable;
   }

   if (native_samples) {
      if (reads_tib || !tag_write_disable) {
         /* Apple's native MSAA fragment holds one class-12 bracket around
          * all sample stores, then releases it once with the final bit. */
         if (!(prolog && prolog->info.agx3_colour_link.entry_emitted) &&
             !agx3_pixel_colour_entry(AGX_ISA_ENCODING_AGX3B,
                                     sample_entry, &sample_entry_size))
            return AGX_LINK_INVALID;
         if (!agx3_pixel_wait(AGX_ISA_ENCODING_AGX3B, 12, 0,
                             sample_wait, &sample_wait_size) ||
             !agx3_pixel_release(AGX_ISA_ENCODING_AGX3B, 12, true, 0,
                                sample_release, &sample_release_size))
            return AGX_LINK_INVALID;
      }
      /* Native sample-discard/DEPTHF loops acquire class1 before iterating.
       * Releasing it after the first sample loses later stencil updates. */
      if (writes_sample_mask &&
          !agx3_pixel_wait(AGX_ISA_ENCODING_AGX3B, 1, 0,
                           sample_zs_wait, &sample_zs_wait_size))
         return AGX_LINK_INVALID;
      size += sample_entry_size + sample_wait_size + sample_zs_wait_size;
      for (unsigned sample = 0; sample < nr_samples_shaded; ++sample)
         size += sample_setup_size[sample];
      size += sample_release_size + 4; /* Final release and native END. */
   }

   if (colour_fused)
      size = sizeof(fused_colour);
   else if (dedup_entry)
      size -= AGX3_PIXEL_MAX_LENGTH;

   assert(size > 0 && "must stop");

   if (agx_native_graphics(dev) &&
       (main->info.stage == MESA_SHADER_VERTEX ||
        main->info.stage == MESA_SHADER_FRAGMENT) &&
       staging_known && !linked->g17p_compiler_d_valid) {
      linked->g17p_compiler_d_valid = true;
      linked->g17p_compiler_d = staging_halfs * 2;
   }

   const bool container = agx3_uses_code_container(dev);
   const size_t alloc_size = container ? agx3_container_size(size) : size;

   linked->bo = agx_bo_create(dev, alloc_size, 0, AGX_BO_EXEC | AGX_BO_LOW_VA,
                              "Linked executable");
   linked->size = size;
   linked->entry_offset = container ? AGX3_CONTAINER_ENTRY : 0;
   if (!linked->bo) {
      fprintf(stderr,
              "agx_fast_link: executable low-VA allocation of %zu bytes "
              "failed (shader base 0x%" PRIx64 ")\n",
              size, dev->shader_base);
      return AGX_LINK_OUT_OF_DEVICE_MEMORY;
   }

   uint8_t *container_map = agx_bo_map(linked->bo);
   uint8_t *linked_map = container_map ? container_map + linked->entry_offset : NULL;
   if (!linked_map) {
      fprintf(stderr,
              "agx_fast_link: executable BO mapping failed (size %zu, "
              "VA 0x%" PRIx64 ", handle %u)\n",
              size, linked->bo->va ? linked->bo->va->addr : 0,
              (unsigned)linked->bo->handle);
      agx_bo_unreference(dev, linked->bo);
      linked->bo = NULL;
      return AGX_LINK_OUT_OF_HOST_MEMORY;
   }

   if (container)
      agx3_build_container(container_map, size, fragment, dev->chip);

   size_t offset = 0;
   if (colour_fused) {
      memcpy(linked_map, fused_colour, sizeof(fused_colour));
      offset = sizeof(fused_colour);
   }

   /* FS prolog happens per-pixel, outside the sample loop */
   if (prolog) {
      size_t sz = prolog->info.main_size;
      memcpy(linked_map + offset, prolog->binary, sz);
      if (agx_native_graphics(dev) && fragment &&
          !agx3_fltnorm_relocate_start(linked_map + offset, sz,
                                       prolog->info.agx3_fltnorm_start, offset))
         goto invalid_start_marker;
      offset += sz;
   }

   if (nr_samples_shaded && !native_samples) {
      memcpy(linked_map + offset, sample_loop_header,
             sizeof(sample_loop_header));
      offset += sizeof(sample_loop_header);
   }

   size_t sample_loop_begin = offset;

   if (native_samples) {
      memcpy(linked_map + offset, sample_entry, sample_entry_size);
      offset += sample_entry_size;
      memcpy(linked_map + offset, sample_zs_wait, sample_zs_wait_size);
      offset += sample_zs_wait_size;
      memcpy(linked_map + offset, sample_wait, sample_wait_size);
      offset += sample_wait_size;
   }
   bool native_entry_seen = sample_entry_size ||
      (prolog && prolog->info.agx3_colour_link.entry_emitted);
   for (unsigned sample = 0;
        sample < (native_samples ? nr_samples_shaded : 1); ++sample) {
      if (native_samples) {
         memcpy(linked_map + offset, sample_setup[sample],
                sample_setup_size[sample]);
         offset += sample_setup_size[sample];
      }

      /* Main shader and epilog happen in the sample loop, so start from i=1 */
      for (unsigned i = 1; !colour_fused && i < ARRAY_SIZE(parts); ++i) {
         struct agx_shader_part *part = parts[i];
         if (!part)
            continue;

         size_t sz = part->info.main_size;
         const uint8_t *part_code = (const uint8_t *)part->binary + part->info.main_offset;
         if (dedup_entry && i == 2) {
            const unsigned gate = part->info.agx3_colour_link.entry_offset;
            memcpy(linked_map + offset, part_code, gate);
            memcpy(linked_map + offset + gate, part_code + gate + AGX3_PIXEL_MAX_LENGTH,
                   sz - gate - AGX3_PIXEL_MAX_LENGTH);
            sz -= AGX3_PIXEL_MAX_LENGTH;
         } else {
            memcpy(linked_map + offset, part_code, sz);
         }
         if (native_samples && part->info.agx3_colour_link.entry_emitted) {
            unsigned gate = part->info.agx3_colour_link.entry_offset;
            if (gate > sz || sz - gate < AGX3_PIXEL_MAX_LENGTH)
               goto invalid_start_marker;
            if (native_entry_seen) {
               /* Preserve all intra-part branch offsets while acquiring the
                * shader-entry gate only once across the linked invocations. */
               for (unsigned j = 0; j < AGX3_PIXEL_MAX_LENGTH; j += 2) {
                  linked_map[offset + gate + j] = 0x06;
                  linked_map[offset + gate + j + 1] = 0;
               }
            }
            native_entry_seen = true;
         }
         if (agx_native_graphics(dev) && fragment &&
             !agx3_fltnorm_relocate_start(linked_map + offset, sz,
                                          part->info.agx3_fltnorm_start, offset))
            goto invalid_start_marker;
         offset += sz;
      }

   }

   if (native_samples) {
      memcpy(linked_map + offset, sample_release, sample_release_size);
      offset += sample_release_size;
      agx3_put_le32(linked_map + offset, 0x0e);
      offset += 4;
   } else if (nr_samples_shaded > 1) {
      assert(sample_loop_footer[SAMPLE_LOOP_FOOTER_COUNT_PATCH_OFFS] == 0);

      /* Make a stack copy of the footer so we can efficiently patch it */
      uint8_t footer[sizeof(sample_loop_footer)];
      memcpy(footer, sample_loop_footer, sizeof(footer));

      /* Patch in sample end */
      uint8_t end = (1u << nr_samples_shaded) - 1;
      footer[SAMPLE_LOOP_FOOTER_COUNT_PATCH_OFFS] =
         end << SAMPLE_LOOP_FOOTER_COUNT_SHIFT;

      /* Patch in the branch target */
      int32_t loop_size = offset - sample_loop_begin;
      int32_t branch_offs = -(SAMPLE_LOOP_FOOTER_JMP_OFFS + loop_size);
      int32_t *target = (int32_t *)(footer + SAMPLE_LOOP_FOOTER_JMP_PATCH_OFFS);
      *target = branch_offs;

      /* Copy in the patched footer */
      memcpy(linked_map + offset, footer, sizeof(footer));
      offset += sizeof(footer);
   } else if (nr_samples_shaded) {
      /* Just end after the first sample, no need to loop for a single sample */
      memcpy(linked_map + offset, stop, sizeof(stop));
      offset += sizeof(stop);
   }

   assert(offset == size);

   if (fragment && getenv("AGX_NATIVE_LINK_DIAGNOSTICS")) {
      fprintf(stderr, "Native sample link: samples=%u size=%zu main=%u epilog=%u gprs=%u staging=%u hex=",
              nr_samples_shaded, offset, main ? main->info.main_size : 0,
              epilog ? epilog->info.main_size : 0, nr_gprs, staging_halfs);
      for (size_t i = 0; i < offset; ++i)
         fprintf(stderr, "%02x", linked_map[i]);
      fprintf(stderr, "\n");
   }

   linked->scratch_size = scratch_size;
   linked->nr_gprs = nr_gprs;

   if (agx_bo_sync_for_device(linked->bo, 0, alloc_size)) {
      agx_bo_unreference(dev, linked->bo);
      linked->bo = NULL;
      return AGX_LINK_UPLOAD_FAILED;
   }

   agx_pack(&linked->shader, USC_SHADER, cfg) {
      cfg.code = agx_usc_addr(dev, linked->bo->va->addr + linked->entry_offset);
      cfg.unk_2 = fragment ? 2 : 3;

      if (fragment)
         cfg.loads_varyings = linked->cf.nr_bindings > 0;
   }

   agx_pack(&linked->regs, USC_REGISTERS, cfg) {
      cfg.register_count = nr_gprs;
      cfg.unk_1 = fragment;
      cfg.spill_size = scratch_size ? agx_scratch_get_bucket(scratch_size) : 0;
      cfg.unk_4 = 1;
   }

   if (fragment) {
      agx_pack(&linked->fragment_props, USC_FRAGMENT_PROPERTIES, cfg) {
         cfg.early_z_testing = !writes_sample_mask;
         cfg.unk_2 = true;
         cfg.unk_3 = 0xf;
         cfg.unk_4 = 0x2;
         cfg.unk_5 = 0x0;
      }

      /* If we skip the fragment shader, we can optimize the pass type */
      if (tag_write_disable) {
         reads_tib = false;
         writes_sample_mask = false;
         linked->no_op = true;
      }

      agx_pack(&linked->fragment_control, FRAGMENT_CONTROL, cfg) {
         cfg.tag_write_disable = tag_write_disable;
         cfg.disable_tri_merging = disable_tri_merging;

         if (reads_tib && writes_sample_mask)
            cfg.pass_type = AGX_PASS_TYPE_TRANSLUCENT_PUNCH_THROUGH;
         else if (reads_tib)
            cfg.pass_type = AGX_PASS_TYPE_TRANSLUCENT;
         else if (writes_sample_mask)
            cfg.pass_type = AGX_PASS_TYPE_PUNCH_THROUGH;
         else
            cfg.pass_type = AGX_PASS_TYPE_OPAQUE;
      }

      /* Merge the CF binding lists from the prolog to handle cull distance */
      memcpy(&linked->cf, &main->info.varyings.fs,
             sizeof(struct agx_varyings_fs));

      struct agx_varyings_fs *prolog_vary =
         prolog ? &prolog->info.varyings.fs : NULL;

      if (prolog_vary && prolog_vary->nr_bindings) {
         assert(!prolog_vary->reads_z);
         linked->cf.nr_cf = MAX2(linked->cf.nr_cf, prolog_vary->nr_cf);

         assert(linked->cf.nr_bindings + prolog_vary->nr_bindings <=
                   ARRAY_SIZE(linked->cf.bindings) &&
                "bounded by # of coeff registers");

         memcpy(linked->cf.bindings + linked->cf.nr_bindings,
                prolog_vary->bindings,
                sizeof(struct agx_cf_binding) * prolog_vary->nr_bindings);

         linked->cf.nr_bindings += prolog_vary->nr_bindings;
      }

      agx_pack(&linked->osel, OUTPUT_SELECT, cfg) {
         cfg.varyings = linked->cf.nr_bindings > 0;
         cfg.frag_coord_z = linked->cf.reads_z;
      }
   }

   return AGX_LINK_SUCCESS;

invalid_start_marker:
   fprintf(stderr, "agx_fast_link: invalid G17P part-start FLTNORM marker\n");
   agx_bo_unreference(dev, linked->bo);
   linked->bo = NULL;
   return AGX_LINK_INVALID;
}

bool
agx_fast_link(struct agx_linked_shader *linked, struct agx_device *dev,
              bool fragment, struct agx_shader_part *main,
              struct agx_shader_part *prolog, struct agx_shader_part *epilog,
              unsigned nr_samples_shaded)
{
   return agx_fast_link_checked(linked, dev, fragment, main, prolog, epilog,
                                nr_samples_shaded) == AGX_LINK_SUCCESS;
}

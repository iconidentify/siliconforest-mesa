/*
 * Copyright 2021 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

#include "agx_test.h"

#include <gtest/gtest.h>

#define CASE(instr, expected)                                                  \
   INSTRUCTION_CASE(instr, expected, agx_lower_pseudo)
#define NEGCASE(instr) CASE(instr, instr)

class LowerPseudo : public testing::Test {
 protected:
   LowerPseudo()
   {
      mem_ctx = ralloc_context(NULL);

      wx = agx_register(0, AGX_SIZE_32);
      wy = agx_register(2, AGX_SIZE_32);
      wz = agx_register(4, AGX_SIZE_32);
   }

   ~LowerPseudo()
   {
      ralloc_free(mem_ctx);
   }

   void *mem_ctx;
   agx_index wx, wy, wz;
};

TEST_F(LowerPseudo, Move)
{
   CASE(agx_mov_to(b, wx, wy), agx_bitop_to(b, wx, wy, agx_zero(), 0xA));
}

TEST_F(LowerPseudo, UnalignedMove32)
{
   agx_index odd_dst = agx_register(11, AGX_SIZE_32);
   agx_index odd_src = agx_register(3, AGX_SIZE_32);

   CASE(agx_mov_to(b, odd_dst, odd_src), {
      agx_bitop_to(b, agx_register(11, AGX_SIZE_16),
                   agx_register(3, AGX_SIZE_16), agx_zero(), 0xA);
      agx_bitop_to(b, agx_register(12, AGX_SIZE_16),
                   agx_register(4, AGX_SIZE_16), agx_zero(), 0xA);
   });
}

TEST_F(LowerPseudo, OverlappingUnalignedMove32)
{
   CASE(agx_mov_to(b, agx_register(3, AGX_SIZE_32),
                   agx_register(2, AGX_SIZE_32)), {
      agx_bitop_to(b, agx_register(4, AGX_SIZE_16),
                   agx_register(3, AGX_SIZE_16), agx_zero(), 0xA);
      agx_bitop_to(b, agx_register(3, AGX_SIZE_16),
                   agx_register(2, AGX_SIZE_16), agx_zero(), 0xA);
   });
}

TEST_F(LowerPseudo, ReverseOverlappingUnalignedMove32)
{
   CASE(agx_mov_to(b, agx_register(2, AGX_SIZE_32),
                   agx_register(3, AGX_SIZE_32)), {
      agx_bitop_to(b, agx_register(2, AGX_SIZE_16),
                   agx_register(3, AGX_SIZE_16), agx_zero(), 0xA);
      agx_bitop_to(b, agx_register(3, AGX_SIZE_16),
                   agx_register(4, AGX_SIZE_16), agx_zero(), 0xA);
   });
}

TEST_F(LowerPseudo, UnalignedStackMove32)
{
   CASE(agx_mov_to(b, agx_memory_register(3, AGX_SIZE_32),
                   agx_memory_register(2, AGX_SIZE_32)), {
      agx_bitop_to(b, agx_memory_register(4, AGX_SIZE_16),
                   agx_memory_register(3, AGX_SIZE_16), agx_zero(), 0xA);
      agx_bitop_to(b, agx_memory_register(3, AGX_SIZE_16),
                   agx_memory_register(2, AGX_SIZE_16), agx_zero(), 0xA);
   });
}

TEST_F(LowerPseudo, UnalignedDestinationFromUniform32)
{
   CASE(agx_mov_to(b, agx_register(3, AGX_SIZE_32),
                   agx_uniform(5, AGX_SIZE_32)), {
      agx_bitop_to(b, agx_register(3, AGX_SIZE_16),
                   agx_uniform(5, AGX_SIZE_16), agx_zero(), 0xA);
      agx_bitop_to(b, agx_register(4, AGX_SIZE_16),
                   agx_uniform(6, AGX_SIZE_16), agx_zero(), 0xA);
   });
}

TEST_F(LowerPseudo, WideningMoveIsUnchanged)
{
   agx_index dst = agx_register(3, AGX_SIZE_32);
   agx_index src = agx_register(5, AGX_SIZE_16);

   CASE(agx_mov_to(b, dst, src),
        agx_bitop_to(b, dst, src, agx_zero(), 0xA));
}

TEST_F(LowerPseudo, Not)
{
   CASE(agx_not_to(b, wx, wy), agx_bitop_to(b, wx, wy, agx_zero(), 0x5));
}

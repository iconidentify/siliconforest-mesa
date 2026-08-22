/*
 * Copyright 2025 Valve Corporation
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include "util/bitset.h"

signed agx2_disassemble_instr(BITSET_WORD *code, FILE *fp, unsigned offset,
                              size_t available, bool verbose);

static inline bool
agx2_disassemble(uint8_t *code, size_t max_len, FILE *fp)
{
   int i = 0;
   bool errors = false;

   while (i < max_len) {
      size_t available = max_len - i;

      if (available < 2) {
         fprintf(fp, "XXX truncated instruction\n");
         return true;
      }

      /* Break on trap */
      if (code[i + 0] == 0x08 && code[i + 1] == 0x00)
         break;

      signed ret = agx2_disassemble_instr((BITSET_WORD *)(code + i), fp, i,
                                          available, true);
      if (ret < 0)
         fprintf(fp, "XXX error here\n");

      errors |= ret < 0;
      unsigned length = abs(ret);
      if (length == 0 || length > available)
         return true;
      i += length;
   }

   return errors;
}

/*
 * Copyright 2024 Valve Corporation
 * SPDX-License-Identifier: MIT
 */

#include "agx_bo.h"
#include "agx_device.h"

/* Shared-VM runtime-device bridge: present only in the D3DKMT build (its
 * sources are overlaid into this directory), so this is inert in the AGXKMD
 * build and on Linux. */
#if defined(_WIN32) && defined(__has_include)
#  if __has_include("agx_bridge_vma.h")
#    include "agx_bridge_vma.h"
#    define HK_HAVE_BRIDGE_VMA 1
#  endif
#endif

static struct util_vma_heap *
agx_vma_heap(struct agx_device *dev, enum agx_va_flags flags)
{
   return (flags & AGX_VA_USC) ? &dev->usc_heap : &dev->main_heap;
}

struct agx_va *
agx_va_alloc(struct agx_device *dev, uint64_t size_B, uint64_t align_B,
             enum agx_va_flags flags, uint64_t fixed_va)
{
#ifdef _WIN32
   const uint64_t d3dkmt_granularity = UINT64_C(0x10000);
   if (size_B > UINT64_MAX - (d3dkmt_granularity - 1))
      return NULL;
   size_B = ALIGN_POT(size_B, d3dkmt_granularity);
   align_B = MAX2(align_B, d3dkmt_granularity);
#endif
   assert((fixed_va != 0) == !!(flags & AGX_VA_FIXED));
   if ((fixed_va % align_B) != 0)
      return NULL;

   /* All allocations need a guard at the end to prevent overreads.
    *
    * TODO: Even with soft fault?
    */
   size_B += dev->guard_size;

   uint64_t addr = 0;

#ifdef HK_HAVE_BRIDGE_VMA
   if (agx_bridge_vma_registered(dev)) {
      /* One VA allocator for every device sharing this process/adapter root:
       * dynamic allocations are globally disjoint, and exact fixed claims
       * fail on collision because VA equality does not prove BO identity. */
      addr = agx_bridge_vma_alloc(dev, size_B, align_B, flags, fixed_va);
   } else
#endif
   {
      struct util_vma_heap *heap = agx_vma_heap(dev, flags);
      simple_mtx_lock(&dev->vma_lock);
      if (flags & AGX_VA_FIXED) {
         if (util_vma_heap_alloc_addr(heap, fixed_va, size_B))
            addr = fixed_va;
      } else {
         addr = util_vma_heap_alloc(heap, size_B, align_B);
      }
      simple_mtx_unlock(&dev->vma_lock);
   }

   if (addr == 0)
      return NULL;

   struct agx_va *va = malloc(sizeof(struct agx_va));
   if (va == NULL) {
      /* Unchecked since upstream; under memory pressure the write below was
       * a NULL dereference inside the allocation path. Return the VA to its
       * heap and fail the allocation cleanly (agx-va-malloc hardening). */
#ifdef HK_HAVE_BRIDGE_VMA
      if (agx_bridge_vma_registered(dev))
         agx_bridge_vma_free(dev, addr, size_B, flags);
      else
#endif
      {
         struct util_vma_heap *heap = agx_vma_heap(dev, flags);
         simple_mtx_lock(&dev->vma_lock);
         util_vma_heap_free(heap, addr, size_B);
         simple_mtx_unlock(&dev->vma_lock);
      }
      return NULL;
   }
   *va = (struct agx_va){
      .flags = flags,
      .size_B = size_B,
      .addr = addr,
   };
   return va;
}

void
agx_va_free(struct agx_device *dev, struct agx_va *va, bool unbind)
{
   if (!va)
      return;

   if (unbind) {
      agx_bo_bind(dev, NULL, va->addr, va->size_B, 0, DRM_ASAHI_BIND_UNBIND);
   }

#ifdef HK_HAVE_BRIDGE_VMA
   if (agx_bridge_vma_registered(dev)) {
      agx_bridge_vma_free(dev, va->addr, va->size_B, va->flags);
   } else
#endif
   {
      struct util_vma_heap *heap = agx_vma_heap(dev, va->flags);
      simple_mtx_lock(&dev->vma_lock);
      util_vma_heap_free(heap, va->addr, va->size_B);
      simple_mtx_unlock(&dev->vma_lock);
   }
   free(va);
}

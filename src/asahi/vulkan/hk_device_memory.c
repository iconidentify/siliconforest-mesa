/*
 * Copyright 2024 Valve Corporation
 * Copyright 2024 Alyssa Rosenzweig
 * Copyright 2022-2023 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */
#include "hk_device_memory.h"

#include "hk_device.h"
#include "hk_entrypoints.h"
#include "hk_image.h"
#include "hk_physical_device.h"

#include "asahi/lib/agx_bo.h"
#include "util/u_atomic.h"

#ifdef _WIN32
#include <windows.h>
#include "asahi/lib/agx_device_d3dkmt.h"

/* Runtime-device bridge hooks (__has_include keeps AGXKMD/Linux
 * byte-identical). Provides agx_bridge_vma_registered() -- the per-device
 * "bridge is live" signal -- and agx_bridge_valog(). The functional
 * runtime-resource consumer below and the optional failure breadcrumbs both
 * gate on HK_HAVE_BRIDGE_HOOKS. */
#if defined(__has_include)
#if __has_include("asahi/lib/agx_bridge_vma.h")
#include "asahi/lib/agx_bridge_vma.h"
#define HK_HAVE_BRIDGE_HOOKS 1
#endif
#endif
#endif

#include <inttypes.h>
#ifndef _WIN32
#include <sys/mman.h>
#endif

/* Supports opaque fd only */
const VkExternalMemoryProperties hk_opaque_fd_mem_props = {
   .externalMemoryFeatures = VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT |
                             VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT,
   .exportFromImportedHandleTypes =
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
   .compatibleHandleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
};

/* Supports opaque fd and dma_buf. */
const VkExternalMemoryProperties hk_dma_buf_mem_props = {
   .externalMemoryFeatures = VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT |
                             VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT,
   .exportFromImportedHandleTypes =
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT |
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   .compatibleHandleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT |
                            VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
};

#ifdef _WIN32
/* The NT and KMT forms deliberately are not mutually compatible. Creating an
 * NT-security shared resource does not mint hGlobalShare, while the legacy KMT
 * form is non-owning. Advertising them as compatible would allow an export
 * allocation that no D3DKMT_CREATEALLOCATION flag set can satisfy. */
const VkExternalMemoryProperties hk_opaque_win32_mem_props = {
   .externalMemoryFeatures = VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT |
                             VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT,
   .exportFromImportedHandleTypes =
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT,
   .compatibleHandleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT,
};

const VkExternalMemoryProperties hk_opaque_win32_kmt_mem_props = {
   .externalMemoryFeatures = VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT |
                             VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT,
   .exportFromImportedHandleTypes =
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT,
   .compatibleHandleTypes =
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT,
};

static enum agx_d3dkmt_external_memory_type
hk_d3dkmt_external_type(VkExternalMemoryHandleTypeFlags handle_types)
{
   switch (handle_types) {
   case VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT:
      return AGX_D3DKMT_EXTERNAL_MEMORY_OPAQUE_WIN32;
   case VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT:
      return AGX_D3DKMT_EXTERNAL_MEMORY_OPAQUE_WIN32_KMT;
   default:
      return AGX_D3DKMT_EXTERNAL_MEMORY_NONE;
   }
}
#endif

static enum agx_bo_flags
hk_memory_type_flags(const VkMemoryType *type,
                     VkExternalMemoryHandleTypeFlagBits handle_types)
{
   unsigned flags = 0;

   if (handle_types)
      flags |= AGX_BO_SHARED | AGX_BO_SHAREABLE;

   return flags;
}

/*
 * honeykrisp-h1-execution.md SS2.2 row P9 (same rationale, this file's
 * analogue): this bookkeeping is virtio-gpu-only (guarded at every call
 * site by `dev->dev.is_virtio`, which is unconditionally false on Windows
 * -- agx_win_open_device() always sets it), but the bodies below name
 * `struct asahi_ccmd_submit_res` BY VALUE, which needs the complete type
 * (agx_device.h forward-declares it only, SS2.1 row D2, when !HAVE_LIBDRM).
 * Gate on the same HAVE_VDRM define P9 introduces (src/asahi/lib/meson.build,
 * defined for Linux builds only) so there is exactly one virtio-availability
 * define, not two.
 */
#ifdef HAVE_VDRM
static void
hk_add_ext_bo_locked(struct hk_device *dev, struct agx_bo *bo)
{
   uint32_t id = bo->uapi_handle;

   unsigned count = util_dynarray_num_elements(&dev->external_bos.list,
                                               struct asahi_ccmd_submit_res);

   for (unsigned i = 0; i < count; i++) {
      struct asahi_ccmd_submit_res *p = util_dynarray_element(
         &dev->external_bos.list, struct asahi_ccmd_submit_res, i);

      if (p->res_id == id) {
         ++*util_dynarray_element(&dev->external_bos.counts, unsigned, i);
         return;
      }
   }

   struct asahi_ccmd_submit_res res = {
      .res_id = id,
      .flags = ASAHI_EXTRES_READ | ASAHI_EXTRES_WRITE,
   };
   util_dynarray_append(&dev->external_bos.list, res);
   util_dynarray_append_typed(&dev->external_bos.counts, unsigned, 1);
}

static void
hk_add_ext_bo(struct hk_device *dev, struct agx_bo *bo)
{
   if (dev->dev.is_virtio) {
      u_rwlock_wrlock(&dev->external_bos.lock);
      hk_add_ext_bo_locked(dev, bo);
      u_rwlock_wrunlock(&dev->external_bos.lock);
   }
}

static void
hk_remove_ext_bo_locked(struct hk_device *dev, struct agx_bo *bo)
{
   uint32_t id = bo->uapi_handle;
   unsigned count = util_dynarray_num_elements(&dev->external_bos.list,
                                               struct asahi_ccmd_submit_res);

   for (unsigned i = 0; i < count; i++) {
      struct asahi_ccmd_submit_res *p = util_dynarray_element(
         &dev->external_bos.list, struct asahi_ccmd_submit_res, i);

      if (p->res_id == id) {
         unsigned *ctr =
            util_dynarray_element(&dev->external_bos.counts, unsigned, i);
         if (!--*ctr) {
            *ctr = util_dynarray_pop(&dev->external_bos.counts, unsigned);
            *p = util_dynarray_pop(&dev->external_bos.list,
                                   struct asahi_ccmd_submit_res);
         }
         return;
      }
   }

   UNREACHABLE("BO not found");
}

static void
hk_remove_ext_bo(struct hk_device *dev, struct agx_bo *bo)
{
   if (dev->dev.is_virtio) {
      u_rwlock_wrlock(&dev->external_bos.lock);
      hk_remove_ext_bo_locked(dev, bo);
      u_rwlock_wrunlock(&dev->external_bos.lock);
   }
}
#else
static inline void
hk_add_ext_bo(struct hk_device *dev, struct agx_bo *bo)
{
   /* No virtio on Windows (HAVE_VDRM unset) -- dev->dev.is_virtio is always
    * false, so the Linux body above is always a no-op here too. */
}

static inline void
hk_remove_ext_bo(struct hk_device *dev, struct agx_bo *bo)
{
}
#endif /* HAVE_VDRM */

VKAPI_ATTR VkResult VKAPI_CALL
hk_GetMemoryFdPropertiesKHR(VkDevice device,
                            VkExternalMemoryHandleTypeFlagBits handleType,
                            int fd,
                            VkMemoryFdPropertiesKHR *pMemoryFdProperties)
{
   VK_FROM_HANDLE(hk_device, dev, device);
   struct hk_physical_device *pdev = hk_device_physical(dev);
   struct agx_bo *bo;

   switch (handleType) {
   case VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT:
   case VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT:
      bo = agx_bo_import(&dev->dev, fd);
      if (bo == NULL)
         return vk_error(dev, VK_ERROR_INVALID_EXTERNAL_HANDLE);
      break;
   default:
      return vk_error(dev, VK_ERROR_INVALID_EXTERNAL_HANDLE);
   }

   uint32_t type_bits = 0;
   for (unsigned t = 0; t < ARRAY_SIZE(pdev->mem_types); t++) {
      const unsigned flags =
         hk_memory_type_flags(&pdev->mem_types[t], handleType);
      if (!(flags & ~bo->flags))
         type_bits |= (1 << t);
   }

   pMemoryFdProperties->memoryTypeBits = type_bits;

   agx_bo_unreference(&dev->dev, bo);

   return VK_SUCCESS;
}

#ifdef _WIN32
VKAPI_ATTR VkResult VKAPI_CALL
hk_GetMemoryWin32HandlePropertiesKHR(
   VkDevice device, VkExternalMemoryHandleTypeFlagBits handleType,
   HANDLE handle, VkMemoryWin32HandlePropertiesKHR *pProperties)
{
   VK_FROM_HANDLE(hk_device, dev, device);
   struct hk_physical_device *pdev = hk_device_physical(dev);
   const enum agx_d3dkmt_external_memory_type external_type =
      hk_d3dkmt_external_type(handleType);
   if (external_type == AGX_D3DKMT_EXTERNAL_MEMORY_NONE || handle == NULL)
      return vk_error(dev, VK_ERROR_INVALID_EXTERNAL_HANDLE);
   if (external_type == AGX_D3DKMT_EXTERNAL_MEMORY_OPAQUE_WIN32_KMT &&
       agx_d3dkmt_is_local_allocation_handle((uintptr_t)handle))
      return vk_error(dev, VK_ERROR_INVALID_EXTERNAL_HANDLE);

   struct agx_bo *bo = agx_d3dkmt_bo_import_external(
      &dev->dev, (uintptr_t)handle, external_type,
      "Win32 memory property query");
   if (bo == NULL)
      return vk_error(dev, VK_ERROR_INVALID_EXTERNAL_HANDLE);

   uint32_t type_bits = 0;
   for (unsigned t = 0; t < ARRAY_SIZE(pdev->mem_types); t++) {
      const unsigned flags =
         hk_memory_type_flags(&pdev->mem_types[t], handleType);
      if (!(flags & ~bo->flags))
         type_bits |= (1 << t);
   }
   pProperties->memoryTypeBits = type_bits;
   agx_bo_unreference(&dev->dev, bo);

   return type_bits ? VK_SUCCESS
                    : vk_error(dev, VK_ERROR_INVALID_EXTERNAL_HANDLE);
}
#endif

VKAPI_ATTR VkResult VKAPI_CALL
hk_AllocateMemory(VkDevice device, const VkMemoryAllocateInfo *pAllocateInfo,
                  const VkAllocationCallbacks *pAllocator, VkDeviceMemory *pMem)
{
   VK_FROM_HANDLE(hk_device, dev, device);
   struct hk_physical_device *pdev = hk_device_physical(dev);
   struct hk_device_memory *mem;
   VkResult result = VK_SUCCESS;

   const VkImportMemoryFdInfoKHR *fd_info =
      vk_find_struct_const(pAllocateInfo->pNext, IMPORT_MEMORY_FD_INFO_KHR);
   const VkMemoryDedicatedAllocateInfo *dedicated_info =
      vk_find_struct_const(pAllocateInfo->pNext,
                           MEMORY_DEDICATED_ALLOCATE_INFO);
   const VkExportMemoryAllocateInfo *export_info =
      vk_find_struct_const(pAllocateInfo->pNext, EXPORT_MEMORY_ALLOCATE_INFO);
#ifdef _WIN32
   const VkImportMemoryWin32HandleInfoKHR *win32_info =
      vk_find_struct_const(pAllocateInfo->pNext,
                           IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR);
#endif
   const VkMemoryType *type = &pdev->mem_types[pAllocateInfo->memoryTypeIndex];

#ifdef HK_HAVE_BRIDGE_HOOKS
   /* VK_STRUCTURE_TYPE_AURORA_RUNTIME_RESOURCE_ALLOCATE_INFO. Private sType,
    * matched by value to avoid a header dependency on the DXVK-side struct. */
   bool aurora_runtime_resource = false;
   for (const VkBaseInStructure *bs =
           (const VkBaseInStructure *)pAllocateInfo->pNext;
        bs != NULL; bs = bs->pNext) {
      if ((unsigned)bs->sType == 0x7ff84159u) {
         aurora_runtime_resource = true;
         break;
      }
   }
#endif

   VkExternalMemoryHandleTypeFlags handle_types = 0;
   if (export_info != NULL)
      handle_types |= export_info->handleTypes;
   if (fd_info != NULL)
      handle_types |= fd_info->handleType;
#ifdef _WIN32
   if (win32_info != NULL)
      handle_types |= win32_info->handleType;

   const enum agx_d3dkmt_external_memory_type win32_type =
      hk_d3dkmt_external_type(handle_types);
   {
      extern void agx_bridge_valog(const char *fmt, ...);
      static unsigned hk_alloc_ext_logged;
      if (hk_alloc_ext_logged < 16u) {
         hk_alloc_ext_logged++;
         agx_bridge_valog(
            "HK ALLOC handle_types=0x%x win32_type=%d export=%d fd=%d "
            "win32import=%d size=%llu",
            (unsigned)handle_types, (int)win32_type,
            export_info != NULL ? 1 : 0, fd_info != NULL ? 1 : 0,
            win32_info != NULL ? 1 : 0,
            (unsigned long long)pAllocateInfo->allocationSize);
      }
   }
   const uintptr_t win32_handle =
      win32_info != NULL ? (uintptr_t)win32_info->handle : 0;
   const bool local_allocation =
      agx_d3dkmt_is_local_allocation_handle(win32_handle);
   if ((handle_types != 0 && win32_type == AGX_D3DKMT_EXTERNAL_MEMORY_NONE) ||
       (win32_info != NULL &&
        (win32_info->handle == NULL || win32_info->name != NULL)) ||
       (local_allocation &&
        win32_type != AGX_D3DKMT_EXTERNAL_MEMORY_OPAQUE_WIN32_KMT) ||
       (win32_info != NULL && fd_info != NULL) ||
       (win32_info != NULL && export_info != NULL &&
        export_info->handleTypes != 0 &&
        export_info->handleTypes != win32_info->handleType)) {
      return vk_error(dev, VK_ERROR_INVALID_EXTERNAL_HANDLE);
   }
#endif

   const unsigned flags = hk_memory_type_flags(type, handle_types);

   uint32_t alignment = 16384; /* Apple page size */

   struct hk_memory_heap *heap = &pdev->mem_heaps[type->heapIndex];
   if (p_atomic_read(&heap->used) > heap->size) {
#if defined(__has_include)
#if __has_include("asahi/lib/agx_bridge_vma.h")
      {
         static unsigned hk_heap_full_logged;
         if (hk_heap_full_logged < 16u) {
            hk_heap_full_logged++;
            agx_bridge_valog(
               "HK ALLOC FAIL reason=heap-full size=%llu typeIndex=%u "
               "heapIndex=%u heap=%llu/%llu",
               (unsigned long long)pAllocateInfo->allocationSize,
               pAllocateInfo->memoryTypeIndex, type->heapIndex,
               (unsigned long long)p_atomic_read(&heap->used),
               (unsigned long long)heap->size);
         }
      }
#endif
#endif
      return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   }

   const uint64_t aligned_size =
      align64(pAllocateInfo->allocationSize, alignment);

   mem = vk_device_memory_create(&dev->vk, pAllocateInfo, pAllocator,
                                 sizeof(*mem));
   if (!mem)
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);

   mem->map = NULL;
#ifdef _WIN32
   if (win32_info != NULL) {
      if (local_allocation) {
         mem->bo = agx_d3dkmt_bo_import_local(
            &dev->dev,
            agx_d3dkmt_local_allocation_handle(win32_handle), aligned_size,
            "Borrowed D3DKMT allocation");
      } else {
         mem->bo = agx_d3dkmt_bo_import_external(
            &dev->dev, win32_handle, win32_type,
            "Imported Win32 memory");
      }
      /* For a Win32 import the Vulkan allocationSize is the importing
       * image's own requirement (and the specification ignores it for
       * import operations), while the opened KM allocation is 64 KiB
       * granular (d3dkmt_create_allocation) and so may legitimately be
       * larger. Refuse only a smaller BO. The borrowed local path keeps
       * exact equality: its BO size is this very call's rounded request,
       * so any difference there is a caller bug, not granularity. */
      if (mem->bo == NULL || mem->bo->size < aligned_size ||
          (local_allocation && mem->bo->size != aligned_size) ||
          (flags & ~mem->bo->flags)) {
         if (mem->bo != NULL)
            agx_bo_unreference(&dev->dev, mem->bo);
         result = vk_error(dev, VK_ERROR_INVALID_EXTERNAL_HANDLE);
         goto fail_alloc;
      }
   } else
#endif
   if (fd_info && fd_info->handleType) {
      assert(
         fd_info->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT ||
         fd_info->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);

      mem->bo = agx_bo_import(&dev->dev, fd_info->fd);
      if (mem->bo == NULL) {
         result = vk_error(dev, VK_ERROR_INVALID_EXTERNAL_HANDLE);
         goto fail_alloc;
      }
      assert(!(flags & ~mem->bo->flags));
   } else {
      enum agx_bo_flags bo_flags = 0;
      if (handle_types)
         bo_flags |= AGX_BO_SHAREABLE;
      if (type->propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
         bo_flags |= AGX_BO_WRITEBACK;

#ifdef _WIN32
      /* Aurora scanout donor: a fresh exportable dedicated allocation for a
       * validated tight-linear scanout image declares itself to the KMD so
       * the desktop primary created next by this process can adopt its
       * physical pages (there is no working cross-device import on dxgkrnl).
       * Forced write-combined regardless of the Vulkan memory type: the KMD
       * scans these exact pages out and its donor contract refuses the
       * cached class.  GPU coherency is unaffected; only the rare CPU map of
       * this one image pays the write-combined read cost. */
      if (dedicated_info != NULL &&
          dedicated_info->image != VK_NULL_HANDLE &&
          win32_type != AGX_D3DKMT_EXTERNAL_MEMORY_NONE) {
         VK_FROM_HANDLE(hk_image, dedicated_image, dedicated_info->image);

         if (dedicated_image->aurora_tight_linear) {
            bo_flags = (enum agx_bo_flags)(
               ((uint32_t)bo_flags | AGX_BO_SCANOUT_DONOR) &
               ~(uint32_t)AGX_BO_WRITEBACK);
         }
      }
      enum agx_d3dkmt_external_memory_type create_type = win32_type;
#ifdef HK_HAVE_BRIDGE_HOOKS
      /* The private runtime-resource pNext is the outer D3D runtime's claim
       * of allocation ownership, and under the callback bridge it must
       * DOMINATE DXVK's speculative Vulkan export request, not defer to it.
       * A bridge (callback) create can never mint an hGlobalShare, so when
       * Triton keeps MISC_SHARED on the runtime-owned surface DXVK chains an
       * OPAQUE_WIN32_KMT export and the KMT-typed create used to claim the
       * pending Triton handoff, then fail d3dkmt_create_allocation's
       * global-share shape check -- destroying the allocation AFTER the
       * handoff completion had already published its handle. Triton kept a
       * stale hAllocation (33k D3DKMT 0xc000000d), and the DXVK retry became
       * the VM-private EXPORT-DEGRADED BO whose export was then REFUSED
       * (record.external_type=0 vs want=2), so DWM looped full-screen
       * surface creates until the backing pool ran out. Routing EVERY
       * runtime-owned create through OPAQUE_WIN32 keeps allocation ownership
       * with the runtime -- sharing is the runtime's job on this path -- and
       * passes the shape check (resource-owned, no global share expected).
       * A later vkGetMemoryWin32HandleKHR(KMT) redemption against the type-1
       * record still fails closed exactly as before (DXVK warns and carries
       * on), and with no handoff pending the bridge still degrades the
       * OPAQUE_WIN32 create to a plain VM-private allocation as today. */
      if (aurora_runtime_resource &&
          create_type != AGX_D3DKMT_EXTERNAL_MEMORY_OPAQUE_WIN32 &&
          agx_bridge_vma_registered(&dev->dev)) {
         const int requested_type = (int)create_type;
         create_type = AGX_D3DKMT_EXTERNAL_MEMORY_OPAQUE_WIN32;
         bo_flags = (enum agx_bo_flags)((uint32_t)bo_flags | AGX_BO_SHAREABLE);
         static LONG rr_logged;
         if (InterlockedIncrement(&rr_logged) <= 8)
            agx_bridge_valog(
               "HK RUNTIME-RESOURCE routing size=%llu typeIndex=%u "
               "dedicatedImage=%u requested_type=%d (CreateResource so the "
               "Triton handoff can claim it; the runtime owns sharing)",
               (unsigned long long)aligned_size,
               pAllocateInfo->memoryTypeIndex,
               (dedicated_info != NULL &&
                dedicated_info->image != VK_NULL_HANDLE)
                  ? 1u
                  : 0u,
               requested_type);
      }
      /* Patch 0072 sees the original Win32 handle type before the runtime
       * resource route above, so its export-only donor gate intentionally
       * misses this allocation. Tag the same validated tight-linear
       * dedicated image after routing. Triton's donor scope then strips
       * PRIMARY/ADOPT/PITCH_ALIGN128 and preserves the exact donor role. */
      if (aurora_runtime_resource && dedicated_info != NULL &&
          dedicated_info->image != VK_NULL_HANDLE &&
          create_type != AGX_D3DKMT_EXTERNAL_MEMORY_NONE) {
         VK_FROM_HANDLE(hk_image, dedicated_image, dedicated_info->image);

         if (dedicated_image->aurora_tight_linear) {
            bo_flags = (enum agx_bo_flags)(
               ((uint32_t)bo_flags | AGX_BO_SCANOUT_DONOR) &
               ~(uint32_t)AGX_BO_WRITEBACK);
         }
      }
#endif
      if (create_type != AGX_D3DKMT_EXTERNAL_MEMORY_NONE) {
         mem->bo = agx_d3dkmt_bo_create_external(
            &dev->dev, aligned_size, 0, bo_flags, create_type, "App memory");
      } else
#endif
      {
         mem->bo =
            agx_bo_create(&dev->dev, aligned_size, 0, bo_flags, "App memory");
      }
      if (!mem->bo) {
         result = vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
         goto fail_alloc;
      }
   }

   /* Shadow map in case this is used for a sparse resident buffer */
   int ret = agx_bo_bind(&dev->dev, mem->bo,
                         agx_rw_addr_to_ro(&dev->dev, mem->bo->va->addr),
                         mem->bo->size, 0, DRM_ASAHI_BIND_READ);
   if (ret) {
      result = vk_error(dev, VK_ERROR_UNKNOWN);
      goto fail_bo;
   }

   if (mem->bo->flags & (AGX_BO_SHAREABLE | AGX_BO_SHARED))
      hk_add_ext_bo(dev, mem->bo);

   if (fd_info && fd_info->handleType) {
      /* From the Vulkan spec:
       *
       *    "Importing memory from a file descriptor transfers ownership of
       *    the file descriptor from the application to the Vulkan
       *    implementation. The application must not perform any operations on
       *    the file descriptor after a successful import."
       *
       * If the import fails, we leave the file descriptor open.
       */
      close(fd_info->fd);
   }

   uint64_t heap_used = p_atomic_add_return(&heap->used, mem->bo->size);
   if (heap_used > heap->size) {
#if defined(__has_include)
#if __has_include("asahi/lib/agx_bridge_vma.h")
      {
         static unsigned hk_post_full_logged;
         if (hk_post_full_logged < 16u) {
            hk_post_full_logged++;
            agx_bridge_valog(
               "HK ALLOC FAIL reason=post-add size=%llu bo_size=%llu "
               "typeIndex=%u heap=%llu/%llu",
               (unsigned long long)pAllocateInfo->allocationSize,
               (unsigned long long)mem->bo->size,
               pAllocateInfo->memoryTypeIndex,
               (unsigned long long)heap_used,
               (unsigned long long)heap->size);
         }
      }
#endif
#endif
      hk_FreeMemory(device, hk_device_memory_to_handle(mem), pAllocator);
      return vk_errorf(device, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                       "Out of heap memory");
   }

   *pMem = hk_device_memory_to_handle(mem);

   return VK_SUCCESS;

fail_bo:
   agx_bo_unreference(&dev->dev, mem->bo);
fail_alloc:
   vk_device_memory_destroy(&dev->vk, pAllocator, &mem->vk);
   return result;
}

VKAPI_ATTR void VKAPI_CALL
hk_FreeMemory(VkDevice device, VkDeviceMemory _mem,
              const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(hk_device, dev, device);
   VK_FROM_HANDLE(hk_device_memory, mem, _mem);
   struct hk_physical_device *pdev = hk_device_physical(dev);

   if (!mem)
      return;

   const VkMemoryType *type = &pdev->mem_types[mem->vk.memory_type_index];
   struct hk_memory_heap *heap = &pdev->mem_heaps[type->heapIndex];
   p_atomic_add(&heap->used, -((int64_t)mem->bo->size));

   if (mem->bo->flags & (AGX_BO_SHAREABLE | AGX_BO_SHARED))
      hk_remove_ext_bo(dev, mem->bo);

   agx_bo_unreference(&dev->dev, mem->bo);

   vk_device_memory_destroy(&dev->vk, pAllocator, &mem->vk);
}

VKAPI_ATTR VkResult VKAPI_CALL
hk_MapMemory2KHR(VkDevice device, const VkMemoryMapInfoKHR *pMemoryMapInfo,
                 void **ppData)
{
   VK_FROM_HANDLE(hk_device, dev, device);
   VK_FROM_HANDLE(hk_device_memory, mem, pMemoryMapInfo->memory);

   if (mem == NULL) {
      *ppData = NULL;
      return VK_SUCCESS;
   }

   const VkDeviceSize offset = pMemoryMapInfo->offset;
   const VkDeviceSize size = vk_device_memory_range(
      &mem->vk, pMemoryMapInfo->offset, pMemoryMapInfo->size);

   void *fixed_addr = NULL;
   if (pMemoryMapInfo->flags & VK_MEMORY_MAP_PLACED_BIT_EXT) {
      const VkMemoryMapPlacedInfoEXT *placed_info = vk_find_struct_const(
         pMemoryMapInfo->pNext, MEMORY_MAP_PLACED_INFO_EXT);
      fixed_addr = placed_info->pPlacedAddress;
   }

   /* From the Vulkan spec version 1.0.32 docs for MapMemory:
    *
    *  * If size is not equal to VK_WHOLE_SIZE, size must be greater than 0
    *    assert(size != 0);
    *  * If size is not equal to VK_WHOLE_SIZE, size must be less than or
    *    equal to the size of the memory minus offset
    */
   assert(size > 0);
   assert(offset + size <= mem->bo->size);

   if (size != (size_t)size) {
      return vk_errorf(dev, VK_ERROR_MEMORY_MAP_FAILED,
                       "requested size 0x%" PRIx64 " does not fit in %u bits",
                       size, (unsigned)(sizeof(size_t) * 8));
   }

   /* From the Vulkan 1.2.194 spec:
    *
    *    "memory must not be currently host mapped"
    */
   if (mem->map != NULL) {
      return vk_errorf(dev, VK_ERROR_MEMORY_MAP_FAILED,
                       "Memory object already mapped.");
   }

   mem->map = agx_bo_map_placed(mem->bo, fixed_addr);
   if (mem->map == NULL) {
      *ppData = NULL;
      return vk_errorf(dev, VK_ERROR_MEMORY_MAP_FAILED,
                       "failed to map memory BO handle %u",
                       mem->bo->uapi_handle);
   }

   assert(!fixed_addr || mem->map == fixed_addr);
   *ppData = mem->map + offset;

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
hk_UnmapMemory2KHR(VkDevice device,
                   const VkMemoryUnmapInfoKHR *pMemoryUnmapInfo)
{
   VK_FROM_HANDLE(hk_device_memory, mem, pMemoryUnmapInfo->memory);

   if (mem == NULL)
      return VK_SUCCESS;

   if (pMemoryUnmapInfo->flags & VK_MEMORY_UNMAP_RESERVE_BIT_EXT) {
      VK_FROM_HANDLE(hk_device, dev, device);

      /*
       * honeykrisp-h1-execution.md SS2.2 row P7: the PROT_NONE over-map
       * path backs EXT_map_memory_placed's "unmap but reserve the VA"
       * semantics, POSIX mmap(2)-only. That extension is disabled on
       * Windows (row P2 -- its enable bit is #ifndef _WIN32 in
       * hk_physical_device.c), so this branch is unreachable there; still
       * guard it so hk_device_memory.c stays includable without
       * <sys/mman.h>.
       */
#ifndef _WIN32
      void *err = mmap(mem->bo->_map, mem->bo->size, PROT_NONE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
      if (err == MAP_FAILED) {
         return vk_errorf(dev, VK_ERROR_MEMORY_MAP_FAILED,
                          "Failed to map over original mapping");
      }
      mem->bo->_map = NULL;
#else
      return vk_errorf(dev, VK_ERROR_FEATURE_NOT_PRESENT,
                       "VK_MEMORY_UNMAP_RESERVE_BIT_EXT unsupported");
#endif
   } else {
      if (mem->bo->_map) {
         /*
          * honeykrisp-h1-execution.md SS2.2 row P7: route through the
          * bo_munmap op (works both OSes; the Linux body is plain munmap)
          * instead of calling munmap(2) directly.
          */
         VK_FROM_HANDLE(hk_device, dev, device);
         dev->dev.ops.bo_munmap(&dev->dev, mem->bo);
         mem->bo->_map = NULL;
      }
   }

   mem->map = NULL;

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
hk_FlushMappedMemoryRanges(VkDevice device, uint32_t memoryRangeCount,
                           const VkMappedMemoryRange *pMemoryRanges)
{
   VK_FROM_HANDLE(hk_device, dev, device);

   for (uint32_t i = 0; i < memoryRangeCount; i++) {
      const VkMappedMemoryRange *range = &pMemoryRanges[i];
      VK_FROM_HANDLE(hk_device_memory, mem, range->memory);
      const VkDeviceSize size =
         vk_device_memory_range(&mem->vk, range->offset, range->size);

      if (size != 0 &&
          agx_bo_sync_for_device(mem->bo, range->offset, size) != 0)
         return vk_device_set_lost(&dev->vk, "memory flush failed");
   }
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
hk_InvalidateMappedMemoryRanges(VkDevice device, uint32_t memoryRangeCount,
                                const VkMappedMemoryRange *pMemoryRanges)
{
   VK_FROM_HANDLE(hk_device, dev, device);

   for (uint32_t i = 0; i < memoryRangeCount; i++) {
      const VkMappedMemoryRange *range = &pMemoryRanges[i];
      VK_FROM_HANDLE(hk_device_memory, mem, range->memory);
      const VkDeviceSize size =
         vk_device_memory_range(&mem->vk, range->offset, range->size);

      if (size != 0 && agx_bo_sync_for_cpu(mem->bo, range->offset, size) != 0)
         return vk_device_set_lost(&dev->vk, "memory invalidate failed");
   }
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
hk_GetDeviceMemoryCommitment(VkDevice device, VkDeviceMemory _mem,
                             VkDeviceSize *pCommittedMemoryInBytes)
{
   VK_FROM_HANDLE(hk_device_memory, mem, _mem);

   *pCommittedMemoryInBytes = mem->bo->size;
}

VKAPI_ATTR VkResult VKAPI_CALL
hk_GetMemoryFdKHR(VkDevice device, const VkMemoryGetFdInfoKHR *pGetFdInfo,
                  int *pFD)
{
   VK_FROM_HANDLE(hk_device, dev, device);
   VK_FROM_HANDLE(hk_device_memory, memory, pGetFdInfo->memory);

   switch (pGetFdInfo->handleType) {
   case VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT:
   case VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT:
      *pFD = agx_bo_export(&dev->dev, memory->bo);
      return VK_SUCCESS;
   default:
      assert(!"unsupported handle type");
      return vk_error(dev, VK_ERROR_FEATURE_NOT_PRESENT);
   }
}

#ifdef _WIN32
VKAPI_ATTR VkResult VKAPI_CALL
hk_GetMemoryWin32HandleKHR(
   VkDevice device,
   const VkMemoryGetWin32HandleInfoKHR *pGetWin32HandleInfo, HANDLE *pHandle)
{
   VK_FROM_HANDLE(hk_device, dev, device);
   VK_FROM_HANDLE(hk_device_memory, memory,
                  pGetWin32HandleInfo->memory);
   const enum agx_d3dkmt_external_memory_type external_type =
      hk_d3dkmt_external_type(pGetWin32HandleInfo->handleType);

   if (external_type == AGX_D3DKMT_EXTERNAL_MEMORY_NONE || memory == NULL ||
       !(memory->vk.export_handle_types &
         pGetWin32HandleInfo->handleType)) {
      return vk_error(dev, VK_ERROR_INVALID_EXTERNAL_HANDLE);
   }

   uintptr_t handle = 0;
   if (agx_d3dkmt_bo_export_external(&dev->dev, memory->bo, external_type,
                                     &handle) != 0) {
      return vk_error(dev, VK_ERROR_INVALID_EXTERNAL_HANDLE);
   }

   *pHandle = (HANDLE)handle;
   return VK_SUCCESS;
}
#endif

VKAPI_ATTR uint64_t VKAPI_CALL
hk_GetDeviceMemoryOpaqueCaptureAddress(
   UNUSED VkDevice device,
   UNUSED const VkDeviceMemoryOpaqueCaptureAddressInfo *pInfo)
{
   /* Addresses are replayed at buffer and image creation, not memory. */
   return 0;
}

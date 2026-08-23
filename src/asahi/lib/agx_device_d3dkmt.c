/*
 * Copyright 2026 the Asahi Linux contributors
 * SPDX-License-Identifier: MIT
 */

/*
 * agx_device_ops_t over D3DKMT. See agx_device_d3dkmt.h for why, and
 * agx_d3dkmt.h's CONTRACT ASSUMPTIONS block for every kernel-side guess this
 * file makes, tagged with the contract question it answers.
 *
 * Each op below is annotated with the AGX ioctl it replaces and with the one
 * thing that changes semantically, because in three places the change is real
 * rather than cosmetic:
 *
 *   VM_BIND    DRM binds synchronously; WDDM page-table updates complete on a
 *              paging fence. This file restores synchrony by waiting.
 *   SUBMIT     DRM submits waits, work and signals as one atomic ioctl; WDDM
 *              has no sync list on SubmitCommand and expresses the same thing
 *              as three ordered calls on one context.
 *   GEM handle DRM GEM handles are dense small integers; D3DKMT handles are
 *              not, and agx_device.c iterates the handle space.
 *
 * NOT RUNTIME-VERIFIED: there is no WDDM miniport for this GPU yet and no
 * hardware available this session. Everything here cross-compiles for
 * aarch64-w64-mingw32 as part of the real ICD; nothing here has executed.
 */

#include "agx_device_d3dkmt.h"
#include "agx_d3dkmt_priv.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winternl.h>

#include "util/macros.h"
#include "util/bitscan.h"
#include "util/u_math.h"
#include "util/u_atomic.h"
#include "agx_bo.h"
#include "agx_bridge_vma.h"
#include "agx_d3dkmt_bridge.h"
#include "layout.h"

/* GPU virtual-address reservations are owned by dxgkrnl's process/adapter
 * address space, not by an individual D3DKMT device.  Vulkan keeps a
 * physical-device winsys alive while creating one or more logical-device
 * winsyses, so blindly reserving the fixed Honeykrisp layout for every
 * winsys collides with the first reservation.  Keep an exact process-local
 * lease keyed by adapter LUID and release it only after the last winsys.
 *
 * A partially failed last release is retained with a per-range mask.  The
 * next acquire repairs only the missing ranges before publishing a new
 * reference, so a FreeGpuVirtualAddress failure cannot turn into an ABA hole.
 */
struct agx_d3dkmt_va_lease {
   struct agx_kmt_luid luid;
   struct agx_d3dkmt_va_range ranges[2];
   uint32_t range_count;
   uint32_t reserved_mask;
   uint32_t references;
   struct agx_d3dkmt_va_lease *next;
};

struct agx_d3dkmt_device_lease {
   struct agx_kmt_luid luid;
   agx_kmt_handle device;
   uint32_t references;
   struct agx_d3dkmt_device_lease *next;
};

static SRWLOCK agx_d3dkmt_va_lease_lock = SRWLOCK_INIT;
static struct agx_d3dkmt_va_lease *agx_d3dkmt_va_leases;
static SRWLOCK agx_d3dkmt_device_lease_lock = SRWLOCK_INIT;
static struct agx_d3dkmt_device_lease *agx_d3dkmt_device_leases;

/* The mirrored constants in agx_d3dkmt_core.h must equal the real ones. This
 * is the tripwire that keeps the Mesa-free core honest. */
static_assert(AGX_D3DKMT_BIND_UNBIND == DRM_ASAHI_BIND_UNBIND, "BIND_UNBIND");
static_assert(AGX_D3DKMT_BIND_READ == DRM_ASAHI_BIND_READ, "BIND_READ");
static_assert(AGX_D3DKMT_BIND_WRITE == DRM_ASAHI_BIND_WRITE, "BIND_WRITE");
static_assert(AGX_D3DKMT_BIND_SINGLE_PAGE == DRM_ASAHI_BIND_SINGLE_PAGE,
              "BIND_SINGLE_PAGE");
static_assert(AGX_D3DKMT_BO_SHARED == AGX_BO_SHARED, "BO_SHARED");
static_assert(AGX_D3DKMT_BO_LOW_VA == AGX_BO_LOW_VA, "BO_LOW_VA");
static_assert(AGX_D3DKMT_BO_EXEC == AGX_BO_EXEC, "BO_EXEC");
static_assert(AGX_D3DKMT_BO_WRITEBACK == AGX_BO_WRITEBACK, "BO_WRITEBACK");
static_assert(AGX_D3DKMT_BO_SHAREABLE == AGX_BO_SHAREABLE, "BO_SHAREABLE");
static_assert(AGX_D3DKMT_BO_READONLY == AGX_BO_READONLY, "BO_READONLY");
static_assert(AGX_D3DKMT_PAGE_SIZE == AIL_PAGESIZE, "page size");
static_assert(AGXW_PARAMS_GLOBAL_SIZE == sizeof(struct drm_asahi_params_global),
              "GET_PARAMS escape payload size");
static_assert(AGXW_GET_TIME_SIZE == sizeof(struct drm_asahi_get_time),
              "GET_TIME escape payload size");
static_assert((int)AGXW_SYNC_BINARY == (int)DRM_ASAHI_SYNC_SYNCOBJ,
              "SYNC_SYNCOBJ");
static_assert((int)AGXW_SYNC_TIMELINE == (int)DRM_ASAHI_SYNC_TIMELINE_SYNCOBJ,
              "SYNC_TIMELINE_SYNCOBJ");
static_assert(sizeof(struct agxw_sync) == sizeof(struct drm_asahi_sync),
              "agxw_sync mirrors drm_asahi_sync");
static_assert((int)AGXW_PRIORITY_LOW == (int)DRM_ASAHI_PRIORITY_LOW,
              "priority LOW");
static_assert((int)AGXW_PRIORITY_REALTIME == (int)DRM_ASAHI_PRIORITY_REALTIME,
              "priority REALTIME");

/* -------------------------------------------------------------------------
 * Error mapping and waits.
 * ------------------------------------------------------------------------- */
int
agx_d3dkmt_fail(agx_kmt_status st)
{
#if AGX_GPU_DIAG >= 2
   extern IMAGE_DOS_HEADER __ImageBase;
   /* BRIDGE DIAGNOSTIC: name every D3DKMT failure with its raw NTSTATUS.
    * Every sync/submit failure funnels through here and the first one marks
    * the winsys device_lost, after which every sync op returns EIO and DXVK
    * reports only "waitForIdle: Operation failed" / "Failed to create a DXGI
    * factory" -- naming neither the call nor the status. */
   {
      char aurora_line[160];
      int aurora_len = _snprintf_s(aurora_line, sizeof(aurora_line), _TRUNCATE,
                                   "agx_d3dkmt_fail: NTSTATUS=0x%08x rva=0x%08llx\n",
                                   (unsigned)st,
                                   (unsigned long long)((char *)__builtin_return_address(0) -
                                                        (char *)&__ImageBase));
      if (aurora_len > 0) {
         HANDLE aurora_h = CreateFileA(
            "C:\\aurora\\logs\\agx_d3dkmt_fail.log", FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
         if (aurora_h != INVALID_HANDLE_VALUE) {
            DWORD aurora_written = 0;
            (void)WriteFile(aurora_h, aurora_line, (DWORD)aurora_len,
                            &aurora_written, NULL);
            CloseHandle(aurora_h);
         }
      }
   }
#endif
   switch ((uint32_t)st) {
   case 0xC0000017u: /* STATUS_NO_MEMORY */
   case 0xC000009Au: /* STATUS_INSUFFICIENT_RESOURCES */
      errno = ENOMEM;
      break;
   case 0xC000000Du: /* STATUS_INVALID_PARAMETER */
   case 0xC0000008u: /* STATUS_INVALID_HANDLE */
      errno = EINVAL;
      break;
   case 0xC0000022u: /* STATUS_ACCESS_DENIED */
      errno = EACCES;
      break;
   case 0x00000102u: /* STATUS_TIMEOUT */
      errno = ETIME;
      break;
   case 0xC00000BBu: /* STATUS_NOT_SUPPORTED */
      errno = ENOTSUP;
      break;
   case 0xC01E0116u: /* STATUS_GRAPHICS_ALLOCATION_INVALID */
   case 0xC01E0110u: /* STATUS_GRAPHICS_NO_VIDEO_MEMORY */
      errno = ENOMEM;
      break;
   default:
      errno = EIO;
      break;
   }

   return -1;
}

void
agx_d3dkmt_diag_sync_gap(struct agx_device *dev, uint32_t queue_id,
                         uint32_t syncobj, uint64_t requested,
                         uint64_t observed)
{
#if AGX_GPU_DIAG >= 2
   struct agx_d3dkmt_winsys *ws = agx_d3dkmt_ws(dev);
   struct agx_d3dkmt_diag_submit ring[AGX_D3DKMT_DIAG_SUBMIT_RING];
   uint64_t wait_current[AGX_D3DKMT_DIAG_SUBMIT_RING]
                        [AGX_D3DKMT_DIAG_SYNCS] = {{0}};
   uint64_t signal_current[AGX_D3DKMT_DIAG_SUBMIT_RING]
                          [AGX_D3DKMT_DIAG_SYNCS] = {{0}};
   char output[8192];
   size_t used = 0;

   simple_mtx_lock(&ws->lock);
   if (ws->diag_sync_gap_dumped) {
      simple_mtx_unlock(&ws->lock);
      return;
   }
   ws->diag_sync_gap_dumped = true;
   memcpy(ring, ws->diag_submits, sizeof(ring));
   for (unsigned i = 0; i < AGX_D3DKMT_DIAG_SUBMIT_RING; ++i) {
      for (unsigned j = 0; j < AGX_D3DKMT_DIAG_SYNCS; ++j) {
         if (ring[i].wait_handles[j] == 0)
            continue;
         struct agx_d3dkmt_sync *s =
            agx_d3dkmt_sync_table_get(&ws->syncs, ring[i].wait_handles[j]);
         if (s != NULL)
            wait_current[i][j] = agx_d3dkmt_sync_current(s);
      }
      for (unsigned j = 0; j < AGX_D3DKMT_DIAG_SYNCS; ++j) {
         if (ring[i].signal_handles[j] == 0)
            continue;
         struct agx_d3dkmt_sync *s = agx_d3dkmt_sync_table_get(
            &ws->syncs, ring[i].signal_handles[j]);
         if (s != NULL)
            signal_current[i][j] = agx_d3dkmt_sync_current(s);
      }
   }
   simple_mtx_unlock(&ws->lock);

#define AGX_DIAG_APPEND(...)                                                   \
   do {                                                                        \
      if (used < sizeof(output)) {                                             \
         int n = _snprintf_s(output + used, sizeof(output) - used, _TRUNCATE, \
                             __VA_ARGS__);                                     \
         if (n > 0)                                                           \
            used += (size_t)n;                                                \
      }                                                                        \
   } while (0)

   AGX_DIAG_APPEND(
      "SYNC_GAP pid=%lu tick=%llu queue=%u sync=%u requested=%llu observed=%llu\n",
      (unsigned long)GetCurrentProcessId(),
      (unsigned long long)GetTickCount64(), queue_id, syncobj,
      (unsigned long long)requested, (unsigned long long)observed);
   for (unsigned i = 0; i < AGX_D3DKMT_DIAG_SUBMIT_RING; ++i) {
      const struct agx_d3dkmt_diag_submit *d = &ring[i];
      if (d->sequence == 0)
         continue;
      AGX_DIAG_APPEND(
         "SYNC_SUBMIT seq=%llu tick=%llu queue=%u ctx=%u bytes=%u in=%u out=%u callbacks=0x%x",
         (unsigned long long)d->sequence, (unsigned long long)d->tick_ms,
         d->queue_id, d->context, d->cmdbuf_size, d->in_count, d->out_count,
         d->callback_mask);
      for (unsigned j = 0;
           j < MIN2(d->in_count, AGX_D3DKMT_DIAG_SYNCS); ++j) {
         AGX_DIAG_APPEND(" wait%u=%u:%llu/%llu", j, d->wait_handles[j],
                         (unsigned long long)wait_current[i][j],
                         (unsigned long long)d->wait_values[j]);
      }
      for (unsigned j = 0;
           j < MIN2(d->out_count, AGX_D3DKMT_DIAG_SYNCS); ++j) {
         AGX_DIAG_APPEND(" signal%u=%u:%llu/%llu", j, d->signal_handles[j],
                         (unsigned long long)signal_current[i][j],
                         (unsigned long long)d->signal_values[j]);
      }
      AGX_DIAG_APPEND("\n");
   }
#undef AGX_DIAG_APPEND

   if (used != 0) {
      HANDLE file = CreateFileA(
         "C:\\aurora\\logs\\agx_d3dkmt_sync.log", FILE_APPEND_DATA,
         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
      if (file != INVALID_HANDLE_VALUE) {
         DWORD written = 0;
         (void)WriteFile(file, output, (DWORD)used, &written, NULL);
         CloseHandle(file);
      }
   }
#else
   (void)dev;
   (void)queue_id;
   (void)syncobj;
   (void)requested;
   (void)observed;
#endif
}

int
agx_d3dkmt_poll_cpu_fences(const volatile uint64_t *const *current,
                           const uint64_t *wanted, uint32_t count,
                           bool wait_any, uint32_t timeout_ms,
                           uint32_t *first_signaled)
{
   if (timeout_ms == AGX_D3DKMT_INFINITE) {
      errno = EINVAL;
      return -1;
   }

   ULONGLONG start = GetTickCount64();
   for (;;) {
      enum agx_d3dkmt_cpu_wait_state state = agx_d3dkmt_cpu_wait_probe(
         current, wanted, count, wait_any, first_signaled);
      if (state == AGX_D3DKMT_CPU_WAIT_SATISFIED) {
         MemoryBarrier();
         return 0;
      }
      if (state == AGX_D3DKMT_CPU_WAIT_INVALID) {
         errno = EIO;
         return -1;
      }

      if (GetTickCount64() - start >= timeout_ms) {
         errno = ETIME;
         return -1;
      }

      if (!SwitchToThread())
         Sleep(1);
   }
}

int
agx_d3dkmt_wait_fence(struct agx_d3dkmt_winsys *ws, agx_kmt_handle fence,
                      const volatile uint64_t *fence_cpu, uint64_t value,
                      uint32_t timeout_ms)
{
   const agx_kmt_handle handles[1] = {fence};
   const uint64_t values[1] = {value};
   const volatile uint64_t *current[1] = {fence_cpu};

   enum agx_d3dkmt_cpu_wait_state state =
      agx_d3dkmt_cpu_wait_probe(current, values, 1, true, NULL);
   if (state == AGX_D3DKMT_CPU_WAIT_SATISFIED) {
      MemoryBarrier();
      return 0;
   }
   if (state == AGX_D3DKMT_CPU_WAIT_INVALID) {
      errno = EIO;
      return -1;
   }

   if (timeout_ms != AGX_D3DKMT_INFINITE)
      return agx_d3dkmt_poll_cpu_fences(current, values, 1, true, timeout_ms,
                                        NULL);

   struct agx_kmt_waitsyncfromcpu wait = {
      .hDevice = ws->device,
      .ObjectCount = 1,
      .ObjectHandleArray = handles,
      .FenceValueArray = values,
      .hAsyncEvent = NULL,
   };

   /*
    * D3DKMTWaitForSynchronizationObjectFromCpu has no timeout parameter at
    * all. Its NULL-event form is a synchronous, per-call wait. The event form
    * is intentionally unused: there is no documented cancellation API, so an
    * event cannot safely be closed or reused after a caller-side timeout.
    */
   agx_kmt_status st = ws->fns.WaitForSynchronizationObjectFromCpu(&wait);
   if (!AGX_KMT_SUCCESS(st))
      return agx_d3dkmt_fail(st);

   MemoryBarrier();
   if (agx_d3dkmt_cpu_wait_probe(current, values, 1, true, NULL) ==
       AGX_D3DKMT_CPU_WAIT_SATISFIED)
      return 0;

   errno = EIO;
   return -1;
}

int
agx_d3dkmt_wait_paging(struct agx_d3dkmt_winsys *ws, uint64_t value)
{
   if (value == 0)
      return 0;

   /* Fast path: the paging fence has a CPU-visible value, so most waits cost
    * a load rather than a kernel transition. */
   if (ws->paging_fence_cpu != NULL && *ws->paging_fence_cpu >= value)
      return 0;

   return agx_d3dkmt_wait_fence(ws, ws->paging_fence, ws->paging_fence_cpu,
                                value,
                                AGX_D3DKMT_INFINITE);
}

/* -------------------------------------------------------------------------
 * Contexts.
 * ------------------------------------------------------------------------- */
static struct agx_d3dkmt_ctx *
ctx_get(struct agx_d3dkmt_winsys *ws, uint32_t queue_id)
{
   if (queue_id == 0 || queue_id > ws->ctx_capacity)
      return NULL;

   struct agx_d3dkmt_ctx *ctx = &ws->ctxs[queue_id - 1];
   return ctx->hContext ? ctx : NULL;
}

/* -------------------------------------------------------------------------
 * CPU-ownership worksets.
 *
 * Dense BO handles make a bitset the natural queue: setting a dirty bit is
 * O(1), duplicate-free and allocation-free at every CPU-write choke point.
 * Capacity grows before a newly adopted BO becomes visible. The second half
 * of the allocation is persistent conservative membership; ORing it into the
 * pending half once per submit replaces a BO-table walk with one word per 64
 * possible handles.
 * ------------------------------------------------------------------------- */
static bool
d3dkmt_worksets_reserve_locked(struct agx_device *dev, uint32_t handles)
{
   uint32_t needed = handles / 64u + ((handles % 64u) != 0u);
   uint32_t words = dev->d3dkmt_workset_words;
   uint64_t *replacement;

   if (needed <= words)
      return true;
   if (words == 0)
      words = 16u;
   while (words < needed) {
      if (words > UINT32_MAX / 2u)
         return false;
      words *= 2u;
   }
   if ((size_t)words > SIZE_MAX / (2u * sizeof(uint64_t)))
      return false;

   replacement = calloc((size_t)words * 2u, sizeof(uint64_t));
   if (replacement == NULL)
      return false;
   if (dev->d3dkmt_worksets != NULL) {
      memcpy(replacement, dev->d3dkmt_worksets,
             (size_t)dev->d3dkmt_workset_words * sizeof(uint64_t));
      memcpy(replacement + words,
             dev->d3dkmt_worksets + dev->d3dkmt_workset_words,
             (size_t)dev->d3dkmt_workset_words * sizeof(uint64_t));
      free(dev->d3dkmt_worksets);
   }
   dev->d3dkmt_worksets = replacement;
   dev->d3dkmt_workset_words = words;
   return true;
}

static inline uint64_t *
d3dkmt_dirty_workset(struct agx_device *dev)
{
   return dev->d3dkmt_worksets;
}

static inline uint64_t *
d3dkmt_conservative_workset(struct agx_device *dev)
{
   return dev->d3dkmt_worksets == NULL
             ? NULL
             : dev->d3dkmt_worksets + dev->d3dkmt_workset_words;
}

static inline bool
d3dkmt_workset_write(uint64_t *workset, uint32_t words, uint32_t handle,
                     bool value)
{
   uint32_t word = handle / 64u;
   uint64_t bit = UINT64_C(1) << (handle % 64u);

   if (workset == NULL || word >= words)
      return false;
   if (value)
      workset[word] |= bit;
   else
      workset[word] &= ~bit;
   return true;
}

static inline bool
d3dkmt_bo_requires_conservative_clean(const struct agx_bo *bo)
{
   return bo->d3dkmt_cpu_map.conservative ||
          bo->d3dkmt_origin == AGX_D3DKMT_BO_IMPORTED_EXTERNAL ||
          bo->d3dkmt_origin == AGX_D3DKMT_BO_EXPORTED;
}

static void
d3dkmt_bo_publish_work_locked(struct agx_device *dev, struct agx_bo *bo)
{
   const bool dirty = agx_d3dkmt_dirty_has_pending(&bo->d3dkmt_dirty);
   const bool conservative = d3dkmt_bo_requires_conservative_clean(bo);

   (void)d3dkmt_workset_write(d3dkmt_dirty_workset(dev),
                              dev->d3dkmt_workset_words, bo->handle, dirty);
   (void)d3dkmt_workset_write(d3dkmt_conservative_workset(dev),
                              dev->d3dkmt_workset_words, bo->handle,
                              conservative);
}

static void
d3dkmt_bo_remove_work_locked(struct agx_device *dev, struct agx_bo *bo)
{
   (void)d3dkmt_workset_write(d3dkmt_dirty_workset(dev),
                              dev->d3dkmt_workset_words, bo->handle, false);
   (void)d3dkmt_workset_write(d3dkmt_conservative_workset(dev),
                              dev->d3dkmt_workset_words, bo->handle, false);
}

/* -------------------------------------------------------------------------
 * GEM_CREATE / GEM_CLOSE.
 * ------------------------------------------------------------------------- */
static void
d3dkmt_destroy_record(struct agx_d3dkmt_winsys *ws,
                      const struct agx_d3dkmt_allocation_record *record)
{
   if (record == NULL ||
       (record->allocation == 0 && record->resource == 0))
      return;

   /* The outer D3D runtime owns same-process imports. This winsys owns only
    * the BO wrapper and GPUVA binding, so every unwind/free path must stop
    * before DestroyAllocation2. */
   if (record->borrowed)
      return;

   const agx_kmt_handle list[1] = {record->allocation};
   struct agx_kmt_destroyallocation2 destroy = {
      .hDevice = ws->device,
      .hResource = record->resource,
      .phAllocationList = record->resource ? NULL : list,
      .AllocationCount = record->resource ? 0 : 1,
   };

   agx_kmt_status st = ws->fns.DestroyAllocation2(&destroy);
   if (!AGX_KMT_SUCCESS(st))
      agx_d3dkmt_fail(st);
}

static bool
d3dkmt_make_resident(struct agx_d3dkmt_winsys *ws,
                     agx_kmt_handle allocation)
{
   if (!ws->policy.needs_residency)
      return true;

   const agx_kmt_handle list[1] = {allocation};
   struct agx_kmt_makeresident mr = {
      .hPagingQueue = ws->paging_queue,
      .NumAllocations = 1,
      .AllocationList = list,
   };

   agx_kmt_status st = ws->fns.MakeResident(&mr);
   if (!AGX_KMT_SUCCESS(st)) {
      /* Loud on purpose, matching the UpdateGpuVirtualAddress diagnostic
       * below: MakeResident was the only kernel refusal on this path that
       * failed without naming itself, which cost a hardware cycle. A
       * 0xC000000D here for an allocation this winsys did not create is
       * dxgkrnl's per-device handle scoping, not a KMD or VidMm failure. */
      fprintf(stderr,
              "D3DKMTMakeResident failed status=0x%08x paging_queue=0x%08x "
              "allocation=0x%08x\n",
              (unsigned)st, (unsigned)ws->paging_queue,
              (unsigned)allocation);
      agx_d3dkmt_fail(st);
      return false;
   }

   int wait_result = agx_d3dkmt_wait_paging(ws, mr.PagingFenceValue);
   agx_d3dkmt_bridge_log_paging_wait(
      ws, AGX_D3DKMT_BRIDGE_WAIT_MAKE_RESIDENT,
      mr.PagingFenceValue, wait_result);
   return wait_result == 0;
}

static bool
d3dkmt_create_allocation(struct agx_d3dkmt_winsys *ws, uint64_t size,
                         enum agx_bo_flags flags,
                         enum agx_d3dkmt_external_memory_type external_type,
                         struct agx_d3dkmt_allocation_record *record)
{
   const uint64_t allocation_granularity = UINT64_C(0x10000);
   if (size == 0 || size > UINT64_MAX - (allocation_granularity - 1)) {
      errno = EINVAL;
      return false;
   }
   size = ALIGN_POT(size, allocation_granularity);

   /*
    * CONTRACT.md SS12.1: the private-data size is pinned, and treated as a
    * memory-safety property rather than a convenience, because the D3D
    * runtime does not validate what the UMD claims against what the kernel
    * reads. Assert the width; never infer it.
    */
   struct agxw_alloc_private priv = {
      .version = AGXW_ALLOC_PRIVATE_VERSION,
      .struct_size = sizeof(priv),
      .size = size,
      .flags = agx_d3dkmt_agxw_alloc_flags((uint32_t)flags),
      .align_log2 = 16,
   };
   static_assert(sizeof(priv) == AGXW_ALLOC_PRIVATE_SIZE,
                 "allocation private data width");

   struct agx_kmt_allocationinfo2 info = {
      .pPrivateDriverData = &priv,
      .PrivateDriverDataSize = sizeof(priv),
   };

   /* Honeykrisp has no resource/allocation split at this layer, so the
    * resource-level blob is absent -- explicitly legal (CONTRACT.md SS3.2). */
   struct agx_kmt_createallocation create = {
      .hDevice = ws->device,
      .NumAllocations = 1,
      .pAllocationInfo2 = &info,
      .Flags = agx_d3dkmt_create_flags_external((uint32_t)flags,
                                                external_type),
   };

   agx_kmt_status st = ws->fns.CreateAllocation2(&create);
   if (!AGX_KMT_SUCCESS(st)) {
      agx_d3dkmt_fail(st);
      return false;
   }

   *record = (struct agx_d3dkmt_allocation_record){
      .allocation = info.hAllocation,
      .resource = create.hResource,
      .global_share = create.hGlobalShare,
      .external_type = external_type,
   };

   bool expected_resource =
      external_type != AGX_D3DKMT_EXTERNAL_MEMORY_NONE;
   bool expected_global =
      external_type == AGX_D3DKMT_EXTERNAL_MEMORY_OPAQUE_WIN32_KMT;
   /* Bridge-mode export degrade (degraded-create tolerance): an exportable
    * create with NO Triton resource handoff pending is converted by the
    * bridge into a plain VM-private allocation rather than refused
    * (0.1.339: 475x REFUSED no-handoff was every compositor allocation
    * failure). The record is then legitimately resourceless; accept it and
    * drop the external type so a later export attempt fails cleanly at
    * bo_export (record.resource == 0) instead of here. Non-bridge builds
    * keep the strict shape check. */
   if (agxw_bridge_is_sentinel(ws->device) && expected_resource &&
       record->allocation != 0 && record->resource == 0 &&
       record->global_share == 0) {
      record->external_type = AGX_D3DKMT_EXTERNAL_MEMORY_NONE;
      expected_resource = false;
      expected_global = false;
   }
   if (record->allocation == 0 || (!!record->resource != expected_resource) ||
       (!!record->global_share != expected_global)) {
      errno = EIO;
      d3dkmt_destroy_record(ws, record);
      memset(record, 0, sizeof(*record));
      return false;
   }

   if (!d3dkmt_make_resident(ws, record->allocation)) {
      d3dkmt_destroy_record(ws, record);
      memset(record, 0, sizeof(*record));
      return false;
   }

   return true;
}

static struct agx_bo *
d3dkmt_adopt_allocation(
   struct agx_device *dev, size_t size, size_t align, enum agx_bo_flags flags,
   const struct agx_d3dkmt_allocation_record *record)
{
   struct agx_d3dkmt_winsys *ws = agx_d3dkmt_ws(dev);
   struct agx_bo *bo;

   simple_mtx_lock(&ws->lock);
   uint32_t handle = agx_d3dkmt_pool_alloc_record(&ws->pool, record);
   simple_mtx_unlock(&ws->lock);

   if (handle == 0) {
      d3dkmt_destroy_record(ws, record);
      return NULL;
   }
   if (handle == UINT32_MAX) {
      simple_mtx_lock(&ws->lock);
      struct agx_d3dkmt_allocation_record released;
      bool found =
         agx_d3dkmt_pool_release_record(&ws->pool, handle, &released);
      simple_mtx_unlock(&ws->lock);
      if (found)
         d3dkmt_destroy_record(ws, &released);
      errno = ENOSPC;
      return NULL;
   }

   simple_mtx_lock(&dev->bo_map_lock);
   simple_mtx_lock(&dev->d3dkmt_dirty_lock);
   if (!d3dkmt_worksets_reserve_locked(dev, handle + 1u)) {
      simple_mtx_unlock(&dev->d3dkmt_dirty_lock);
      simple_mtx_unlock(&dev->bo_map_lock);
      simple_mtx_lock(&ws->lock);
      struct agx_d3dkmt_allocation_record released;
      bool found =
         agx_d3dkmt_pool_release_record(&ws->pool, handle, &released);
      simple_mtx_unlock(&ws->lock);
      if (found)
         d3dkmt_destroy_record(ws, &released);
      errno = ENOMEM;
      return NULL;
   }
   bo = agx_lookup_bo(dev, handle);
   /* max_handle is consumed as an exclusive bound throughout agx_bo.c and
    * submit read-set construction. Dense D3DKMT handle 1 must therefore make
    * the bound 2, not 1. */
   dev->max_handle = MAX2(dev->max_handle, handle + 1);
   simple_mtx_unlock(&dev->d3dkmt_dirty_lock);
   simple_mtx_unlock(&dev->bo_map_lock);

   /* util_sparse_array_get grows its node tree on demand and returns NULL on
    * allocation failure; release builds compile the zero-slot assert out, so
    * without this guard the field writes below would be the process's first
    * dereference of NULL (adopt-null-bo hardening, after the 0.1.340 LogonUI
    * access violations in this flow). Fail the allocation cleanly instead. */
   if (bo == NULL) {
      simple_mtx_lock(&ws->lock);
      struct agx_d3dkmt_allocation_record released;
      bool found =
         agx_d3dkmt_pool_release_record(&ws->pool, handle, &released);
      simple_mtx_unlock(&ws->lock);
      if (found)
         d3dkmt_destroy_record(ws, &released);
      fprintf(stderr, "d3dkmt_adopt_allocation: BO map slot unavailable\n");
      return NULL;
   }

   assert(!memcmp(bo, &((struct agx_bo){}), sizeof(*bo)));
   bo->dev = dev;
   bo->size = size;
   bo->align = align;
   bo->flags = flags;
   bo->handle = handle;
   bo->uapi_handle = record->allocation;
   bo->prime_fd = -1;

   enum agx_va_flags va_flags = flags & AGX_BO_LOW_VA ? AGX_VA_USC : 0;
   bo->va = agx_va_alloc(dev, size, bo->align, va_flags, 0);
   if (!bo->va) {
      fprintf(stderr, "Failed to allocate BO VMA\n");
      agx_bo_free(dev, bo);
      return NULL;
   }

   uint32_t bind = DRM_ASAHI_BIND_READ;
   if (!(flags & AGX_BO_READONLY))
      bind |= DRM_ASAHI_BIND_WRITE;

   if (agx_bo_bind(dev, bo, bo->va->addr, bo->size, 0, bind)) {
      agx_bo_free(dev, bo);
      return NULL;
   }

   return bo;
}

/*
 * Clone of agx_bo_alloc()/win_bo_alloc() with GEM_CREATE replaced by
 * D3DKMTCreateAllocation2. The handle split is the only structural change:
 * bo->handle stays a dense index into dev->bo_map (see agx_d3dkmt_core.h),
 * bo->uapi_handle carries the D3DKMT allocation handle that every kernel-
 * facing call site already passes.
 */
static struct agx_bo *
d3dkmt_bo_alloc(struct agx_device *dev, size_t size, size_t align,
                enum agx_bo_flags flags)
{
   struct agx_d3dkmt_winsys *ws = agx_d3dkmt_ws(dev);

   /* executable implies low va */
   assert(!(flags & AGX_BO_EXEC) || (flags & AGX_BO_LOW_VA));
   flags = (enum agx_bo_flags)agx_d3dkmt_cpu_visible_flags((uint32_t)flags);

   struct agx_d3dkmt_allocation_record record;
   enum agx_d3dkmt_external_memory_type external_type =
      (flags & AGX_BO_SHAREABLE)
         ? AGX_D3DKMT_EXTERNAL_MEMORY_OPAQUE_WIN32
         : AGX_D3DKMT_EXTERNAL_MEMORY_NONE;
   if (!d3dkmt_create_allocation(ws, size, flags,
                                 external_type, &record)) {
      fprintf(stderr, "D3DKMTCreateAllocation2 failed\n");
      return NULL;
   }

   struct agx_bo *bo =
      d3dkmt_adopt_allocation(dev, size, align, flags, &record);
   return bo;
}

/*
 * GEM_CLOSE. Takes the DENSE handle, because agx_bo_free() memsets struct
 * agx_bo to zero before calling this (agx_device.c:100-105, deliberately, to
 * avoid a lookup race) -- so bo->uapi_handle is already gone and the pool is
 * the only remaining way to recover the D3DKMT handle.
 */
static void
d3dkmt_bo_free(struct agx_device *dev, uint32_t handle)
{
   struct agx_d3dkmt_winsys *ws = agx_d3dkmt_ws(dev);

   simple_mtx_lock(&ws->lock);
   struct agx_d3dkmt_allocation_record record;
   bool found = agx_d3dkmt_pool_release_record(&ws->pool, handle, &record);
   simple_mtx_unlock(&ws->lock);

   if (!found)
      return;

   d3dkmt_destroy_record(ws, &record);
}

/* A shared resource opened here may have been created by the outer D3D
 * runtime (pfnAllocateCb with an hResource) or by the OS as a standard
 * allocation. Both attach blobs Honeykrisp never consumes: the runtime's
 * resource metadata and the UMD/KMD resource-level driver blob. They must be
 * received (dxgkrnl requires the open buffers to match the queried sizes
 * exactly) but carry no meaning here, so they are size-bounded and dropped.
 * The load-bearing validation stays the KMD's pinned 32-byte AGXW allocation
 * blob, checked in d3dkmt_external_open(). */
#define AGX_D3DKMT_FOREIGN_BLOB_MAX 4096u

struct agx_d3dkmt_external_sizes {
   uint32_t runtime_size;
   uint32_t resource_driver_size;
};

static bool
d3dkmt_external_query(struct agx_d3dkmt_winsys *ws, uintptr_t handle,
                      enum agx_d3dkmt_external_memory_type external_type,
                      struct agx_d3dkmt_external_sizes *sizes)
{
   uint32_t runtime_size = 0;
   uint32_t total_driver_size = 0;
   uint32_t resource_driver_size = 0;
   uint32_t allocation_count = 0;
   agx_kmt_status st;

   if (external_type == AGX_D3DKMT_EXTERNAL_MEMORY_OPAQUE_WIN32) {
      if (handle == 0)
         return false;
      struct agx_kmt_queryresourceinfofromnthandle query = {
         .hDevice = ws->device,
         .hNtHandle = (void *)handle,
      };
      st = ws->fns.QueryResourceInfoFromNtHandle(&query);
      runtime_size = query.PrivateRuntimeDataSize;
      total_driver_size = query.TotalPrivateDriverDataSize;
      resource_driver_size = query.ResourcePrivateDriverDataSize;
      allocation_count = query.NumAllocations;
   } else if (external_type ==
              AGX_D3DKMT_EXTERNAL_MEMORY_OPAQUE_WIN32_KMT) {
      if (handle == 0 || handle > UINT32_MAX)
         return false;
      struct agx_kmt_queryresourceinfo query = {
         .hDevice = ws->device,
         .hGlobalShare = (agx_kmt_handle)handle,
      };
      st = ws->fns.QueryResourceInfo(&query);
      runtime_size = query.PrivateRuntimeDataSize;
      total_driver_size = query.TotalPrivateDriverDataSize;
      resource_driver_size = query.ResourcePrivateDriverDataSize;
      allocation_count = query.NumAllocations;
   } else {
      errno = EINVAL;
      return false;
   }

   if (!AGX_KMT_SUCCESS(st)) {
      agx_d3dkmt_fail(st);
      return false;
   }

   /* This miniport's source-pinned contract is one allocation per resource
    * carrying exactly one AGXW allocation blob. Runtime and resource-level
    * blobs are tolerated within a hard bound so that D3D-runtime-created and
    * standard-allocation shared resources can be adopted; their content is
    * never interpreted. Refuse anything else before supplying output buffers
    * to OpenResource. */
   if (allocation_count != AGXW_MAX_ALLOCATIONS_PER_RESOURCE ||
       runtime_size > AGX_D3DKMT_FOREIGN_BLOB_MAX ||
       resource_driver_size > AGX_D3DKMT_FOREIGN_BLOB_MAX ||
       total_driver_size != AGXW_ALLOC_PRIVATE_SIZE) {
      errno = EINVAL;
      return false;
   }

   sizes->runtime_size = runtime_size;
   sizes->resource_driver_size = resource_driver_size;
   return true;
}

static bool
d3dkmt_external_open(
   struct agx_d3dkmt_winsys *ws, uintptr_t handle,
   enum agx_d3dkmt_external_memory_type external_type,
   struct agx_d3dkmt_allocation_record *record,
   struct agxw_alloc_private *private_data)
{
   struct agx_d3dkmt_external_sizes sizes = {0};
   if (!d3dkmt_external_query(ws, handle, external_type, &sizes))
      return false;

   uint8_t total_driver_data[AGXW_ALLOC_PRIVATE_SIZE] = {0};
   struct agx_kmt_openallocationinfo2 info = {0};
   agx_kmt_handle resource = 0;
   agx_kmt_status st;

   /* dxgkrnl refuses an open whose receive buffers do not match the queried
    * blob sizes, so the foreign blobs are received into bounded scratch and
    * then discarded unread. Heap, not stack: this runs inside
    * vkAllocateMemory, and DWM is known to call in on threads whose stacks
    * cannot take multi-KiB frames (see 0054-heap-agx-parallel-copy-context).
    */
   uint8_t *foreign_blobs = NULL;
   if (sizes.runtime_size > 0 || sizes.resource_driver_size > 0) {
      foreign_blobs = calloc(
         1, (size_t)sizes.runtime_size + sizes.resource_driver_size);
      if (foreign_blobs == NULL) {
         errno = ENOMEM;
         return false;
      }
   }
   void *runtime_buf = sizes.runtime_size ? foreign_blobs : NULL;
   void *resource_buf =
      sizes.resource_driver_size ? foreign_blobs + sizes.runtime_size : NULL;

   if (external_type == AGX_D3DKMT_EXTERNAL_MEMORY_OPAQUE_WIN32) {
      struct agx_kmt_openresourcefromnthandle open = {
         .hDevice = ws->device,
         .hNtHandle = (void *)handle,
         .NumAllocations = AGXW_MAX_ALLOCATIONS_PER_RESOURCE,
         .pOpenAllocationInfo2 = &info,
         .PrivateRuntimeDataSize = sizes.runtime_size,
         .pPrivateRuntimeData = runtime_buf,
         .ResourcePrivateDriverDataSize = sizes.resource_driver_size,
         .pResourcePrivateDriverData = resource_buf,
         .TotalPrivateDriverDataBufferSize = sizeof(total_driver_data),
         .pTotalPrivateDriverDataBuffer = total_driver_data,
      };
      st = ws->fns.OpenResourceFromNtHandle(&open);
      resource = open.hResource;
      if (AGX_KMT_SUCCESS(st) &&
          (open.hKeyedMutex != 0 || open.hSyncObject != 0)) {
         errno = ENOTSUP;
         st = (agx_kmt_status)0xc00000bbu; /* STATUS_NOT_SUPPORTED */
      }
   } else {
      struct agx_kmt_openresource open = {
         .hDevice = ws->device,
         .hGlobalShare = (agx_kmt_handle)handle,
         .NumAllocations = AGXW_MAX_ALLOCATIONS_PER_RESOURCE,
         .pOpenAllocationInfo2 = &info,
         .pPrivateRuntimeData = runtime_buf,
         .PrivateRuntimeDataSize = sizes.runtime_size,
         .pResourcePrivateDriverData = resource_buf,
         .ResourcePrivateDriverDataSize = sizes.resource_driver_size,
         .pTotalPrivateDriverDataBuffer = total_driver_data,
         .TotalPrivateDriverDataBufferSize = sizeof(total_driver_data),
      };
      st = ws->fns.OpenResource(&open);
      resource = open.hResource;
   }

   free(foreign_blobs);

   if (!AGX_KMT_SUCCESS(st)) {
      fprintf(stderr,
              "D3DKMT external open failed status=0x%08x type=%d "
              "runtime_size=%u resource_driver_size=%u\n",
              (unsigned)st, (int)external_type, sizes.runtime_size,
              sizes.resource_driver_size);
      if (resource != 0) {
         const struct agx_d3dkmt_allocation_record opened = {
            .allocation = info.hAllocation,
            .resource = resource,
         };
         d3dkmt_destroy_record(ws, &opened);
      }
      agx_d3dkmt_fail(st);
      return false;
   }

   const uintptr_t data_begin = (uintptr_t)total_driver_data;
   const uintptr_t data_end = data_begin + sizeof(total_driver_data);
   const uintptr_t private_begin = (uintptr_t)info.pPrivateDriverData;
   if (resource == 0 || info.hAllocation == 0 ||
       info.PrivateDriverDataSize != AGXW_ALLOC_PRIVATE_SIZE ||
       private_begin < data_begin || private_begin > data_end ||
       AGXW_ALLOC_PRIVATE_SIZE > data_end - private_begin) {
      errno = EINVAL;
      const struct agx_d3dkmt_allocation_record opened = {
         .allocation = info.hAllocation,
         .resource = resource,
      };
      d3dkmt_destroy_record(ws, &opened);
      return false;
   }

   memcpy(private_data, info.pPrivateDriverData, sizeof(*private_data));
   if (agxw_alloc_private_validate(private_data, sizeof(*private_data)) != 0 ||
       !(private_data->flags & AGXW_ALLOC_SHAREABLE) ||
       (private_data->flags & AGXW_ALLOC_VM_PRIVATE) ||
       private_data->size == 0 ||
       private_data->size > SIZE_MAX - (AIL_PAGESIZE - 1)) {
      errno = EINVAL;
      const struct agx_d3dkmt_allocation_record opened = {
         .allocation = info.hAllocation,
         .resource = resource,
      };
      d3dkmt_destroy_record(ws, &opened);
      return false;
   }

   *record = (struct agx_d3dkmt_allocation_record){
      .allocation = info.hAllocation,
      .resource = resource,
      .global_share =
         external_type == AGX_D3DKMT_EXTERNAL_MEMORY_OPAQUE_WIN32_KMT
            ? (agx_kmt_handle)handle
            : 0,
      .external_type = external_type,
   };

   if (!d3dkmt_make_resident(ws, record->allocation)) {
      d3dkmt_destroy_record(ws, record);
      memset(record, 0, sizeof(*record));
      return false;
   }

   return true;
}

struct agx_bo *
agx_d3dkmt_bo_create_external(
   struct agx_device *dev, size_t size, unsigned align,
   enum agx_bo_flags flags,
   enum agx_d3dkmt_external_memory_type external_type, const char *label)
{
   if (external_type == AGX_D3DKMT_EXTERNAL_MEMORY_NONE ||
       external_type > AGX_D3DKMT_EXTERNAL_MEMORY_OPAQUE_WIN32_KMT ||
       size == 0 || size > SIZE_MAX - (AIL_PAGESIZE - 1)) {
      errno = EINVAL;
      return NULL;
   }

   size = ALIGN_POT(size, AIL_PAGESIZE);
   align = MAX2(align, AIL_PAGESIZE);
   flags |= AGX_BO_SHARED | AGX_BO_SHAREABLE;
   flags = (enum agx_bo_flags)agx_d3dkmt_cpu_visible_flags((uint32_t)flags);

   struct agx_d3dkmt_winsys *ws = agx_d3dkmt_ws(dev);
   struct agx_d3dkmt_allocation_record record;
   if (!d3dkmt_create_allocation(ws, size, flags, external_type, &record))
      return NULL;

   struct agx_bo *bo =
      d3dkmt_adopt_allocation(dev, size, align, flags, &record);
   if (bo != NULL) {
      bo->label = label;
      agx_bo_reset_cpu_dirty(bo);
      simple_mtx_lock(&dev->d3dkmt_dirty_lock);
      bo->d3dkmt_origin = AGX_D3DKMT_BO_CREATED_SHAREABLE;
      d3dkmt_bo_publish_work_locked(dev, bo);
      simple_mtx_unlock(&dev->d3dkmt_dirty_lock);
      p_atomic_set(&bo->refcnt, 1);
      p_atomic_inc(&dev->read_set_generation);
   }
   return bo;
}

struct agx_bo *
agx_d3dkmt_bo_import_external(
   struct agx_device *dev, uintptr_t handle,
   enum agx_d3dkmt_external_memory_type external_type, const char *label)
{
   struct agx_d3dkmt_winsys *ws = agx_d3dkmt_ws(dev);
   struct agx_d3dkmt_allocation_record record;
   struct agxw_alloc_private private_data;
   if (!d3dkmt_external_open(ws, handle, external_type, &record,
                             &private_data))
      return NULL;

   enum agx_bo_flags flags = AGX_BO_SHARED | AGX_BO_SHAREABLE;
   if (private_data.flags & AGXW_ALLOC_WRITEBACK)
      flags |= AGX_BO_WRITEBACK;
   const size_t size = ALIGN_POT((size_t)private_data.size, AIL_PAGESIZE);

   struct agx_bo *bo =
      d3dkmt_adopt_allocation(dev, size, AIL_PAGESIZE, flags, &record);
   if (bo != NULL) {
      bo->label = label;
      agx_bo_reset_cpu_dirty(bo);
      simple_mtx_lock(&dev->d3dkmt_dirty_lock);
      bo->d3dkmt_origin = AGX_D3DKMT_BO_IMPORTED_EXTERNAL;
      (void)agx_d3dkmt_dirty_mark_whole(&bo->d3dkmt_dirty, bo->size);
      d3dkmt_bo_publish_work_locked(dev, bo);
      simple_mtx_unlock(&dev->d3dkmt_dirty_lock);
      p_atomic_set(&bo->refcnt, 1);
      p_atomic_inc(&dev->read_set_generation);
   }
   return bo;
}

struct agx_bo *
agx_d3dkmt_bo_import_local(struct agx_device *dev,
                           agx_kmt_handle allocation, size_t size,
                           const char *label)
{
   if (allocation == 0 || size == 0 ||
       size > SIZE_MAX - (AIL_PAGESIZE - 1)) {
      errno = EINVAL;
      return NULL;
   }

   size = ALIGN_POT(size, AIL_PAGESIZE);
   struct agx_d3dkmt_winsys *ws = agx_d3dkmt_ws(dev);
   const struct agx_d3dkmt_allocation_record record = {
      .allocation = allocation,
      .external_type = AGX_D3DKMT_EXTERNAL_MEMORY_OPAQUE_WIN32_KMT,
      .borrowed = true,
   };

   if (!d3dkmt_make_resident(ws, allocation)) {
      fprintf(stderr,
              "borrowed local import: MakeResident refused foreign "
              "allocation=0x%08x errno=%d\n",
              (unsigned)allocation, errno);
      return NULL;
   }

   const enum agx_bo_flags flags = AGX_BO_SHARED | AGX_BO_SHAREABLE;
   struct agx_bo *bo = d3dkmt_adopt_allocation(
      dev, size, AIL_PAGESIZE, flags, &record);
   if (bo == NULL) {
      fprintf(stderr,
              "borrowed local import: adopt/bind failed for foreign "
              "allocation=0x%08x errno=%d\n",
              (unsigned)allocation, errno);
   }
   if (bo != NULL) {
      bo->label = label;
      agx_bo_reset_cpu_dirty(bo);
      simple_mtx_lock(&dev->d3dkmt_dirty_lock);
      bo->d3dkmt_origin = AGX_D3DKMT_BO_IMPORTED_LOCAL;
      d3dkmt_bo_publish_work_locked(dev, bo);
      simple_mtx_unlock(&dev->d3dkmt_dirty_lock);
      p_atomic_set(&bo->refcnt, 1);
      p_atomic_inc(&dev->read_set_generation);
   }
   return bo;
}

int
agx_d3dkmt_bo_export_external(
   struct agx_device *dev, struct agx_bo *bo,
   enum agx_d3dkmt_external_memory_type external_type, uintptr_t *handle)
{
   if (bo == NULL || handle == NULL ||
       external_type == AGX_D3DKMT_EXTERNAL_MEMORY_NONE ||
       external_type > AGX_D3DKMT_EXTERNAL_MEMORY_OPAQUE_WIN32_KMT) {
      errno = EINVAL;
      return -1;
   }

   struct agx_d3dkmt_winsys *ws = agx_d3dkmt_ws(dev);
   struct agx_d3dkmt_allocation_record record;
   simple_mtx_lock(&ws->lock);
   bool found =
      agx_d3dkmt_pool_get_record(&ws->pool, bo->handle, &record);
   simple_mtx_unlock(&ws->lock);
   if (!found || record.allocation != bo->uapi_handle ||
       record.external_type != external_type || record.resource == 0) {
      {
         extern void agx_bridge_valog(const char *fmt, ...);
         agx_bridge_valog(
            "bo_export_external REFUSED found=%d record.allocation=0x%llx "
            "bo.uapi=0x%llx record.external_type=%d want_external_type=%d "
            "record.resource=0x%llx bo.size=%llu",
            found ? 1 : 0,
            (unsigned long long)record.allocation,
            (unsigned long long)bo->uapi_handle,
            (int)record.external_type, (int)external_type,
            (unsigned long long)record.resource,
            (unsigned long long)bo->size);
      }
      errno = EINVAL;
      return -1;
   }

   if (external_type == AGX_D3DKMT_EXTERNAL_MEMORY_OPAQUE_WIN32_KMT) {
      if (record.global_share == 0) {
         errno = EINVAL;
         return -1;
      }
      /* Non-owning by Vulkan definition: return the stable identifier itself,
       * not a duplicated/closable kernel handle. */
      *handle = (uintptr_t)record.global_share;
   } else {
      OBJECT_ATTRIBUTES attributes;
      InitializeObjectAttributes(&attributes, NULL, 0, NULL, NULL);
      void *nt_handle = NULL;
      agx_kmt_status st = ws->fns.ShareObjects(
         1, &record.resource, &attributes,
         AGX_KMT_SHARED_ALLOCATION_ALL_ACCESS, &nt_handle);
      if (!AGX_KMT_SUCCESS(st) || nt_handle == NULL) {
         if (AGX_KMT_SUCCESS(st))
            errno = EIO;
         else
            agx_d3dkmt_fail(st);
         return -1;
      }

      /* The caller owns this NT handle and closes it. The BO retains no copy. */
      *handle = (uintptr_t)nt_handle;
   }

   /* Once an allocation leaves this process, a foreign persistent mapping can
    * write without a Honeykrisp callback. Keep it conservative until a shared
    * write-epoch protocol exists across the export boundary. */
   simple_mtx_lock(&dev->d3dkmt_dirty_lock);
   bo->d3dkmt_origin = AGX_D3DKMT_BO_EXPORTED;
   (void)agx_d3dkmt_cpu_map_request(
      &bo->d3dkmt_cpu_map, &bo->d3dkmt_dirty, bo->size,
      AGX_D3DKMT_CPU_MAP_RAW, false);
   d3dkmt_bo_publish_work_locked(dev, bo);
   simple_mtx_unlock(&dev->d3dkmt_dirty_lock);
   return 0;
}

/* -------------------------------------------------------------------------
 * VM_BIND.
 * ------------------------------------------------------------------------- */
static bool enum_log_enabled(void);

/*
 * This path runs only for an invariant that would otherwise cross into VidMm
 * and can bugcheck the machine.  Emit the same bounded record to stderr and
 * OutputDebugString, then best-effort append the first failure in a process
 * to ProgramData.  The file is intentionally fixed and append-only so a DWM
 * failure remains inspectable after that process is restarted.
 */
static void
d3dkmt_gpuva_guard_report(const char *reason, const struct agx_device *dev,
                          const struct agx_d3dkmt_winsys *ws,
                          const struct agx_d3dkmt_bind *binds,
                          uint32_t bind_count,
                          const struct agx_kmt_updategpuva_op *ops,
                          uint32_t op_count)
{
   static volatile LONG persisted;
   const struct agx_d3dkmt_bind *bind = bind_count > 0 ? &binds[0] : NULL;
   const struct agx_kmt_updategpuva_op *op = op_count > 0 ? &ops[0] : NULL;
   SYSTEMTIME now;
   char message[1024];

   GetSystemTime(&now);
   int length = snprintf(
      message, sizeof(message),
      "NTAGX_GPUVA_GUARD utc=%04u-%02u-%02uT%02u:%02u:%02u.%03uZ "
      "pid=%lu tid=%lu reason=%s shader=0x%llx policy_base=0x%llx "
      "policy_size=0x%llx binds=%u bind0_addr=0x%llx "
      "bind0_range=0x%llx bind0_flags=0x%x ops=%u op0_type=%d "
      "op0_base=0x%llx op0_size=0x%llx op0_prot=0x%llx "
      "op0_driver=0x%llx\n",
      (unsigned)now.wYear, (unsigned)now.wMonth, (unsigned)now.wDay,
      (unsigned)now.wHour, (unsigned)now.wMinute, (unsigned)now.wSecond,
      (unsigned)now.wMilliseconds, (unsigned long)GetCurrentProcessId(),
      (unsigned long)GetCurrentThreadId(), reason,
      (unsigned long long)(dev ? dev->shader_base : 0),
      (unsigned long long)(ws ? ws->policy.usc_base : 0),
      (unsigned long long)(ws ? ws->policy.usc_size : 0), bind_count,
      (unsigned long long)(bind ? bind->addr : 0),
      (unsigned long long)(bind ? bind->range : 0), bind ? bind->flags : 0,
      op_count, op ? op->OperationType : -1,
      (unsigned long long)(op ? op->MapProtect.BaseAddress : 0),
      (unsigned long long)(op ? op->MapProtect.SizeInBytes : 0),
      (unsigned long long)(op ? op->MapProtect.Protection.Value : 0),
      (unsigned long long)(op ? op->MapProtect.DriverProtection : 0));
   if (length < 0)
      return;

   const DWORD bytes =
      (DWORD)MIN2((size_t)length, sizeof(message) - (size_t)1);
   fputs(message, stderr);
   OutputDebugStringA(message);

   if (InterlockedCompareExchange(&persisted, 1, 0) != 0)
      return;

   const char directory[] = "C:\\ProgramData\\AuroraSilicon";
   const char path[] =
      "C:\\ProgramData\\AuroraSilicon\\honeykrisp-gpuva-guard.log";
   if (!CreateDirectoryA(directory, NULL) &&
       GetLastError() != ERROR_ALREADY_EXISTS)
      return;

   HANDLE file = CreateFileA(path, FILE_APPEND_DATA,
                             FILE_SHARE_READ | FILE_SHARE_WRITE |
                                FILE_SHARE_DELETE,
                             NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
   if (file == INVALID_HANDLE_VALUE)
      return;

   DWORD written;
   (void)WriteFile(file, message, bytes, &written, NULL);
   CloseHandle(file);
}

static int
d3dkmt_va_update_init(struct agx_d3dkmt_winsys *ws)
{
   if (ws->va_context != 0 && ws->va_fence != 0)
      return 0;
   if (ws->va_context != 0 || ws->va_fence != 0 ||
       ws->policy.usc_size == 0) {
      errno = EINVAL;
      return -1;
   }

   struct agx_kmt_createsyncobj2 fence = {
      .hDevice = ws->device,
      .Info = {
         .Type = AGX_KMT_SYNC_MONITORED_FENCE,
         .MonitoredFence = {
            .InitialFenceValue = 0,
            .EngineAffinity = 1,
         },
      },
   };
   agx_kmt_status st = ws->fns.CreateSynchronizationObject2(&fence);
   if (!AGX_KMT_SUCCESS(st))
      return agx_d3dkmt_fail(st);

   struct agxw_context_private priv = {
      .version = AGXW_CONTEXT_PRIVATE_VERSION,
      .struct_size = sizeof(priv),
      .priority = AGXW_PRIORITY_MEDIUM,
      .flags = AGXW_CONTEXT_PRIVATE_FLAG_VA_INTERLOCK,
      .usc_exec_base = ws->policy.usc_base,
   };
   if (agxw_context_private_validate(&priv, sizeof(priv), ws->policy.usc_base,
                                     ws->policy.usc_size) != 0) {
      struct agx_kmt_destroysyncobj d = {.hSyncObject = fence.hSyncObject};
      ws->fns.DestroySynchronizationObject(&d);
      errno = EINVAL;
      return -1;
   }

   /* CreateContextVirtual itself selects the GPU-VA KMD context. Do not set
    * SynchronizationOnly: AppleAgxWddm intentionally admits the same real
    * virtual-addressing context shape used by Honeykrisp queues, and
    * UpdateGpuVirtualAddress requires such a rendering context. */
   struct agx_kmt_createcontextvirtual create = {
      .hDevice = ws->device,
      .NodeOrdinal = ws->policy.node_ordinal,
      .EngineAffinity = 1,
      .pPrivateDriverData = &priv,
      .PrivateDriverDataSize = sizeof(priv),
   };
   st = ws->fns.CreateContextVirtual(&create);
   if (!AGX_KMT_SUCCESS(st)) {
      struct agx_kmt_destroysyncobj d = {.hSyncObject = fence.hSyncObject};
      ws->fns.DestroySynchronizationObject(&d);
      return agx_d3dkmt_fail(st);
   }

   ws->va_context = create.hContext;
   ws->va_fence = fence.hSyncObject;
   ws->va_fence_cpu =
      (const volatile uint64_t *)fence.Info.MonitoredFence
         .FenceValueCPUVirtualAddress;
   ws->va_fence_next = 0;
   if (enum_log_enabled())
      fprintf(stderr,
              "NTAGX_ENUM va_update_context=0x%08x fence=0x%08x cpu=%p\n",
              (unsigned)ws->va_context, (unsigned)ws->va_fence,
              (const void *)ws->va_fence_cpu);
   return 0;
}

static int
d3dkmt_bo_bind(struct agx_device *dev, struct drm_asahi_gem_bind_op *ops,
               uint32_t count)
{
   struct agx_d3dkmt_winsys *ws = agx_d3dkmt_ws(dev);
   struct agx_d3dkmt_policy bind_policy;
   struct agx_d3dkmt_bind stack_binds[8];
   struct agx_kmt_updategpuva_op stack_ops[16];
   struct agx_d3dkmt_bind *binds = stack_binds;
   struct agx_kmt_updategpuva_op *uops = stack_ops;
   uint32_t nops = 0;
   int ret = -1;

   if (count == 0)
      return 0;

   if (count > ARRAY_SIZE(stack_binds)) {
      binds = malloc(count * sizeof(*binds));
      if (!binds) {
         errno = ENOMEM;
         return -1;
      }
   }

   /* Resolve dense GEM handles to D3DKMT allocation handles. */
   simple_mtx_lock(&ws->lock);
   for (uint32_t i = 0; i < count; ++i) {
      const uint64_t granularity = UINT64_C(0x10000);
      uint64_t range = ops[i].range;

      if ((ops[i].addr & (granularity - 1)) != 0 ||
          (ops[i].offset & (granularity - 1)) != 0 || range == 0 ||
          range > UINT64_MAX - (granularity - 1)) {
         simple_mtx_unlock(&ws->lock);
         errno = EINVAL;
         goto out;
      }
      range = ALIGN_POT(range, granularity);
      binds[i] = (struct agx_d3dkmt_bind){
         .flags = ops[i].flags,
         .kmt_handle = ops[i].handle,
         .offset = ops[i].offset,
         .range = range,
         .addr = ops[i].addr,
      };
   }
   simple_mtx_unlock(&ws->lock);

   if (agx_d3dkmt_bind_policy(dev->shader_base, &ws->policy, &bind_policy) !=
       AGX_D3DKMT_OK) {
      d3dkmt_gpuva_guard_report("policy", dev, ws, binds, count, NULL, 0);
      errno = EIO;
      goto out;
   }

   if (agx_d3dkmt_translate_binds(binds, count, &bind_policy, NULL, 0,
                                  &nops) != AGX_D3DKMT_OK) {
      errno = EINVAL;
      goto out;
   }

   if (nops > ARRAY_SIZE(stack_ops)) {
      uops = malloc((size_t)nops * sizeof(*uops));
      if (!uops) {
         errno = ENOMEM;
         goto out;
      }
   }

   if (agx_d3dkmt_translate_binds(binds, count, &bind_policy, uops, nops,
                                  &nops) != AGX_D3DKMT_OK) {
      errno = EINVAL;
      goto out;
   }

   /*
    * The one real semantic change in this op. DRM_IOCTL_ASAHI_VM_BIND returns
    * with the binding in effect; D3DKMTUpdateGpuVirtualAddress queues page-
    * table work that completes on a monitored fence. agx_bo_alloc() maps and
    * then immediately writes through the mapping, so the winsys must restore
    * the synchronous contract rather than push asynchrony up into Honeykrisp.
    */
   simple_mtx_lock(&ws->lock);
   struct agx_d3dkmt_policy submit_policy;
   if (agx_d3dkmt_bind_policy(dev->shader_base, &ws->policy,
                              &submit_policy) != AGX_D3DKMT_OK ||
       agx_d3dkmt_validate_translated_binds(
          binds, count, &submit_policy, uops, nops) != AGX_D3DKMT_OK) {
      simple_mtx_unlock(&ws->lock);
      d3dkmt_gpuva_guard_report("pre-submit", dev, ws, binds, count, uops,
                                nops);
      errno = EIO;
      goto out;
   }
   if (ws->device_lost || ws->va_context == 0 || ws->va_fence == 0 ||
       ws->va_fence_next == UINT64_MAX) {
      simple_mtx_unlock(&ws->lock);
      errno = ws->device_lost ? EIO : EINVAL;
      goto out;
   }
   uint64_t fence_value = ws->va_fence_next;
   uint64_t completion_value = fence_value + 1;
   struct agx_kmt_updategpuva update = {
      .hDevice = ws->device,
      .hContext = ws->va_context,
      .hFenceObject = ws->va_fence,
      .NumOperations = nops,
      .Operations = uops,
      .FenceValue = fence_value,
      .Flags = 1u, /* D3DKMT_UPDATEGPUVIRTUALADDRESS::DoNotWait */
   };
   agx_kmt_status st = ws->fns.UpdateGpuVirtualAddress(&update);
   if (AGX_KMT_SUCCESS(st))
      ws->va_fence_next = completion_value;
   simple_mtx_unlock(&ws->lock);

   if (!AGX_KMT_SUCCESS(st)) {
      fprintf(stderr,
              "D3DKMTUpdateGpuVirtualAddress failed status=0x%08x "
              "device=0x%08x context=0x%08x fence=0x%08x "
              "fence_value=%llu nops=%u flags=0x%08x\n",
              (unsigned)st, (unsigned)update.hDevice,
              (unsigned)update.hContext, (unsigned)update.hFenceObject,
              (unsigned long long)update.FenceValue, update.NumOperations,
              update.Flags);
      for (uint32_t i = 0; i < nops; ++i) {
         fprintf(stderr,
                 "D3DKMTUpdateGpuVirtualAddress op[%u] type=%d "
                 "base=0x%llx size=0x%llx alloc=0x%08x "
                 "offset=0x%llx alloc_size=0x%llx prot=0x%llx "
                 "driver=0x%llx\n",
                 i, uops[i].OperationType,
                 (unsigned long long)uops[i].MapProtect.BaseAddress,
                 (unsigned long long)uops[i].MapProtect.SizeInBytes,
                 (unsigned)uops[i].MapProtect.hAllocation,
                 (unsigned long long)uops[i].MapProtect.AllocationOffsetInBytes,
                 (unsigned long long)uops[i].MapProtect.AllocationSizeInBytes,
                 (unsigned long long)uops[i].MapProtect.Protection.Value,
                 (unsigned long long)uops[i].MapProtect.DriverProtection);
      }
      ret = agx_d3dkmt_fail(st);
      goto out;
   }

   if (ws->va_fence_cpu == NULL || *ws->va_fence_cpu < completion_value)
      ret = agx_d3dkmt_wait_fence(ws, ws->va_fence, ws->va_fence_cpu,
                                  completion_value, AGX_D3DKMT_INFINITE);
   else
      ret = 0;
   agx_d3dkmt_bridge_log_paging_wait(
      ws, AGX_D3DKMT_BRIDGE_WAIT_UPDATE_GPUVA,
      completion_value, ret);
   if (ret != 0)
      agx_d3dkmt_mark_device_lost(ws, NULL);

out:
   if (binds != stack_binds)
      free(binds);
   if (uops != stack_ops)
      free(uops);
   return ret;
}

/* -------------------------------------------------------------------------
 * GEM_MMAP / GEM_MUNMAP.
 *
 * Linux does GEM_MMAP_OFFSET plus mmap(2); the AGXKMD port invented a
 * GEM_MMAP ioctl. WDDM already has the operation: D3DKMTLock2 returns a CPU
 * mapping of an allocation. It does NOT accept a placement hint, which is why
 * VK_EXT_map_memory_placed stays disabled on Windows (already the case,
 * patch 0007) -- agx_bo_map_placed()'s fixed_addr is unusable here and is
 * asserted away rather than silently ignored.
 * ------------------------------------------------------------------------- */
static void
d3dkmt_bo_mmap(struct agx_device *dev, struct agx_bo *bo, void *fixed_addr)
{
   struct agx_d3dkmt_winsys *ws = agx_d3dkmt_ws(dev);

   assert(bo->_map == NULL && "not double mapped");
   assert(fixed_addr == NULL &&
          "placed maps need VK_EXT_map_memory_placed, disabled on Windows");
   (void)fixed_addr;

   struct agx_kmt_lock2 lock = {
      .hDevice = ws->device,
      .hAllocation = bo->uapi_handle,
   };

   agx_kmt_status st = ws->fns.Lock2(&lock);
   if (!AGX_KMT_SUCCESS(st)) {
      bo->_map = NULL;
      agx_d3dkmt_fail(st);
      fprintf(stderr, "D3DKMTLock2 failed: handle=%u\n", bo->uapi_handle);
      return;
   }

   bo->_map = lock.pData;
}

static void
d3dkmt_bo_munmap(struct agx_device *dev, struct agx_bo *bo)
{
   struct agx_d3dkmt_winsys *ws = agx_d3dkmt_ws(dev);

   struct agx_kmt_unlock2 unlock = {
      .hDevice = ws->device,
      .hAllocation = bo->uapi_handle,
   };

   ws->fns.Unlock2(&unlock);
   bo->_map = NULL;
   if ((bo->flags & (AGX_BO_SHARED | AGX_BO_SHAREABLE)) == 0)
      p_atomic_inc(&dev->read_set_generation);

   /* Unmap invalidates this process's pointer. Preserve pending ownership for
    * the next submit; imported/exported origins remain conservative because a
    * foreign mapping may still exist. */
   simple_mtx_lock(&dev->d3dkmt_dirty_lock);
   if (bo->d3dkmt_cpu_map.conservative)
      (void)agx_d3dkmt_dirty_mark_whole(&bo->d3dkmt_dirty, bo->size);
   agx_d3dkmt_cpu_map_reset(&bo->d3dkmt_cpu_map, false);
   d3dkmt_bo_publish_work_locked(dev, bo);
   simple_mtx_unlock(&dev->d3dkmt_dirty_lock);
}

static int
d3dkmt_bo_sync_for_device(struct agx_device *dev, struct agx_bo *bo,
                          size_t offset, size_t size)
{
   if (bo == NULL || bo->uapi_handle == 0 || offset > bo->size ||
       size > bo->size - offset) {
      errno = EINVAL;
      return -1;
   }
   if (size == 0)
      return 0;

   /* Vulkan flush and Honeykrisp's upload choke points publish the exact
    * CPU-written interval. The v5 clean vector performs the ownership
    * transition once, next to the command that consumes it, instead of one
    * D3DKMT thunk per upload. */
   agx_bo_mark_cpu_dirty(bo, offset, size, true);
   return 0;
}

static int
d3dkmt_bo_sync_for_cpu(struct agx_device *dev, struct agx_bo *bo,
                       size_t offset, size_t size)
{
   struct agx_d3dkmt_winsys *ws = agx_d3dkmt_ws(dev);

   if (bo == NULL || bo->uapi_handle == 0 || offset > bo->size ||
       size > bo->size - offset) {
      errno = EINVAL;
      return -1;
   }
   if (size == 0)
      return 0;

   const struct agx_kmt_invalidatecache invalidate = {
      .hDevice = ws->device,
      .hAllocation = bo->uapi_handle,
      .Offset = offset,
      .Length = size,
   };
   agx_kmt_status st = ws->fns.InvalidateCache(&invalidate);
   if (!AGX_KMT_SUCCESS(st)) {
      agx_d3dkmt_mark_device_lost(ws, NULL);
      return agx_d3dkmt_fail(st);
   }
   return 0;
}

static void
d3dkmt_bo_mark_cpu_dirty(struct agx_device *dev, struct agx_bo *bo,
                         size_t offset, size_t size, bool precise)
{
   if (bo == NULL || size == 0)
      return;

   simple_mtx_lock(&dev->d3dkmt_dirty_lock);
   if (bo->d3dkmt_dirty.identity != 0) {
      if (!precise) {
         (void)agx_d3dkmt_cpu_map_request(
            &bo->d3dkmt_cpu_map, &bo->d3dkmt_dirty, bo->size,
            AGX_D3DKMT_CPU_MAP_RAW, false);
      } else {
         (void)agx_d3dkmt_dirty_mark(&bo->d3dkmt_dirty, bo->size, offset,
                                     size);
      }
      d3dkmt_bo_publish_work_locked(dev, bo);
   }
   simple_mtx_unlock(&dev->d3dkmt_dirty_lock);
}

static void
d3dkmt_bo_set_cpu_map_class(struct agx_device *dev, struct agx_bo *bo,
                            enum agx_bo_cpu_map_class map_class)
{
   if (bo == NULL)
      return;

   simple_mtx_lock(&dev->d3dkmt_dirty_lock);
   if (bo->d3dkmt_dirty.identity != 0) {
      const bool tracked_allowed =
         map_class == AGX_BO_CPU_MAP_TRACKED &&
         bo->d3dkmt_origin == AGX_D3DKMT_BO_PRIVATE &&
         (bo->flags & (AGX_BO_SHARED | AGX_BO_SHAREABLE)) == 0;
      const enum agx_d3dkmt_cpu_map_class requested =
         map_class == AGX_BO_CPU_MAP_TRACKED ? AGX_D3DKMT_CPU_MAP_TRACKED
                                             : AGX_D3DKMT_CPU_MAP_RAW;
      (void)agx_d3dkmt_cpu_map_request(
         &bo->d3dkmt_cpu_map, &bo->d3dkmt_dirty, bo->size, requested,
         tracked_allowed);
      d3dkmt_bo_publish_work_locked(dev, bo);
   }
   simple_mtx_unlock(&dev->d3dkmt_dirty_lock);
}

static uint64_t
d3dkmt_next_bo_identity(struct agx_device *dev)
{
   dev->d3dkmt_next_bo_identity++;
   if (dev->d3dkmt_next_bo_identity == 0)
      dev->d3dkmt_next_bo_identity++;
   return dev->d3dkmt_next_bo_identity;
}

static void
d3dkmt_bo_reset_cpu_dirty(struct agx_device *dev, struct agx_bo *bo)
{
   simple_mtx_lock(&dev->d3dkmt_dirty_lock);
   agx_d3dkmt_dirty_reset_mapped(&bo->d3dkmt_dirty,
                                 d3dkmt_next_bo_identity(dev), bo->size,
                                 bo->_map != NULL);
   bo->d3dkmt_origin = (bo->flags & AGX_BO_SHAREABLE)
                          ? AGX_D3DKMT_BO_CREATED_SHAREABLE
                          : AGX_D3DKMT_BO_PRIVATE;
   /* The BO cache preserves a Lock2 mapping across logical lifetimes. Reset
    * runs before refcnt publication, so re-establish the mapped lifetime's
    * whole-BO ownership atomically with its new identity. */
   agx_d3dkmt_cpu_map_reset(&bo->d3dkmt_cpu_map, bo->_map != NULL);
   d3dkmt_bo_publish_work_locked(dev, bo);
   simple_mtx_unlock(&dev->d3dkmt_dirty_lock);
}

static void
d3dkmt_bo_discard_cpu_dirty(struct agx_device *dev, struct agx_bo *bo)
{
   simple_mtx_lock(&dev->d3dkmt_dirty_lock);
   d3dkmt_bo_remove_work_locked(dev, bo);
   agx_d3dkmt_dirty_reset(&bo->d3dkmt_dirty,
                          d3dkmt_next_bo_identity(dev));
   agx_d3dkmt_cpu_map_reset(&bo->d3dkmt_cpu_map, false);
   bo->d3dkmt_origin = AGX_D3DKMT_BO_PRIVATE;
   simple_mtx_unlock(&dev->d3dkmt_dirty_lock);
}

/* Build v5's two independent declarations. The cached read set retains every
 * live GPU VA until retirement, including device-local BOs which were never
 * CPU mapped. The clean set detaches only pending CPU-write intervals, except
 * for raw persistent maps and true cross-process ownership, which remain
 * whole-BO conservative. */
static int
d3dkmt_reserve_dirty_snapshots(struct agx_d3dkmt_ctx *ctx, uint32_t capacity)
{
   if (capacity <= ctx->dirty_snapshot_capacity &&
       capacity <= ctx->read_candidate_capacity)
      return 0;

   if (capacity > ctx->dirty_snapshot_capacity) {
      struct agx_d3dkmt_bo_snapshot *snapshots =
         realloc(ctx->dirty_snapshots, (size_t)capacity * sizeof(*snapshots));
      if (snapshots == NULL) {
         errno = ENOMEM;
         return -1;
      }

      ctx->dirty_snapshots = snapshots;
      ctx->dirty_snapshot_capacity = capacity;
   }

   if (capacity > ctx->read_candidate_capacity) {
      struct agxw_submit_read_range *candidates =
         realloc(ctx->read_candidates,
                 (size_t)capacity * sizeof(*candidates));
      if (candidates == NULL) {
         errno = ENOMEM;
         return -1;
      }

      ctx->read_candidates = candidates;
      ctx->read_candidate_capacity = capacity;
   }
   return 0;
}

static void
d3dkmt_snapshot_dirty_bo_locked(
   struct agx_device *dev, struct agx_d3dkmt_ctx *ctx, uint32_t handle,
   struct agxw_submit_read_range *clean_ranges, uint32_t *clean_count,
   bool *clean_fallback, uint64_t *precise_bytes,
   uint64_t *conservative_bytes)
{
   struct agx_bo *bo = agx_lookup_bo(dev, handle);

   if (bo->refcnt <= 0 || bo->va == NULL || bo->size == 0) {
      (void)d3dkmt_workset_write(d3dkmt_dirty_workset(dev),
                                 dev->d3dkmt_workset_words, handle, false);
      return;
   }
   if (d3dkmt_bo_requires_conservative_clean(bo))
      (void)agx_d3dkmt_dirty_mark_whole(&bo->d3dkmt_dirty, bo->size);

   struct agx_d3dkmt_bo_snapshot *entry =
      &ctx->dirty_snapshots[ctx->dirty_snapshot_count];
   if (!agx_d3dkmt_dirty_detach(&bo->d3dkmt_dirty, &entry->dirty)) {
      d3dkmt_bo_publish_work_locked(dev, bo);
      return;
   }

   entry->handle = handle;
   entry->bo_size = bo->size;
   ctx->dirty_snapshot_count++;

   if (entry->dirty.whole_bo) {
      *conservative_bytes += bo->size;
      if (agx_d3dkmt_read_range_add(
             clean_ranges, clean_count, AGXW_MAX_READ_RANGES_PER_SUBMIT,
             bo->va->addr, bo->size) != AGX_D3DKMT_OK)
         *clean_fallback = true;
   } else {
      for (uint32_t i = 0; i < entry->dirty.range_count; i++) {
         const struct agx_d3dkmt_dirty_range *range =
            &entry->dirty.ranges[i];
         *precise_bytes += range->length;
         if (agx_d3dkmt_read_range_add(
                clean_ranges, clean_count, AGXW_MAX_READ_RANGES_PER_SUBMIT,
                bo->va->addr + range->offset,
                range->length) != AGX_D3DKMT_OK)
         *clean_fallback = true;
      }
   }

   /* Detach is the commit point for this work item. Clear it only after the
    * snapshot owns the dirty state; failed submit restoration republishes the
    * bit in d3dkmt_finish_dirty_snapshots(). */
   d3dkmt_bo_publish_work_locked(dev, bo);
}

static int
d3dkmt_build_submit_sets(struct agx_device *dev, struct agx_d3dkmt_ctx *ctx,
                         struct agxw_submit_read_range *read_ranges,
                         uint32_t *read_range_count,
                         struct agxw_submit_read_range *clean_ranges,
                         uint32_t *clean_range_count)
{
   struct agx_d3dkmt_winsys *ws = agx_d3dkmt_ws(dev);
   uint32_t generation;
   uint64_t compacted_gap_bytes = 0;
   bool read_set_compacted = false;
   uint64_t worklist_candidates = 0;
   uint64_t conservative_candidates = 0;
   bool worklist_full_scan = false;

   /* max_handle may grow between this read and taking bo_map_lock. Retry
    * rather than allocating while both BO locks are held. */
   for (;;) {
      uint32_t capacity = p_atomic_read(&dev->max_handle);
      if (d3dkmt_reserve_dirty_snapshots(ctx, capacity))
         return -1;

      simple_mtx_lock(&dev->bo_map_lock);
      if (ctx->dirty_snapshot_capacity >= dev->max_handle &&
          ctx->read_candidate_capacity >= dev->max_handle)
         break;
      simple_mtx_unlock(&dev->bo_map_lock);
   }

   generation = p_atomic_read(&dev->read_set_generation);
   if (!ws->read_set_valid || ws->read_set_generation != generation) {
      uint32_t count = 0;

      for (uint32_t handle = 0; handle < dev->max_handle; handle++) {
         struct agx_bo *bo = agx_lookup_bo(dev, handle);

         if (bo->refcnt <= 0 || bo->va == NULL || bo->size == 0)
            continue;
         if (agx_d3dkmt_read_range_add(
                ctx->read_candidates, &count,
                ctx->read_candidate_capacity,
                bo->va->addr, bo->size) != AGX_D3DKMT_OK) {
            simple_mtx_unlock(&dev->bo_map_lock);
            errno = ENOSPC;
            return -1;
         }
      }
      if (count > AGXW_MAX_READ_RANGES_PER_SUBMIT) {
         if (agx_d3dkmt_read_ranges_compact(
                ctx->read_candidates, &count,
                AGXW_MAX_READ_RANGES_PER_SUBMIT,
                &compacted_gap_bytes) != AGX_D3DKMT_OK) {
            simple_mtx_unlock(&dev->bo_map_lock);
            errno = ENOSPC;
            return -1;
         }
         read_set_compacted = true;
      }
      memcpy(ws->read_set, ctx->read_candidates,
             (size_t)count * sizeof(*ws->read_set));
      ws->read_set_count = count;
      ws->read_set_generation = generation;
      ws->read_set_valid = true;
   }
   memcpy(read_ranges, ws->read_set,
          (size_t)ws->read_set_count * sizeof(*read_ranges));
   *read_range_count = ws->read_set_count;

   uint32_t clean_count = 0;
   bool clean_fallback = false;
   uint64_t precise_bytes = 0;
   uint64_t conservative_bytes = 0;
   ctx->dirty_snapshot_count = 0;

   simple_mtx_lock(&dev->d3dkmt_dirty_lock);
   const uint32_t needed_words =
      dev->max_handle / 64u + ((dev->max_handle % 64u) != 0u);
   uint64_t *dirty_workset = d3dkmt_dirty_workset(dev);
   uint64_t *conservative_workset = d3dkmt_conservative_workset(dev);
   if (dirty_workset != NULL && conservative_workset != NULL &&
       needed_words <= dev->d3dkmt_workset_words) {
      for (uint32_t word = 0; word < needed_words; word++) {
         uint64_t conservative = conservative_workset[word];
         uint64_t pending = dirty_workset[word] | conservative;

         worklist_candidates += util_bitcount64(pending);
         conservative_candidates += util_bitcount64(conservative);
         while (pending != 0) {
            uint32_t bit = u_bit_scan64(&pending);
            uint32_t handle = word * 64u + bit;

            if (handle >= dev->max_handle)
               continue;
            d3dkmt_snapshot_dirty_bo_locked(
               dev, ctx, handle, clean_ranges, &clean_count,
               &clean_fallback, &precise_bytes, &conservative_bytes);
         }
      }
   } else {
      /* Capacity is established before BO publication, so this path is an
       * invariant fallback for correctness, never the expected hot path. */
      worklist_full_scan = true;
      if (dirty_workset != NULL)
         memset(dirty_workset, 0,
                (size_t)dev->d3dkmt_workset_words * sizeof(uint64_t));
      for (uint32_t handle = 0; handle < dev->max_handle; handle++) {
         d3dkmt_snapshot_dirty_bo_locked(
            dev, ctx, handle, clean_ranges, &clean_count, &clean_fallback,
            &precise_bytes, &conservative_bytes);
      }
   }
   simple_mtx_unlock(&dev->d3dkmt_dirty_lock);
   simple_mtx_unlock(&dev->bo_map_lock);

   if (clean_fallback) {
      memcpy(clean_ranges, read_ranges,
             (size_t)*read_range_count * sizeof(*clean_ranges));
      clean_count = *read_range_count;
   }
   *clean_range_count = clean_count;

   simple_mtx_lock(&ws->lock);
   ws->dirty_submit_count++;
   if (ctx->dirty_snapshot_count == 0)
      ws->dirty_unchanged_submits++;
   ws->dirty_precise_bytes += precise_bytes;
   ws->dirty_conservative_bytes += conservative_bytes;
   ws->dirty_worklist_candidates += worklist_candidates;
   ws->dirty_conservative_candidates += conservative_candidates;
   if (worklist_full_scan)
      ws->dirty_worklist_full_scans++;
   if (clean_fallback)
      ws->dirty_clean_vector_fallbacks++;
   if (read_set_compacted) {
      ws->read_set_compactions++;
      ws->read_set_compacted_gap_bytes += compacted_gap_bytes;
   }
   simple_mtx_unlock(&ws->lock);
   return 0;
}

static void
d3dkmt_finish_dirty_snapshots(struct agx_device *dev,
                              struct agx_d3dkmt_ctx *ctx, bool accepted)
{
   if (ctx->dirty_snapshot_count == 0)
      return;

   struct agx_d3dkmt_winsys *ws = agx_d3dkmt_ws(dev);
   uint64_t stale = 0;
   simple_mtx_lock(&dev->bo_map_lock);
   simple_mtx_lock(&dev->d3dkmt_dirty_lock);
   for (uint32_t i = 0; i < ctx->dirty_snapshot_count; i++) {
      struct agx_d3dkmt_bo_snapshot *entry = &ctx->dirty_snapshots[i];
      struct agx_bo *bo = agx_lookup_bo(dev, entry->handle);
      enum agx_d3dkmt_dirty_result result = AGX_D3DKMT_DIRTY_STALE;

      if (bo->refcnt > 0 && bo->size == entry->bo_size) {
         result = accepted
                     ? agx_d3dkmt_dirty_commit(&bo->d3dkmt_dirty,
                                               &entry->dirty)
                     : agx_d3dkmt_dirty_restore(&bo->d3dkmt_dirty, bo->size,
                                                &entry->dirty);
         if (result == AGX_D3DKMT_DIRTY_OK)
            d3dkmt_bo_publish_work_locked(dev, bo);
      }
      if (result == AGX_D3DKMT_DIRTY_STALE)
         stale++;
   }
   ctx->dirty_snapshot_count = 0;
   simple_mtx_unlock(&dev->d3dkmt_dirty_lock);
   simple_mtx_unlock(&dev->bo_map_lock);

   if (stale != 0) {
      simple_mtx_lock(&ws->lock);
      ws->dirty_stale_snapshots += stale;
      simple_mtx_unlock(&ws->lock);
   }
}

/* -------------------------------------------------------------------------
 * GET_PARAMS.
 *
 * KMTQAITYPE_UMDRIVERPRIVATE is the WDDM-native miniport->UMD capability
 * channel (it is what DxgkDdiQueryAdapterInfo(DXGKQAITYPE_UMDRIVERPRIVATE)
 * exists to serve, and what every vendor UMD uses for exactly this). It is
 * queried once at open; this op is a copy out of that cached blob.
 * ------------------------------------------------------------------------- */
static int
d3dkmt_escape(struct agx_d3dkmt_winsys *ws, uint32_t op, void *payload,
              uint32_t payload_size)
{
   /*
    * CONTRACT.md SS8: exactly two escapes exist, both pure device-property
    * reads with no WDDM equivalent -- GET_PARAMS and GET_TIME. Nothing that
    * allocates, binds, submits or synchronises goes through one, because an
    * escape for any of those would rebuild the private-side-channel split
    * this whole port exists to remove.
    */
   const uint32_t total = (uint32_t)sizeof(struct agxw_escape_header) +
                          payload_size;
   uint8_t *blob = malloc(total);
   if (blob == NULL) {
      errno = ENOMEM;
      return -1;
   }

   struct agxw_escape_header hdr = {
      .magic = AGXW_ESCAPE_MAGIC,
      .version = AGXW_ESCAPE_VERSION,
      .op = op,
      .payload_size = payload_size,
   };
   memcpy(blob, &hdr, sizeof(hdr));
   memset(blob + sizeof(hdr), 0, payload_size);

   struct agx_kmt_escape escape = {
      .hAdapter = ws->adapter,
      .pPrivateDriverData = blob,
      .PrivateDriverDataSize = total,
   };

   agx_kmt_status st = ws->fns.Escape(&escape);
   if (!AGX_KMT_SUCCESS(st)) {
      free(blob);
      return agx_d3dkmt_fail(st);
   }

   memcpy(&hdr, blob, sizeof(hdr));
   if (hdr.status != 0) {
      free(blob);
      errno = -hdr.status;
      return -1;
   }

   memcpy(payload, blob + sizeof(hdr), payload_size);
   free(blob);
   return 0;
}

/*
 * GET_PARAMS. One of the two escapes -- the AGX global parameter block has no
 * WDDM equivalent at all, and KMTQAITYPE_UMDRIVERPRIVATE is spoken for by the
 * adapter identity blob (CONTRACT.md SS2.4, SS8).
 *
 * Fetched once at open, because agx_open_device() calls this before anything
 * else and the values never change; this op is a copy out of that cache.
 */
static ssize_t
d3dkmt_get_params(struct agx_device *dev, void *buf, size_t size)
{
   struct agx_d3dkmt_winsys *ws = agx_d3dkmt_ws(dev);

   memset(buf, 0, size);

   if (ws->params == NULL || ws->params_size == 0)
      return -EINVAL;

   memcpy(buf, ws->params, MIN2(size, (size_t)ws->params_size));
   return (ssize_t)ws->params_size;
}

/* -------------------------------------------------------------------------
 * GEM_BIND_OBJECT (timestamp objects).
 *
 * AGX registers a BO with the firmware's timestamp-object table and gets back
 * a firmware-scoped handle that goes into drm_asahi_cmd_render::ts_vtx /
 * ts_frag.
 *
 * CONTRACT.md SS3.5 puts this on AGXW_ALLOC_TIMESTAMP: the KMD does the
 * context-0 bind at ALLOCATION time and returns object_handle in the output
 * private data. That is the right shape, and it is not an escape -- but it
 * does not fit the current call flow, and this is a contract gap to close
 * rather than something the UMD can paper over:
 *
 *   1. struct agxw_alloc_private has no object_handle field. It is
 *      {version, struct_size, size, flags, align_log2, reserved}, so there is
 *      nowhere for SS3.5's promised output to go.
 *   2. hk_query_pool.c:287 calls agx_bind_timestamps() on a BO that
 *      agx_bo_create() has ALREADY created, so the winsys cannot know at
 *      create time that an allocation will be a timestamp buffer. Making it
 *      knowable means a new agx_bo_flags bit plumbed through
 *      hk_create_query_pool -- small, but a Honeykrisp change, not a winsys
 *      one.
 *
 * Until both are settled this op is an identity: the "object handle" IS the
 * allocation handle, which is a value the miniport already has and can map to
 * a firmware slot lazily when it parses ts_vtx/ts_frag out of the command
 * stream. That is still not an escape, and it keeps timestamp queries on the
 * same allocator as everything else.
 * ------------------------------------------------------------------------- */
static int
d3dkmt_bo_bind_object(struct agx_device *dev,
                      struct drm_asahi_gem_bind_object *bind)
{
   (void)dev;

   if (bind->op != DRM_ASAHI_BIND_OBJECT_OP_BIND) {
      errno = EINVAL;
      return -1;
   }

   if (bind->handle == 0) {
      errno = EINVAL;
      return -1;
   }

   bind->object_handle = bind->handle;
   return 0;
}

static int
d3dkmt_bo_unbind_object(struct agx_device *dev, uint32_t object_handle)
{
   (void)dev;
   (void)object_handle;
   return 0;
}

/* -------------------------------------------------------------------------
 * SUBMIT.
 * ------------------------------------------------------------------------- */
static struct agx_bo *
staging_for(struct agx_device *dev, struct agx_d3dkmt_ctx *ctx, uint32_t size)
{
   struct agx_d3dkmt_winsys *ws = agx_d3dkmt_ws(dev);
   unsigned slot = ctx->staging_slot;
   struct agx_bo *bo = ctx->staging[slot];

   if (bo != NULL && bo->size < size) {
      /* Wait for the slot to retire before recycling its memory. */
      if (agx_d3dkmt_wait_fence(ws, ctx->fence, ctx->fence_cpu,
                                ctx->staging_retire[slot],
                                AGX_D3DKMT_INFINITE))
         return NULL;
      agx_bo_unreference(dev, bo);
      bo = ctx->staging[slot] = NULL;
   }

   if (bo == NULL) {
      size_t alloc = ALIGN_POT(MAX2((size_t)size, (size_t)AIL_PAGESIZE),
                               AIL_PAGESIZE);
      /* The CPU writes this ring slot once, then the KMD consumes it. Keep it
       * write-combined like Asahi's ordinary upload storage. The miniport's
       * submit-scoped WC publication orders every producer CPU before AGX is
       * kicked, so this page has no write-back cache lines to clean. */
      bo = agx_bo_create(dev, alloc, 0, 0, "D3DKMT cmdbuf");
      if (bo == NULL)
         return NULL;
      ctx->staging[slot] = bo;
      ctx->staging_retire[slot] = 0;
   } else if (ctx->staging_retire[slot] != 0) {
      if (agx_d3dkmt_wait_fence(ws, ctx->fence, ctx->fence_cpu,
                                ctx->staging_retire[slot],
                                AGX_D3DKMT_INFINITE))
         return NULL;
   }

   return bo;
}

/*
 * DRM_IOCTL_ASAHI_SUBMIT carries in-syncs, the command stream and out-syncs in
 * ONE ioctl. Under the contract that becomes:
 *
 *   1. D3DKMTWaitForSynchronizationObjectFromGpu on the context, for the
 *      in-syncs. NOT the submit private data's in_sync_count, which
 *      CONTRACT.md SS7.3 requires to be 0 in contract version 1 -- AppleAgxGpu
 *      rejects a non-zero one (submit.c:629-630) and the WDDM front end
 *      inherits that gap. The contract notes this is the idiomatic shape
 *      regardless, and stays the recommendation after the gap closes.
 *   2. D3DKMTSubmitCommand, with Commands pointing at an AGXW_DMA_HEADER
 *      followed by the verbatim AGX command stream. No allocation list, no
 *      patch locations: final GPU VAs are already in the stream, exactly as on
 *      Linux (CONTRACT.md SS7.2 item 3).
 *   3. D3DKMTSignalSynchronizationObjectFromGpu, on the same context, for
 *      every out-sync and the staging-retire fence. This queues an ordered
 *      dxgkrnl software signal packet behind the submitted DMA. D3DKMT sync
 *      handles are dxgkrnl handles and MUST NOT appear in KMD private data.
 *
 * A D3DKMT context is a strictly serial queue, so (1)-(3) are equivalent
 * to the atomic ioctl PROVIDED nothing fails in between -- a failure after the
 * wait would leave a dangling dependency on the context. Everything under
 * UMD control (context lookup, staging allocation, sync resolution, blob
 * sizing) is therefore done BEFORE the first D3DKMT call. A thunk failure
 * after sync values are reserved poisons the context fail-closed.
 */
static int
d3dkmt_submit(struct agx_device *dev, struct drm_asahi_submit *submit,
              struct agx_submit_virt *virt)
{
   struct agx_d3dkmt_winsys *ws = agx_d3dkmt_ws(dev);
   const struct drm_asahi_sync *syncs =
      (const struct drm_asahi_sync *)(uintptr_t)submit->syncs;
   agx_kmt_handle *waits = NULL;
   uint64_t *wait_values = NULL;
   agx_kmt_handle *signals = NULL;
   uint64_t *signal_values = NULL;
   struct agxw_submit_private_v5 *priv = NULL;
   uint32_t priv_size = 0;
   int ret = -1;
   bool command_submitted = false;
   bool clean_lock_held = false;
#if AGX_GPU_DIAG >= 2
   uint32_t diag_slot = 0;
   uint64_t diag_sequence = 0;
#endif

   (void)virt; /* no vdrm concept here, same as the Linux drm backend */

   const uint32_t n_in = submit->in_sync_count;
   const uint32_t n_out = submit->out_sync_count;

   /* +1 for the winsys's own staging-retire fence. */
   if ((uint64_t)n_in + n_out + 1 > AGXW_MAX_SYNCS_PER_SUBMIT ||
       submit->cmdbuf_size > AGXW_MAX_CMDBUF_BYTES) {
      errno = EINVAL;
      return -1;
   }

   if (submit->flags & ~AGX_SUBMIT_WRITE_SET_COMPLETE) {
      errno = EINVAL;
      return -1;
   }

   simple_mtx_lock(&ws->lock);
   struct agx_d3dkmt_ctx *ctx = ctx_get(ws, submit->queue_id);
   if (ctx == NULL) {
      simple_mtx_unlock(&ws->lock);
      errno = EINVAL;
      return -1;
   }
   if (ws->device_lost || ctx->submit_failed || ctx->submit_refs != 0) {
      simple_mtx_unlock(&ws->lock);
      errno = (ws->device_lost || ctx->submit_failed) ? EIO : EBUSY;
      return -1;
   }
   ctx->submit_refs = 1;
   ws->active_submits++;
   const unsigned staging_slot = ctx->staging_slot;
   simple_mtx_unlock(&ws->lock);

   signals = malloc((size_t)(n_out + 1) * sizeof(*signals));
   signal_values = malloc((size_t)(n_out + 1) * sizeof(*signal_values));
   if (n_in > 0) {
      waits = malloc((size_t)n_in * sizeof(*waits));
      wait_values = malloc((size_t)n_in * sizeof(*wait_values));
   }
   if (signals == NULL || signal_values == NULL ||
       (n_in > 0 && (waits == NULL || wait_values == NULL))) {
      errno = ENOMEM;
      goto out;
   }

   /* The DMA buffer: header + verbatim AGX command stream, in a BO this
    * winsys owns and has already mapped to GPU VA. */
   struct agx_bo *staging =
      staging_for(dev, ctx,
                  (uint32_t)sizeof(struct agxw_dma_header) +
                     submit->cmdbuf_size);
   if (staging == NULL) {
      errno = ENOMEM;
      goto out;
   }

   uint8_t *map = agx_bo_map_tracked(staging);
   if (map == NULL) {
      errno = ENOMEM;
      goto out;
   }

   struct agxw_dma_header dma = {
      .magic = AGXW_DMA_MAGIC,
      .header_size = sizeof(dma),
      .cmdbuf_size = submit->cmdbuf_size,
   };
   memcpy(map, &dma, sizeof(dma));
   memcpy(map + sizeof(dma), (const void *)(uintptr_t)submit->cmdbuf,
          submit->cmdbuf_size);

   if (agx_bo_sync_for_device(
          staging, 0,
          (size_t)sizeof(struct agxw_dma_header) + submit->cmdbuf_size))
      goto out;

   /* Resolve handles and reserve fence values before serializing CPU-dirty
    * ownership. Only the build-to-SubmitCommand accept window needs the
    * device-wide gate. */
   uint32_t n_sig = 0;
   uint64_t retire;

   simple_mtx_lock(&ws->lock);
   for (uint32_t i = 0; i < n_in; ++i) {
      struct agx_d3dkmt_sync *s =
         agx_d3dkmt_sync_table_get(&ws->syncs, syncs[i].handle);
      if (s == NULL) {
         simple_mtx_unlock(&ws->lock);
         errno = EINVAL;
         goto out;
      }
      waits[i] = s->kmt;
      wait_values[i] = syncs[i].sync_type == DRM_ASAHI_SYNC_TIMELINE_SYNCOBJ
                          ? syncs[i].timeline_value
                          : agx_d3dkmt_binary_wait_value(s);
   }

   for (uint32_t i = 0; i < n_out; ++i) {
      const struct drm_asahi_sync *src = &syncs[n_in + i];
      struct agx_d3dkmt_sync *e =
         agx_d3dkmt_sync_table_get(&ws->syncs, src->handle);
      if (e == NULL) {
         simple_mtx_unlock(&ws->lock);
         errno = EINVAL;
         goto out;
      }

      uint64_t v = src->sync_type == DRM_ASAHI_SYNC_TIMELINE_SYNCOBJ
                      ? src->timeline_value
                      : agx_d3dkmt_binary_signal_value(e, NULL);

      signals[n_sig] = e->kmt;
      signal_values[n_sig] = v;
      if (v > e->last_request)
         e->last_request = v;
      n_sig++;
   }

   retire = ++ctx->fence_next;
   signals[n_sig] = ctx->fence;
   signal_values[n_sig] = retire;
   n_sig++;
#if AGX_GPU_DIAG >= 2
   diag_sequence = ++ws->diag_submit_sequence;
   diag_slot = (uint32_t)((diag_sequence - 1) % AGX_D3DKMT_DIAG_SUBMIT_RING);
   struct agx_d3dkmt_diag_submit *diag = &ws->diag_submits[diag_slot];
   memset(diag, 0, sizeof(*diag));
   diag->sequence = diag_sequence;
   diag->tick_ms = GetTickCount64();
   diag->queue_id = submit->queue_id;
   diag->cmdbuf_size = submit->cmdbuf_size;
   diag->in_count = n_in;
   diag->out_count = n_out;
   diag->context = ctx->hContext;
   for (uint32_t i = 0; i < MIN2(n_in, AGX_D3DKMT_DIAG_SYNCS); ++i) {
      diag->wait_handles[i] = syncs[i].handle;
      diag->wait_values[i] = wait_values[i];
   }
   for (uint32_t i = 0; i < MIN2(n_out, AGX_D3DKMT_DIAG_SYNCS); ++i) {
      diag->signal_handles[i] = syncs[n_in + i].handle;
      diag->signal_values[i] = signal_values[i];
   }
#endif
   simple_mtx_unlock(&ws->lock);

   simple_mtx_lock(&dev->d3dkmt_submit_clean_lock);
   clean_lock_held = true;
   memset(ctx->submit_blob, 0, sizeof(ctx->submit_blob->header));
   priv = &ctx->submit_blob->header;
   struct agxw_submit_read_range *read_ranges =
      ctx->submit_blob->ranges;
   struct agxw_submit_read_range *clean_ranges =
      &ctx->submit_blob->ranges[AGXW_MAX_READ_RANGES_PER_SUBMIT];
   if (d3dkmt_build_submit_sets(
          dev, ctx, read_ranges, &priv->read_range_count, clean_ranges,
          &priv->clean_range_count) != 0)
      goto out;

   /* The ABI vectors are tightly packed even though the scratch clean vector
    * starts at a fixed offset to make independent overflow handling simple. */
   memmove(read_ranges + priv->read_range_count, clean_ranges,
           (size_t)priv->clean_range_count * sizeof(*clean_ranges));
   priv->base.version = AGXW_SUBMIT_PRIVATE_VERSION_V5;
   priv->base.struct_size = sizeof(*priv);
   priv->base.in_sync_count = 0;
   priv->base.out_sync_count = 0;
   priv->base.flags = AGXW_SUBMIT_READ_SET_COMPLETE |
      AGXW_SUBMIT_CLEAN_SET_COMPLETE |
      ((submit->flags & AGX_SUBMIT_WRITE_SET_COMPLETE)
          ? AGXW_SUBMIT_WRITE_SET_COMPLETE : 0);
   priv_size = (uint32_t)sizeof(*priv) +
      (priv->read_range_count + priv->clean_range_count) *
         sizeof(*read_ranges);
   if (agxw_submit_private_validate(&priv->base, priv_size) != 0) {
      errno = EINVAL;
      goto out;
   }

   if (n_in > 0) {
      struct agx_kmt_waitsyncfromgpu wait = {
         .hContext = ctx->hContext,
         .ObjectCount = n_in,
         .ObjectHandleArray = waits,
         .MonitoredFenceValueArray = wait_values,
      };
      agx_kmt_status st = ws->fns.WaitForSynchronizationObjectFromGpu(&wait);
      if (!AGX_KMT_SUCCESS(st)) {
         ret = agx_d3dkmt_fail(st);
         agx_d3dkmt_mark_device_lost(ws, ctx);
         goto out;
      }
#if AGX_GPU_DIAG >= 2
      simple_mtx_lock(&ws->lock);
      if (ws->diag_submits[diag_slot].sequence == diag_sequence)
         ws->diag_submits[diag_slot].callback_mask |= 1u;
      simple_mtx_unlock(&ws->lock);
#endif
   }

   struct agx_kmt_submitcommand cmd = {
      .Commands = staging->va->addr,
      .CommandLength =
         (uint32_t)sizeof(struct agxw_dma_header) + submit->cmdbuf_size,
      .pPrivateDriverData = priv,
      .PrivateDriverDataSize = priv_size,
      .BroadcastContextCount = 1,
      .BroadcastContext = {ctx->hContext},
   };

   agx_kmt_status st = ws->fns.SubmitCommand(&cmd);
   if (!AGX_KMT_SUCCESS(st)) {
      ret = agx_d3dkmt_fail(st);
      agx_d3dkmt_mark_device_lost(ws, ctx);
      goto out;
   }
   command_submitted = true;
   d3dkmt_finish_dirty_snapshots(dev, ctx, true);
   simple_mtx_unlock(&dev->d3dkmt_submit_clean_lock);
   clean_lock_held = false;
#if AGX_GPU_DIAG >= 2
   simple_mtx_lock(&ws->lock);
   if (ws->diag_submits[diag_slot].sequence == diag_sequence)
      ws->diag_submits[diag_slot].callback_mask |= 2u;
   simple_mtx_unlock(&ws->lock);
#endif

   struct agx_kmt_signalsyncfromgpu signal = {
      .hContext = ctx->hContext,
      .ObjectCount = n_sig,
      .ObjectHandleArray = signals,
      .MonitoredFenceValueArray = signal_values,
   };
   st = ws->fns.SignalSynchronizationObjectFromGpu(&signal);
   if (!AGX_KMT_SUCCESS(st)) {
      /* The DMA is already in flight but its completion signals were not
       * queued. Poison the context so staging can never be reused unsafely. */
      ret = agx_d3dkmt_fail(st);
      agx_d3dkmt_mark_completion_unknown(ws, ctx);
      goto out;
   }
#if AGX_GPU_DIAG >= 2
   simple_mtx_lock(&ws->lock);
   if (ws->diag_submits[diag_slot].sequence == diag_sequence)
      ws->diag_submits[diag_slot].callback_mask |= 4u;
   simple_mtx_unlock(&ws->lock);
#endif

   simple_mtx_lock(&ws->lock);
   ctx->staging_retire[staging_slot] = retire;
   ctx->staging_slot = (staging_slot + 1) % AGX_D3DKMT_STAGING_RING;
   simple_mtx_unlock(&ws->lock);
   ret = 0;

out:
   if (clean_lock_held) {
      d3dkmt_finish_dirty_snapshots(dev, ctx, command_submitted);
      simple_mtx_unlock(&dev->d3dkmt_submit_clean_lock);
   }
   simple_mtx_lock(&ws->lock);
   assert(ctx->submit_refs == 1 && ws->active_submits > 0);
   ctx->submit_refs = 0;
   ws->active_submits--;
   simple_mtx_unlock(&ws->lock);
   free(waits);
   free(wait_values);
   free(signals);
   free(signal_values);
   return ret;
}

/* -------------------------------------------------------------------------
 * simple_ioctl: VM_CREATE, QUEUE_CREATE, QUEUE_DESTROY, GET_TIME.
 * ------------------------------------------------------------------------- */
static int
d3dkmt_reserve(struct agx_d3dkmt_winsys *ws, uint64_t base, uint64_t size)
{
   struct agx_kmt_reservegpuva reserve = {
      .hPagingQueue = ws->paging_queue,
      .BaseAddress = base,
      .MinimumAddress = base,
      .MaximumAddress = base + size,
      .Size = size,
      .ReservationType = AGX_KMT_GPUVA_RESERVE_NO_ACCESS,
   };

   agx_kmt_status st = ws->fns.ReserveGpuVirtualAddress(&reserve);
   if (!AGX_KMT_SUCCESS(st)) {
      fprintf(stderr, "D3DKMTReserveGpuVirtualAddress(0x%llx, 0x%llx) failed\n",
              (unsigned long long)base, (unsigned long long)size);
      return agx_d3dkmt_fail(st);
   }

   /*
    * CONTRACT.md SS4.3 item 2 decides that a non-zero BaseAddress is
    * MANDATORY, not advisory -- but flags it as the one place it wants a
    * hardware datum, because whether VidMm passes BaseAddress through rather
    * than overriding it is unproven. This check is the seam: if VidMm
    * relocates, exactly one call site fails loudly here instead of every GPU
    * address in the driver being silently wrong. Honeykrisp's VA layout is
    * not relocatable -- shader_base, the zero page at 2^32 and the printf
    * buffer at 2^36 are baked into precompiled shaders
    * (agx_device.c:580-600, LIBAGX_PRINTF_BUFFER_ADDRESS) -- so a driver that
    * cannot pin its own addresses cannot run at all, and saying so on the
    * first reservation is the cheapest possible discovery.
    */
   if (reserve.VirtualAddress != base) {
      fprintf(stderr,
              "D3DKMTReserveGpuVirtualAddress ignored the base hint "
              "(wanted 0x%llx, got 0x%llx); Honeykrisp's VA layout is fixed\n",
              (unsigned long long)base,
              (unsigned long long)reserve.VirtualAddress);
      errno = ENOTSUP;
      return -1;
   }

   return agx_d3dkmt_wait_paging(ws, reserve.PagingFenceValue);
}

static bool
d3dkmt_luid_equal(struct agx_kmt_luid a, struct agx_kmt_luid b)
{
   return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

static bool
d3dkmt_ranges_equal(const struct agx_d3dkmt_va_lease *lease,
                    const struct agx_d3dkmt_va_range *ranges, uint32_t count)
{
   if (lease->range_count != count)
      return false;

   for (uint32_t i = 0; i < count; ++i) {
      if (lease->ranges[i].base != ranges[i].base ||
          lease->ranges[i].size != ranges[i].size)
         return false;
   }

   return true;
}

static int
d3dkmt_free_va_range(struct agx_d3dkmt_winsys *ws,
                     const struct agx_d3dkmt_va_range *range)
{
   struct agx_kmt_freegpuva free_va = {
      .hAdapter = ws->adapter,
      .BaseAddress = range->base,
      .Size = range->size,
   };
   agx_kmt_status st = ws->fns.FreeGpuVirtualAddress(&free_va);

   if (!AGX_KMT_SUCCESS(st)) {
      fprintf(stderr,
              "D3DKMTFreeGpuVirtualAddress(0x%llx, 0x%llx) failed "
              "status=0x%08x\n",
              (unsigned long long)range->base,
              (unsigned long long)range->size, (unsigned)st);
      return agx_d3dkmt_fail(st);
   }

   return 0;
}

static int
d3dkmt_acquire_va_lease(struct agx_d3dkmt_winsys *ws,
                        const struct agx_d3dkmt_va_range *ranges,
                        uint32_t count)
{
   struct agx_d3dkmt_va_lease **link;
   struct agx_d3dkmt_va_lease *lease;
   uint32_t acquired_mask = 0;
   int saved_errno = 0;

   if (count == 0 || count > ARRAY_SIZE(ws->va)) {
      errno = EINVAL;
      return -1;
   }

   AcquireSRWLockExclusive(&agx_d3dkmt_va_lease_lock);
   for (link = &agx_d3dkmt_va_leases; *link != NULL;
        link = &(*link)->next) {
      if (d3dkmt_luid_equal((*link)->luid, ws->luid))
         break;
   }

   lease = *link;
   if (lease != NULL) {
      if (!d3dkmt_ranges_equal(lease, ranges, count) ||
          lease->references != 0) {
         if (!d3dkmt_ranges_equal(lease, ranges, count)) {
            ReleaseSRWLockExclusive(&agx_d3dkmt_va_lease_lock);
            errno = EINVAL;
            return -1;
         }

         lease->references++;
         memcpy(ws->va, ranges, count * sizeof(ranges[0]));
         ws->va_count = count;
         ReleaseSRWLockExclusive(&agx_d3dkmt_va_lease_lock);
         return 0;
      }
   } else {
      lease = calloc(1, sizeof(*lease));
      if (lease == NULL) {
         ReleaseSRWLockExclusive(&agx_d3dkmt_va_lease_lock);
         errno = ENOMEM;
         return -1;
      }
      lease->luid = ws->luid;
      lease->range_count = count;
      memcpy(lease->ranges, ranges, count * sizeof(ranges[0]));
      lease->next = agx_d3dkmt_va_leases;
      agx_d3dkmt_va_leases = lease;
   }

   /* references==0 means a prior final release either completed or retained
    * only the ranges whose free failed. Rebuild the missing subset exactly. */
   for (uint32_t i = 0; i < count; ++i) {
      const uint32_t bit = 1u << i;
      if ((lease->reserved_mask & bit) != 0)
         continue;
      if (d3dkmt_reserve(ws, ranges[i].base, ranges[i].size)) {
         saved_errno = errno;
         goto rollback;
      }
      lease->reserved_mask |= bit;
      acquired_mask |= bit;
   }

   lease->references = 1;
   memcpy(ws->va, ranges, count * sizeof(ranges[0]));
   ws->va_count = count;
   ReleaseSRWLockExclusive(&agx_d3dkmt_va_lease_lock);
   return 0;

rollback:
   for (uint32_t i = 0; i < count; ++i) {
      const uint32_t bit = 1u << i;
      if ((acquired_mask & bit) != 0 &&
          d3dkmt_free_va_range(ws, &ranges[i]) == 0)
         lease->reserved_mask &= ~bit;
   }
   if (lease->reserved_mask == 0) {
      for (link = &agx_d3dkmt_va_leases; *link != NULL;
           link = &(*link)->next) {
         if (*link == lease) {
            *link = lease->next;
            break;
         }
      }
      free(lease);
   }
   ReleaseSRWLockExclusive(&agx_d3dkmt_va_lease_lock);
   errno = saved_errno;
   return -1;
}

static void
d3dkmt_release_va_lease(struct agx_d3dkmt_winsys *ws)
{
   struct agx_d3dkmt_va_lease **link;
   struct agx_d3dkmt_va_lease *lease;

   if (ws->va_count == 0)
      return;

   AcquireSRWLockExclusive(&agx_d3dkmt_va_lease_lock);
   for (link = &agx_d3dkmt_va_leases; *link != NULL;
        link = &(*link)->next) {
      if (d3dkmt_luid_equal((*link)->luid, ws->luid))
         break;
   }
   lease = *link;
   if (lease == NULL || !d3dkmt_ranges_equal(lease, ws->va, ws->va_count) ||
       lease->references == 0) {
      fprintf(stderr, "D3DKMT GPUVA lease invariant failure\n");
      ReleaseSRWLockExclusive(&agx_d3dkmt_va_lease_lock);
      ws->va_count = 0;
      return;
   }

   lease->references--;
   if (lease->references != 0) {
      ReleaseSRWLockExclusive(&agx_d3dkmt_va_lease_lock);
      ws->va_count = 0;
      return;
   }

   for (uint32_t i = 0; i < lease->range_count; ++i) {
      const uint32_t bit = 1u << i;
      if ((lease->reserved_mask & bit) != 0 &&
          d3dkmt_free_va_range(ws, &lease->ranges[i]) == 0)
         lease->reserved_mask &= ~bit;
   }
   if (lease->reserved_mask == 0) {
      *link = lease->next;
      free(lease);
   }
   ReleaseSRWLockExclusive(&agx_d3dkmt_va_lease_lock);
   ws->va_count = 0;
}

static int
d3dkmt_vm_create(struct agx_device *dev, struct drm_asahi_vm_create *vm)
{
   struct agx_d3dkmt_winsys *ws = agx_d3dkmt_ws(dev);

   /*
    * There is no VM object to create: CONTRACT.md SS5.1 makes the AGX VM the
    * D3DKMT device, which agx_d3dkmt_open_device() already made. What DOES
    * have to happen here, and can only happen here, is reserving the user VA
    * window: D3DKMTMapGpuVirtualAddress can only map inside a reservation,
    * and vm_create is the moment Honeykrisp has decided where its heaps go.
    *
    * vm->kernel_start/kernel_end are INPUTS on DRM -- userspace picks where
    * the kernel's slice sits. Under WDDM the miniport owns its own kernel VA
    * (it lives in TTBR1 and is not part of the WDDM VA space at all,
    * CONTRACT.md SS4.2), so they are advisory here and only the window below
    * kernel_start is claimed.
    */
   uint64_t base = dev->params.vm_start;
   uint64_t end = vm->kernel_start;

   if (end <= base) {
      errno = EINVAL;
      return -1;
   }

   /*
    * CONTRACT.md SS4.2: a single 16 KiB kernel/firmware page sits in the
    * MIDDLE of the user window. Reserving across it would ask VidMm for a
    * range that includes an address it has declared internal, so the window
    * is reserved as the one or two sub-ranges that avoid it.
    */
   struct agx_d3dkmt_va_range ranges[2];
   uint32_t n = agx_d3dkmt_va_split_carveout(base, end - base, ranges);
   if (n == 0) {
      errno = EINVAL;
      return -1;
   }

   if (d3dkmt_acquire_va_lease(ws, ranges, n))
      return -1;

   /*
    * CONTRACT.md SS3.3: AGX_BO_EXEC is a MAPPING protection, not an
    * allocation flag, and a bind op carries no execute bit -- so the winsys
    * derives it from the address. agx_open_device() has already chosen
    * shader_base and the 4 GiB USC heap above it (agx_device.c:596-612)
    * before it calls VM_CREATE, so this is the first and only place the
    * window is knowable.
    */
   ws->policy.usc_base = dev->shader_base;
   ws->policy.usc_size = AGX_D3DKMT_USC_SIZE;

   /* UpdateGpuVirtualAddress is defined against a real rendering context and
    * its monitored fence. Establish that interlock after the VA reservations
    * and USC policy exist, but before agx_open_device() performs its first
    * zero-page bind. */
   if (d3dkmt_va_update_init(ws) != 0) {
      for (uint32_t i = 0; i < n; ++i) {
         struct agx_kmt_freegpuva f = {
            .hAdapter = ws->adapter,
            .BaseAddress = ranges[i].base,
            .Size = ranges[i].size,
         };
         ws->fns.FreeGpuVirtualAddress(&f);
      }
      ws->va_count = 0;
      return -1;
   }

   /* One VM per device; the id is never interpreted by the kernel here. */
   vm->vm_id = 1;
   return 0;
}

static int
d3dkmt_queue_create(struct agx_device *dev, struct drm_asahi_queue_create *q)
{
   struct agx_d3dkmt_winsys *ws = agx_d3dkmt_ws(dev);
   const char *diag_stage = "entry";
   agx_kmt_handle diag_fence = 0;
   agx_kmt_handle diag_context = 0;
   uint32_t diag_queue = 0;
   int ret = -1;

   simple_mtx_lock(&ws->lock);

   uint32_t slot = 0;
   while (slot < ws->ctx_capacity && ws->ctxs[slot].hContext != 0)
      slot++;

   if (slot == ws->ctx_capacity) {
      /* A realloc would invalidate a ctx pointer held by submit(). */
      if (ws->active_submits != 0) {
         diag_stage = "grow-active-submits";
         errno = EBUSY;
         goto out;
      }
      uint32_t cap = ws->ctx_capacity ? ws->ctx_capacity * 2 : 4;
      struct agx_d3dkmt_ctx *ctxs =
         realloc(ws->ctxs, (size_t)cap * sizeof(*ctxs));
      if (ctxs == NULL) {
         diag_stage = "grow-realloc";
         errno = ENOMEM;
         goto out;
      }
      memset(ctxs + ws->ctx_capacity, 0,
             (size_t)(cap - ws->ctx_capacity) * sizeof(*ctxs));
      ws->ctxs = ctxs;
      ws->ctx_capacity = cap;
   }

   struct agx_d3dkmt_ctx *ctx = &ws->ctxs[slot];
   memset(ctx, 0, sizeof(*ctx));
   ctx->submit_blob = calloc(1, sizeof(*ctx->submit_blob));
   if (ctx->submit_blob == NULL) {
      diag_stage = "submit-blob-alloc";
      errno = ENOMEM;
      goto out;
   }

   diag_stage = "sync-create";
   /* Per-context completion fence, used only for staging-ring recycling. */
   struct agx_kmt_createsyncobj2 fence = {
      .hDevice = ws->device,
      .Info = {.Type = AGX_KMT_SYNC_MONITORED_FENCE},
   };
   agx_kmt_status st = ws->fns.CreateSynchronizationObject2(&fence);
   diag_fence = fence.hSyncObject;
   if (!AGX_KMT_SUCCESS(st)) {
      free(ctx->submit_blob);
      ctx->submit_blob = NULL;
      ret = agx_d3dkmt_fail(st);
      goto out;
   }

   struct agxw_context_private priv = {
      .version = AGXW_CONTEXT_PRIVATE_VERSION,
      .struct_size = sizeof(priv),
      .priority = q->priority,
      .usc_exec_base = q->usc_exec_base,
   };

   /* Pre-validate with the miniport's own validator rather than a private
    * copy of the rules, so a rejection here means the same thing it would
    * mean in the kernel (CONTRACT.md SS5.2). */
   diag_stage = "context-private-validate";
   if (agxw_context_private_validate(&priv, sizeof(priv), ws->policy.usc_base,
                                     ws->policy.usc_size) != 0) {
      struct agx_kmt_destroysyncobj d = {.hSyncObject = fence.hSyncObject};
      ws->fns.DestroySynchronizationObject(&d);
      free(ctx->submit_blob);
      ctx->submit_blob = NULL;
      errno = EINVAL;
      goto out;
   }

   struct agx_kmt_createcontextvirtual create = {
      .hDevice = ws->device,
      .NodeOrdinal = ws->policy.node_ordinal,
      .EngineAffinity = 1,
      .pPrivateDriverData = &priv,
      .PrivateDriverDataSize = sizeof(priv),
   };

   diag_stage = "context-create";
   st = ws->fns.CreateContextVirtual(&create);
   diag_context = create.hContext;
   if (!AGX_KMT_SUCCESS(st)) {
      struct agx_kmt_destroysyncobj d = {.hSyncObject = fence.hSyncObject};
      ws->fns.DestroySynchronizationObject(&d);
      free(ctx->submit_blob);
      ctx->submit_blob = NULL;
      ret = agx_d3dkmt_fail(st);
      goto out;
   }

   ctx->hContext = create.hContext;
   ctx->fence = fence.hSyncObject;
   ctx->fence_cpu =
      (const volatile uint64_t *)fence.Info.MonitoredFence
         .FenceValueCPUVirtualAddress;

   /* queue_id is a dense slot index, not the D3DKMT context handle: it is
    * stored in drm_asahi_submit::queue_id and looked up on every submit. */
   q->queue_id = slot + 1;
   diag_queue = q->queue_id;
   diag_stage = "complete";
   ret = 0;

out:
   simple_mtx_unlock(&ws->lock);
   if (agxw_bridge_is_sentinel(ws->device))
      agx_bridge_valog(
         "QUEUE winsys-create provider_slot=%u stage=%s ctx_slot=%u "
         "queue=%u priority=%u usc=0x%llx sync=0x%08x token=0x%08x "
         "ret=%d errno=%d",
         agxw_bridge_slot_of_sentinel(ws->device), diag_stage, slot,
         diag_queue, q->priority, (unsigned long long)q->usc_exec_base,
         (unsigned)diag_fence, (unsigned)diag_context, ret,
         ret == 0 ? 0 : errno);
   return ret;
}

static int
d3dkmt_queue_destroy(struct agx_device *dev, struct drm_asahi_queue_destroy *q)
{
   struct agx_d3dkmt_winsys *ws = agx_d3dkmt_ws(dev);

   simple_mtx_lock(&ws->lock);
   struct agx_d3dkmt_ctx *ctx = ctx_get(ws, q->queue_id);
   if (ctx == NULL) {
      simple_mtx_unlock(&ws->lock);
      if (agxw_bridge_is_sentinel(ws->device))
         agx_bridge_valog(
            "QUEUE winsys-destroy provider_slot=%u stage=lookup queue=%u "
            "ret=-1 errno=%d",
            agxw_bridge_slot_of_sentinel(ws->device), q->queue_id, EINVAL);
      errno = EINVAL;
      return -1;
   }
   if (ctx->submit_refs != 0 || ctx->completion_unknown) {
      const uint32_t diag_refs = ctx->submit_refs;
      const bool diag_unknown = ctx->completion_unknown;
      simple_mtx_unlock(&ws->lock);
      if (agxw_bridge_is_sentinel(ws->device))
         agx_bridge_valog(
            "QUEUE winsys-destroy provider_slot=%u stage=busy queue=%u "
            "refs=%u unknown=%u ret=-1 errno=%d",
            agxw_bridge_slot_of_sentinel(ws->device), q->queue_id,
            diag_refs, diag_unknown ? 1u : 0u, EBUSY);
      errno = EBUSY;
      return -1;
   }

   struct agx_d3dkmt_ctx local = *ctx;
   memset(ctx, 0, sizeof(*ctx));
   simple_mtx_unlock(&ws->lock);

   struct agx_kmt_destroycontext destroy = {.hContext = local.hContext};
   agx_kmt_status destroy_status = ws->fns.DestroyContext(&destroy);
   if (enum_log_enabled())
      fprintf(stderr,
              "NTAGX_ENUM destroy_context queue=%u context=0x%08x status=0x%08x\n",
              q->queue_id, (unsigned)local.hContext,
              (unsigned)destroy_status);

   for (unsigned i = 0; i < AGX_D3DKMT_STAGING_RING; ++i) {
      if (local.staging[i] != NULL)
         agx_bo_unreference(dev, local.staging[i]);
   }

   struct agx_kmt_destroysyncobj d = {.hSyncObject = local.fence};
   agx_kmt_status sync_status = ws->fns.DestroySynchronizationObject(&d);
   free(local.submit_blob);
   free(local.dirty_snapshots);
   free(local.read_candidates);
   if (agxw_bridge_is_sentinel(ws->device))
      agx_bridge_valog(
         "QUEUE winsys-destroy provider_slot=%u stage=complete queue=%u "
         "token=0x%08x sync=0x%08x context_status=0x%08x "
         "sync_status=0x%08x ret=0 errno=0",
         agxw_bridge_slot_of_sentinel(ws->device), q->queue_id,
         (unsigned)local.hContext, (unsigned)local.fence,
         (unsigned)destroy_status, (unsigned)sync_status);
   return 0;
}

/*
 * GET_TIME. The second and last escape.
 *
 * D3DKMTQueryClockCalibration exists and is the obvious WDDM-native candidate,
 * but it reports an engine tick whose relationship to AGX's
 * command_timestamp_frequency_hz is unspecified, and Honeykrisp converts this
 * value with exactly that frequency (agx_gpu_timestamp_to_ns, agx_device.h).
 * The contract routes it through the escape so the KMD answers from the same
 * timebase source that serves the ioctl today, which is the property that
 * actually matters (CONTRACT.md SS8).
 */
static int
d3dkmt_get_time(struct agx_device *dev, struct drm_asahi_get_time *t)
{
   struct agx_d3dkmt_winsys *ws = agx_d3dkmt_ws(dev);

   return d3dkmt_escape(ws, AGXW_ESC_GET_TIME, t, AGXW_GET_TIME_SIZE);
}

static int
d3dkmt_simple_ioctl(struct agx_device *dev, unsigned cmd, void *req)
{
   switch (cmd) {
   case DRM_IOCTL_ASAHI_VM_CREATE:
      return d3dkmt_vm_create(dev, req);
   case DRM_IOCTL_ASAHI_QUEUE_CREATE:
      return d3dkmt_queue_create(dev, req);
   case DRM_IOCTL_ASAHI_QUEUE_DESTROY:
      return d3dkmt_queue_destroy(dev, req);
   case DRM_IOCTL_ASAHI_GET_TIME:
      return d3dkmt_get_time(dev, req);
   default:
      errno = EINVAL;
      return -1;
   }
}

const agx_device_ops_t agx_device_d3dkmt_ops = {
   .bo_alloc = d3dkmt_bo_alloc,
   .bo_bind = d3dkmt_bo_bind,
   .bo_mmap = d3dkmt_bo_mmap,
   .get_params = d3dkmt_get_params,
   .submit = d3dkmt_submit,
   .bo_bind_object = d3dkmt_bo_bind_object,
   .bo_unbind_object = d3dkmt_bo_unbind_object,
   .simple_ioctl = d3dkmt_simple_ioctl,
   .bo_munmap = d3dkmt_bo_munmap,
   .bo_sync_for_device = d3dkmt_bo_sync_for_device,
   .bo_sync_for_cpu = d3dkmt_bo_sync_for_cpu,
   .bo_mark_cpu_dirty = d3dkmt_bo_mark_cpu_dirty,
   .bo_set_cpu_map_class = d3dkmt_bo_set_cpu_map_class,
   .bo_reset_cpu_dirty = d3dkmt_bo_reset_cpu_dirty,
   .bo_discard_cpu_dirty = d3dkmt_bo_discard_cpu_dirty,
   .bo_free = d3dkmt_bo_free,
};

/* -------------------------------------------------------------------------
 * Adapter enumeration and device open.
 * ------------------------------------------------------------------------- */
/*
 * Is this D3DKMT adapter ours?
 *
 * CONTRACT.md SS2.3 is explicit that VendorId/DeviceId are NOT reliable here:
 * the adapter is ACPI-enumerated (ACPI\NTAS0023), there is no PCI config
 * space, and whether dxgkrnl even calls DxgkDdiGetVendorAndDevice for a
 * non-PCI adapter is unproven. The identity blob is the only supported
 * discriminator, and getting it wrong means claiming another vendor's GPU.
 */
static bool
enum_log_enabled(void)
{
#if AGX_GPU_DIAG >= 2
   const char *value = getenv("NTAGX_ENUM_LOG");
   return value != NULL && value[0] != '\0' && value[0] != '0';
#else
   return false;
#endif
}

static bool
query_identity(const struct agx_d3dkmt_fns *fns, agx_kmt_handle adapter,
               struct agx_d3dkmt_policy *policy,
               struct agxw_adapter_identity *out)
{
   struct agxw_adapter_identity blob;

   memset(&blob, 0, sizeof(blob));

   struct agx_kmt_queryadapterinfo query = {
      .hAdapter = adapter,
      .Type = AGX_KMT_QAITYPE_UMDRIVERPRIVATE,
      .pPrivateDriverData = &blob,
      .PrivateDriverDataSize = sizeof(blob),
   };

   agx_kmt_status status = fns->QueryAdapterInfo(&query);
   if (!AGX_KMT_SUCCESS(status)) {
      if (enum_log_enabled())
         fprintf(stderr,
                 "NTAGX_ENUM identity adapter=0x%08x query=0x%08x\n",
                 (unsigned)adapter, (unsigned)status);
      return false;
   }

   int parsed =
      agx_d3dkmt_parse_identity(&blob, sizeof(blob), policy, out);
   if (enum_log_enabled())
      fprintf(stderr,
              "NTAGX_ENUM identity adapter=0x%08x query=0x%08x "
              "magic=0x%08x version=%u size=%u chip=0x%04x gen=%u "
              "variant=0x%02x revision=0x%02x contract=%u params=%u "
              "submit_private_max=%u parse=%d\n",
              (unsigned)adapter, (unsigned)status, blob.magic, blob.version,
              blob.struct_size, blob.chip_id, blob.gpu_generation,
              blob.gpu_variant, blob.gpu_revision, blob.contract_version,
              blob.params_blob_size, blob.submit_private_max_version, parsed);
   return parsed == AGX_D3DKMT_OK;
}

bool
agx_d3dkmt_enumerate_adapters(struct agx_kmt_luid *out, uint32_t capacity,
                              uint32_t *count)
{
   struct agx_d3dkmt_fns fns;
   struct agx_kmt_adapterinfo *infos = NULL;
   uint32_t found = 0;

   *count = 0;

   if (!agx_d3dkmt_load(&fns)) {
      if (enum_log_enabled())
         fprintf(stderr, "NTAGX_ENUM load=failed\n");
      return false;
   }

   struct agx_kmt_enumadapters2 e = {0};
   agx_kmt_status status = fns.EnumAdapters2(&e);
   if (enum_log_enabled())
      fprintf(stderr, "NTAGX_ENUM count status=0x%08x adapters=%u\n",
              (unsigned)status, e.NumAdapters);
   if (!AGX_KMT_SUCCESS(status))
      return false;
   if (e.NumAdapters == 0)
      return true;

   infos = calloc(e.NumAdapters, sizeof(*infos));
   if (infos == NULL)
      return false;

   e.pAdapters = infos;
   status = fns.EnumAdapters2(&e);
   if (enum_log_enabled())
      fprintf(stderr, "NTAGX_ENUM fill status=0x%08x adapters=%u\n",
              (unsigned)status, e.NumAdapters);
   if (!AGX_KMT_SUCCESS(status)) {
      free(infos);
      return false;
   }

   for (uint32_t i = 0; i < e.NumAdapters; ++i) {
      struct agx_d3dkmt_policy policy;
      struct agxw_adapter_identity id;

      if (enum_log_enabled())
         fprintf(stderr,
                 "NTAGX_ENUM candidate=%u adapter=0x%08x "
                 "luid=%08x:%08x\n",
                 i, (unsigned)infos[i].hAdapter,
                 (unsigned)infos[i].AdapterLuid.HighPart,
                 (unsigned)infos[i].AdapterLuid.LowPart);

      /* A foreign adapter's UMDRIVERPRIVATE blob fails the magic/version/
       * contract-version check and is skipped. */
      if (!query_identity(&fns, infos[i].hAdapter, &policy, &id))
         continue;

      if (found < capacity)
         out[found] = infos[i].AdapterLuid;
      found++;
   }

   free(infos);
   *count = found;
   if (enum_log_enabled())
      fprintf(stderr, "NTAGX_ENUM accepted=%u capacity=%u\n", found,
              capacity);
   return true;
}

static bool
d3dkmt_device_lease_acquire(struct agx_d3dkmt_winsys *ws)
{
   struct agx_d3dkmt_device_lease **link;
   struct agx_d3dkmt_device_lease *lease;
   struct agx_kmt_createdevice create = {.hAdapter = ws->adapter};
   agx_kmt_status status;

   AcquireSRWLockExclusive(&agx_d3dkmt_device_lease_lock);
   for (link = &agx_d3dkmt_device_leases; *link != NULL;
        link = &(*link)->next) {
      if (d3dkmt_luid_equal((*link)->luid, ws->luid))
         break;
   }
   lease = *link;
   if (lease != NULL) {
      if (lease->references == 0 || lease->references == UINT32_MAX) {
         ReleaseSRWLockExclusive(&agx_d3dkmt_device_lease_lock);
         errno = EIO;
         return false;
      }
      lease->references++;
      ws->device = lease->device;
      ws->device_lease_held = true;
      ReleaseSRWLockExclusive(&agx_d3dkmt_device_lease_lock);
      return true;
   }

   lease = calloc(1, sizeof(*lease));
   if (lease == NULL) {
      ReleaseSRWLockExclusive(&agx_d3dkmt_device_lease_lock);
      errno = ENOMEM;
      return false;
   }
   status = ws->fns.CreateDevice(&create);
   if (!AGX_KMT_SUCCESS(status)) {
      free(lease);
      ReleaseSRWLockExclusive(&agx_d3dkmt_device_lease_lock);
      agx_d3dkmt_fail(status);
      return false;
   }
   lease->luid = ws->luid;
   lease->device = create.hDevice;
   lease->references = 1;
   lease->next = agx_d3dkmt_device_leases;
   agx_d3dkmt_device_leases = lease;
   ws->device = create.hDevice;
   ws->device_lease_held = true;
   ReleaseSRWLockExclusive(&agx_d3dkmt_device_lease_lock);
   return true;
}

static void
d3dkmt_device_lease_release(struct agx_d3dkmt_winsys *ws)
{
   struct agx_d3dkmt_device_lease **link;
   struct agx_d3dkmt_device_lease *lease;

   if (!ws->device_lease_held)
      return;
   AcquireSRWLockExclusive(&agx_d3dkmt_device_lease_lock);
   for (link = &agx_d3dkmt_device_leases; *link != NULL;
        link = &(*link)->next) {
      if (d3dkmt_luid_equal((*link)->luid, ws->luid))
         break;
   }
   lease = *link;
   if (lease == NULL || lease->device != ws->device ||
       lease->references == 0) {
      fprintf(stderr, "D3DKMT device lease invariant failure\n");
      ReleaseSRWLockExclusive(&agx_d3dkmt_device_lease_lock);
      ws->device_lease_held = false;
      return;
   }
   lease->references--;
   if (lease->references == 0) {
      struct agx_kmt_destroydevice destroy = {.hDevice = lease->device};
      agx_kmt_status status = ws->fns.DestroyDevice(&destroy);

      if (AGX_KMT_SUCCESS(status)) {
         *link = lease->next;
         free(lease);
      } else {
         fprintf(stderr, "D3DKMTDestroyDevice failed status=0x%08x\n",
                 (unsigned)status);
      }
   }
   ReleaseSRWLockExclusive(&agx_d3dkmt_device_lease_lock);
   ws->device_lease_held = false;
}

bool
agx_d3dkmt_open_device(struct agx_device *dev, const struct agx_kmt_luid *luid)
{
   struct agx_d3dkmt_winsys *ws = calloc(1, sizeof(*ws));
   if (ws == NULL)
      return false;

   if (!agx_d3dkmt_load(&ws->fns))
      goto fail_alloc;

   ws->policy = agx_d3dkmt_policy_default();
   ws->luid = *luid;

   struct agx_kmt_openadapterfromluid open = {.AdapterLuid = *luid};
   if (!AGX_KMT_SUCCESS(ws->fns.OpenAdapterFromLuid(&open)))
      goto fail_alloc;
   ws->adapter = open.hAdapter;

   struct agxw_adapter_identity identity;
   if (!query_identity(&ws->fns, ws->adapter, &ws->policy, &identity))
      goto fail_adapter;

   /* GET_PARAMS is an escape (CONTRACT.md SS8) and its payload never
    * changes, so it is fetched once here and agx_open_device()'s
    * ops.get_params() copies out of this. */
   ws->params_size = identity.params_blob_size;
   ws->params = malloc(ws->params_size);
   if (ws->params == NULL)
      goto fail_adapter;
   int params_status =
      d3dkmt_escape(ws, AGXW_ESC_GET_PARAMS, ws->params, ws->params_size);
   if (enum_log_enabled())
      fprintf(stderr, "NTAGX_ENUM get_params result=%d errno=%d size=%u\n",
              params_status, errno, ws->params_size);
   if (params_status != 0)
      goto fail_params;

   /* Runtime-device bridge: instead of D3DKMTCreateDevice, adopt the D3D
    * runtime's device + callbacks from the Aurora UMD and install callback-
    * backed device-scope entries into ws->fns. Default (env unset) is the
    * unchanged D3DKMTCreateDevice path. See gpu/honeykrisp-port/bridge. */
   bool device_status;
   if (agx_d3dkmt_bridge_enabled())
      device_status = agx_d3dkmt_bridge_install(ws);
   else
      device_status = d3dkmt_device_lease_acquire(ws);
   if (enum_log_enabled())
      fprintf(stderr,
              "NTAGX_ENUM create_device status=%u device=0x%08x shared=%u\n",
              device_status ? 0u : 1u, (unsigned)ws->device,
              (unsigned)ws->device_lease_held);
   if (!device_status)
      goto fail_params;

   struct agx_kmt_createpagingqueue pq = {.hDevice = ws->device};
   agx_kmt_status paging_status = ws->fns.CreatePagingQueue(&pq);
   if (enum_log_enabled())
      fprintf(stderr,
              "NTAGX_ENUM paging_queue status=0x%08x device=0x%08x "
              "queue=0x%08x fence=0x%08x cpu=%p\n",
              (unsigned)paging_status, (unsigned)ws->device,
              (unsigned)pq.hPagingQueue, (unsigned)pq.hSyncObject,
              pq.FenceValueCPUVirtualAddress);
   if (!AGX_KMT_SUCCESS(paging_status))
      goto fail_device;
   ws->paging_queue = pq.hPagingQueue;
   ws->paging_fence = pq.hSyncObject;
   ws->paging_fence_cpu =
      (const volatile uint64_t *)pq.FenceValueCPUVirtualAddress;

   simple_mtx_init(&ws->lock, mtx_plain);
   agx_d3dkmt_pool_init(&ws->pool);
   agx_d3dkmt_sync_table_init(&ws->syncs);

   dev->hdev = ws;
   dev->fd = -1;
   dev->is_virtio = false;
   dev->ops = agx_device_d3dkmt_ops;
   dev->write_set_contract = true;
   simple_mtx_init(&dev->d3dkmt_dirty_lock, mtx_plain);
   simple_mtx_init(&dev->d3dkmt_submit_clean_lock, mtx_plain);
   dev->d3dkmt_next_bo_identity = 0;
   if (enum_log_enabled())
      fprintf(stderr, "NTAGX_ENUM open_device=ready\n");
   return true; /* caller then runs agx_open_device() */

fail_device:;
   if (agxw_bridge_is_sentinel(ws->device))
      agx_d3dkmt_bridge_uninstall(ws);
   d3dkmt_device_lease_release(ws);
fail_params:
   free(ws->params);
fail_adapter:;
   struct agx_kmt_closeadapter ca = {.hAdapter = ws->adapter};
   ws->fns.CloseAdapter(&ca);
fail_alloc:
   free(ws);
   return false;
}

void
agx_d3dkmt_close_device(struct agx_device *dev)
{
   struct agx_d3dkmt_winsys *ws = agx_d3dkmt_ws(dev);
   if (ws == NULL)
      return;

   /* Adapter teardown is the forced-retirement boundary for a transfer
    * context or render context whose completion became unknowable. */
   if (ws->xfer_ctx != 0) {
      struct agx_kmt_destroycontext dc = {.hContext = ws->xfer_ctx};
      agx_kmt_status status = ws->fns.DestroyContext(&dc);
      if (enum_log_enabled())
         fprintf(stderr,
                 "NTAGX_ENUM destroy_context xfer context=0x%08x status=0x%08x\n",
                 (unsigned)dc.hContext, (unsigned)status);
      ws->xfer_ctx = 0;
   }

   for (uint32_t i = 0; i < ws->ctx_capacity; ++i) {
      if (ws->ctxs[i].hContext == 0)
         continue;

      struct agx_kmt_destroycontext dc = {.hContext = ws->ctxs[i].hContext};
      agx_kmt_status status = ws->fns.DestroyContext(&dc);
      if (enum_log_enabled())
         fprintf(stderr,
                 "NTAGX_ENUM destroy_context close slot=%u context=0x%08x status=0x%08x\n",
                 i, (unsigned)dc.hContext, (unsigned)status);

      for (unsigned slot = 0; slot < AGX_D3DKMT_STAGING_RING; ++slot) {
         if (ws->ctxs[i].staging[slot] != NULL)
            agx_bo_unreference(dev, ws->ctxs[i].staging[slot]);
      }

      struct agx_kmt_destroysyncobj ds = {.hSyncObject = ws->ctxs[i].fence};
      ws->fns.DestroySynchronizationObject(&ds);
      free(ws->ctxs[i].submit_blob);
      free(ws->ctxs[i].dirty_snapshots);
      free(ws->ctxs[i].read_candidates);
   }
   free(ws->ctxs);

   if (ws->va_context != 0) {
      struct agx_kmt_destroycontext dc = {.hContext = ws->va_context};
      ws->fns.DestroyContext(&dc);
      ws->va_context = 0;
   }
   if (ws->va_fence != 0) {
      struct agx_kmt_destroysyncobj ds = {.hSyncObject = ws->va_fence};
      ws->fns.DestroySynchronizationObject(&ds);
      ws->va_fence = 0;
   }

   d3dkmt_release_va_lease(ws);

   struct agx_kmt_destroypagingqueue dpq = {.hPagingQueue = ws->paging_queue};
   ws->fns.DestroyPagingQueue(&dpq);

   d3dkmt_device_lease_release(ws);
   if (agxw_bridge_is_sentinel(ws->device))
      agx_d3dkmt_bridge_uninstall(ws);

   struct agx_kmt_closeadapter ca = {.hAdapter = ws->adapter};
   ws->fns.CloseAdapter(&ca);

   agx_d3dkmt_pool_finish(&ws->pool);
   agx_d3dkmt_sync_table_finish(&ws->syncs);
   if (enum_log_enabled())
      fprintf(stderr,
              "NTAGX_DIRTY submits=%llu unchanged=%llu precise_bytes=%llu "
              "conservative_bytes=%llu clean_fallbacks=%llu stale=%llu "
              "work_candidates=%llu conservative_candidates=%llu "
              "work_full_scans=%llu read_compactions=%llu gap_bytes=%llu\n",
              (unsigned long long)ws->dirty_submit_count,
              (unsigned long long)ws->dirty_unchanged_submits,
              (unsigned long long)ws->dirty_precise_bytes,
              (unsigned long long)ws->dirty_conservative_bytes,
              (unsigned long long)ws->dirty_clean_vector_fallbacks,
              (unsigned long long)ws->dirty_stale_snapshots,
              (unsigned long long)ws->dirty_worklist_candidates,
              (unsigned long long)ws->dirty_conservative_candidates,
              (unsigned long long)ws->dirty_worklist_full_scans,
              (unsigned long long)ws->read_set_compactions,
              (unsigned long long)ws->read_set_compacted_gap_bytes);
   free(dev->d3dkmt_worksets);
   dev->d3dkmt_worksets = NULL;
   dev->d3dkmt_workset_words = 0;
   simple_mtx_destroy(&dev->d3dkmt_submit_clean_lock);
   simple_mtx_destroy(&dev->d3dkmt_dirty_lock);
   simple_mtx_destroy(&ws->lock);
   free(ws->params);
   free(ws);

   dev->hdev = NULL;
}

struct agx_kmt_luid
agx_d3dkmt_device_luid(const struct agx_device *dev)
{
   const struct agx_d3dkmt_winsys *ws =
      (const struct agx_d3dkmt_winsys *)dev->hdev;

   return ws ? ws->luid : (struct agx_kmt_luid){0, 0};
}

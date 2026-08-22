/*
 * Copyright 2026 the Asahi Linux contributors
 * SPDX-License-Identifier: MIT
 */
#pragma once

/*
 * Internal state shared by agx_device_d3dkmt.c and agx_sync_d3dkmt.c. Not
 * installed, not included by anything outside those two files: the public
 * surface is agx_device_d3dkmt.h.
 */

#include <windows.h>

#include "util/simple_mtx.h"

#include "agx_d3dkmt.h"
#include "agx_d3dkmt_core.h"

/*
 * Staging ring for AGX command buffers.
 *
 * hk_queue.c builds the flat drm_asahi_cmd_header + payload stream in a
 * util_dynarray of plain host memory and hands the winsys a userspace pointer
 * (hk_queue.c:864-872). D3DKMTSubmitCommand instead takes a GPU virtual
 * address, so the winsys copies the stream into a GPU-mapped BO. Two buffers
 * per context are enough to keep the CPU one submission ahead of the GPU
 * without unbounded memory; the ring slot is not reused until the context
 * fence proves the previous submission that used it has retired.
 */
#define AGX_D3DKMT_STAGING_RING 2

struct agx_d3dkmt_submit_blob {
   struct agxw_submit_private_v3 header;
   struct agxw_submit_read_range ranges[AGXW_MAX_READ_RANGES_PER_SUBMIT];
};

struct agx_d3dkmt_ctx {
   agx_kmt_handle hContext; /* 0 = free slot */

   /* Per-context completion fence. Owned by the winsys and separate from
    * anything Honeykrisp signals, so staging reuse never depends on the
    * caller's choice of out-syncs. */
   agx_kmt_handle fence;
   const volatile uint64_t *fence_cpu;
   uint64_t fence_next;
   bool submit_failed;
   bool completion_unknown;
   uint32_t submit_refs;

   struct agx_bo *staging[AGX_D3DKMT_STAGING_RING];
   uint64_t staging_retire[AGX_D3DKMT_STAGING_RING];
   unsigned staging_slot;
   struct agx_d3dkmt_submit_blob *submit_blob;
};

#if AGX_GPU_DIAG >= 2
#define AGX_D3DKMT_DIAG_SUBMIT_RING 16u
#define AGX_D3DKMT_DIAG_SYNCS 4u

struct agx_d3dkmt_diag_submit {
   uint64_t sequence;
   uint64_t tick_ms;
   uint32_t queue_id;
   uint32_t cmdbuf_size;
   uint32_t in_count;
   uint32_t out_count;
   agx_kmt_handle context;
   uint32_t callback_mask;
   uint32_t wait_handles[AGX_D3DKMT_DIAG_SYNCS];
   uint64_t wait_values[AGX_D3DKMT_DIAG_SYNCS];
   uint32_t signal_handles[AGX_D3DKMT_DIAG_SYNCS];
   uint64_t signal_values[AGX_D3DKMT_DIAG_SYNCS];
};
#endif

struct agx_d3dkmt_winsys {
   struct agx_d3dkmt_fns fns;
   struct agx_d3dkmt_policy policy;
   struct agx_kmt_luid luid;

   agx_kmt_handle adapter;
   agx_kmt_handle device;
   bool device_lease_held;

   /* Paging queue: every GPU VA and residency operation is asynchronous on
    * WDDM and completes on this queue's monitored fence. DRM's VM_BIND is
    * synchronous, so the winsys waits on it before returning -- see
    * d3dkmt_wait_paging_fence(). */
   agx_kmt_handle paging_queue;
   agx_kmt_handle paging_fence;
   const volatile uint64_t *paging_fence_cpu;

   /* UpdateGpuVirtualAddress is a rendering-context interlock, not a paging-
    * queue operation. Keep one ordinary WDDM virtual-addressing context and
    * its monitored fence alive for the device lifetime. Fence N is the value
    * the update waits for; successful completion signals N + 1. */
   agx_kmt_handle va_context;
   agx_kmt_handle va_fence;
   const volatile uint64_t *va_fence_cpu;
   uint64_t va_fence_next;

   simple_mtx_t lock; /* guards pool, syncs, ctxs and the staging rings */
   struct agx_d3dkmt_handle_pool pool;
   struct agx_d3dkmt_sync_table syncs;

   struct agx_d3dkmt_ctx *ctxs;
   uint32_t ctx_capacity;
   uint32_t active_submits;
   bool device_lost;

#if AGX_GPU_DIAG >= 2
   struct agx_d3dkmt_diag_submit
      diag_submits[AGX_D3DKMT_DIAG_SUBMIT_RING];
   uint64_t diag_submit_sequence;
   bool diag_sync_gap_dumped;
#endif

   /* Protected by agx_device::bo_map_lock, which already protects the BO
    * census used to build it.  CPU stores do not invalidate this vector:
    * all members remain in every per-context submit blob so KMD performs the
    * required ownership handoff after every possible host write. */
   struct agxw_submit_read_range
      read_set[AGXW_MAX_READ_RANGES_PER_SUBMIT];
   uint32_t read_set_count;
   uint32_t read_set_generation;
   bool read_set_valid;

   /* Lazily created SynchronizationOnly context used to implement DRM
    * syncobj transfer as a GPU-side wait-then-signal. */
   agx_kmt_handle xfer_ctx;

   /* The user GPU VA window, reserved at VM_CREATE time. Up to two ranges:
    * CONTRACT.md SS4.2 puts a 16 KiB kernel/firmware carve-out page in the
    * MIDDLE of the window, so it is not one contiguous reservation. */
   struct agx_d3dkmt_va_range va[2];
   uint32_t va_count;

   /* KMTQAITYPE_UMDRIVERPRIVATE payload: the AGX params blob verbatim. */
   void *params;
   uint32_t params_size;
};

static inline void
agx_d3dkmt_mark_device_lost(struct agx_d3dkmt_winsys *ws,
                            struct agx_d3dkmt_ctx *ctx)
{
#if AGX_GPU_DIAG >= 2
   {
      char aurora_line[160];
      int aurora_len = _snprintf_s(aurora_line, sizeof(aurora_line), _TRUNCATE,
                                   "agx_d3dkmt_device_lost: ws=%p already=%d\n",
                                   (void *)ws, (int)ws->device_lost);
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
   simple_mtx_lock(&ws->lock);
   ws->device_lost = true;
   if (ctx != NULL)
      ctx->submit_failed = true;
   simple_mtx_unlock(&ws->lock);
}

static inline void
agx_d3dkmt_mark_completion_unknown(struct agx_d3dkmt_winsys *ws,
                                    struct agx_d3dkmt_ctx *ctx)
{
   simple_mtx_lock(&ws->lock);
   ws->device_lost = true;
   ctx->submit_failed = true;
   ctx->completion_unknown = true;
   simple_mtx_unlock(&ws->lock);
}

static inline struct agx_d3dkmt_winsys *
agx_d3dkmt_ws(struct agx_device *dev)
{
   return (struct agx_d3dkmt_winsys *)dev->hdev;
}

/*
 * Block until `fence_cpu` reaches `value`, or `timeout_ms` elapses. Infinite
 * waits use D3DKMT's synchronous form. Finite waits poll the required
 * CPU-visible monitored-fence mapping because D3DKMT documents no way to
 * cancel an outstanding asynchronous event wait.
 */
int agx_d3dkmt_wait_fence(struct agx_d3dkmt_winsys *ws, agx_kmt_handle fence,
                          const volatile uint64_t *fence_cpu, uint64_t value,
                          uint32_t timeout_ms);

int agx_d3dkmt_poll_cpu_fences(
   const volatile uint64_t *const *current, const uint64_t *wanted,
   uint32_t count, bool wait_any, uint32_t timeout_ms,
   uint32_t *first_signaled);

/* Wait for a paging-queue operation identified by `value` to complete. */
int agx_d3dkmt_wait_paging(struct agx_d3dkmt_winsys *ws, uint64_t value);

/* Map an NTSTATUS onto the errno a drmIoctl() caller would have seen, and
 * store it in errno. Returns -1 so call sites can `return
 * agx_d3dkmt_fail(st);`. */
int agx_d3dkmt_fail(agx_kmt_status st);

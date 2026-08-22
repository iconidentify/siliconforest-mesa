/*
 * Copyright 2026 the Asahi Linux contributors
 * SPDX-License-Identifier: MIT
 */

/*
 * util_sync_provider over D3DKMT monitored fences -- the Windows counterpart
 * of util_sync_provider_drm (src/util/u_sync_provider.c), which is a thin
 * wrapper over the drmSyncobj* ioctls.
 *
 * The DRM syncobj model and the WDDM monitored-fence model differ in three
 * places, and all three are handled here rather than pushed up into
 * Honeykrisp:
 *
 *  1. RESET. A DRM binary syncobj can be reset to unsignalled; a monitored
 *     fence only ever increases. Solved with a userspace epoch -- see the
 *     "binary syncobjs" section of agx_d3dkmt_core.h. Costs nothing and needs
 *     no kernel support.
 *
 *  2. TIMEOUTS. drmSyncobjWait takes an absolute CLOCK_MONOTONIC deadline;
 *     D3DKMTWaitForSynchronizationObjectFromCpu takes no timeout and exposes
 *     no cancellation for its asynchronous event form. Infinite waits use
 *     the synchronous NULL-event form. Finite waits poll the contractually
 *     CPU-visible fence storage, so no event can outlive or alias a caller.
 *
 *  3. TRANSFER. drmSyncobjTransfer makes dst[dst_point] depend on
 *     src[src_point]. WDDM has no CPU-side equivalent, but it has an exact
 *     GPU-side one: a context created with
 *     D3DDDI_CREATECONTEXTFLAGS.SynchronizationOnly -- a context that exists
 *     precisely to issue waits and signals and never renders. Transfer
 *     becomes WaitForSynchronizationObjectFromGpu followed by
 *     SignalSynchronizationObjectFromGpu on that context. Honeykrisp uses
 *     transfer only in queue_submit_empty() (hk_queue.c:46-80), i.e. the
 *     "no work, just fence plumbing" path, which is exactly what a
 *     synchronization-only context is for. No escape, no polling thread.
 *
 * fd-based slots (handle_to_fd, fd_to_handle, import_sync_file,
 * export_sync_file) remain hard -EINVAL stubs: every extension that could
 * reach them is Windows-disabled. The WDDM-native replacements for
 * VK_KHR_external_semaphore_win32 are D3DKMTShareObjects and
 * D3DKMTOpenSyncObjectFromNtHandle2, which this port does not yet wire up --
 * but note that the fence objects created here are already the right KIND of
 * object for that, which was not true of the AGXKMD syncobjs.
 *
 * NOT RUNTIME-VERIFIED: no miniport, no hardware. The parts that ARE verified
 * are the epoch arithmetic, the timeout conversion and first_signaled
 * recovery, all of which live in agx_d3dkmt_core.c and are covered by
 * agx_d3dkmt_core_tests.c.
 */

#include "agx_device_d3dkmt.h"
#include "agx_d3dkmt_priv.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "util/u_memory.h"
#include "util/u_sync_provider.h"

#include "drm-uapi/drm.h"

#include "agxw_contract.h"

struct util_sync_provider_d3dkmt {
   struct util_sync_provider base;
   struct agx_device *dev;
};

static struct agx_d3dkmt_winsys *
ws_of(struct util_sync_provider *p)
{
   struct util_sync_provider_d3dkmt *d = (struct util_sync_provider_d3dkmt *)p;
   return agx_d3dkmt_ws(d->dev);
}

/* CLOCK_MONOTONIC nanoseconds, matching what DRM's absolute timeouts are
 * measured against. QueryPerformanceCounter is the monotonic clock on
 * Windows. */
static int64_t
now_nsec(void)
{
   LARGE_INTEGER freq, ctr;

   if (!QueryPerformanceFrequency(&freq) || freq.QuadPart == 0)
      return 0;
   QueryPerformanceCounter(&ctr);

   /* Split to avoid overflowing at ~292 years of uptime at 10 MHz. */
   int64_t sec = ctr.QuadPart / freq.QuadPart;
   int64_t rem = ctr.QuadPart % freq.QuadPart;
   return sec * 1000000000ll + (rem * 1000000000ll) / freq.QuadPart;
}

static int
d3dkmt_syncobj_create(struct util_sync_provider *p, uint32_t flags,
                      uint32_t *handle)
{
   struct agx_d3dkmt_winsys *ws = ws_of(p);
   simple_mtx_lock(&ws->lock);
   bool lost = ws->device_lost;
   simple_mtx_unlock(&ws->lock);
   if (lost) {
      errno = EIO;
      return -1;
   }

   /*
    * DRM_SYNCOBJ_CREATE_SIGNALED means "already signalled". With the epoch
    * model that is simply an initial fence value of 1, since binary-signalled
    * is defined as fence >= base + 1 and base starts at 0.
    */
   const uint64_t initial = (flags & DRM_SYNCOBJ_CREATE_SIGNALED) ? 1 : 0;

   struct agx_kmt_createsyncobj2 create = {
      .hDevice = ws->device,
      .Info =
         {
            .Type = AGX_KMT_SYNC_MONITORED_FENCE,
            .MonitoredFence = {.InitialFenceValue = initial},
         },
   };

   agx_kmt_status st = ws->fns.CreateSynchronizationObject2(&create);
   if (!AGX_KMT_SUCCESS(st))
      return agx_d3dkmt_fail(st);

   simple_mtx_lock(&ws->lock);
   uint32_t h = ws->device_lost
                   ? 0
                   : agx_d3dkmt_sync_table_add(
                        &ws->syncs, create.hSyncObject,
                        (const volatile uint64_t *)create.Info.MonitoredFence
                           .FenceValueCPUVirtualAddress,
                        initial);
   lost = ws->device_lost;
   simple_mtx_unlock(&ws->lock);

   if (h == 0) {
      struct agx_kmt_destroysyncobj d = {.hSyncObject = create.hSyncObject};
      ws->fns.DestroySynchronizationObject(&d);
      errno = lost ? EIO : ENOMEM;
      return -1;
   }

   *handle = h;
   return 0;
}

static int
d3dkmt_syncobj_destroy(struct util_sync_provider *p, uint32_t handle)
{
   struct agx_d3dkmt_winsys *ws = ws_of(p);

   simple_mtx_lock(&ws->lock);
   agx_kmt_handle kmt = agx_d3dkmt_sync_table_remove(&ws->syncs, handle);
   simple_mtx_unlock(&ws->lock);

   if (kmt == 0) {
      errno = EINVAL;
      return -1;
   }

   struct agx_kmt_destroysyncobj d = {.hSyncObject = kmt};
   agx_kmt_status st = ws->fns.DestroySynchronizationObject(&d);
   return AGX_KMT_SUCCESS(st) ? 0 : agx_d3dkmt_fail(st);
}

/* fd-based slots: see the file header. */
static int
d3dkmt_syncobj_handle_to_fd(struct util_sync_provider *p, uint32_t handle,
                            int *out_obj_fd)
{
   (void)p, (void)handle, (void)out_obj_fd;
   return -EINVAL;
}

static int
d3dkmt_syncobj_fd_to_handle(struct util_sync_provider *p, int obj_fd,
                            uint32_t *handle)
{
   (void)p, (void)obj_fd, (void)handle;
   return -EINVAL;
}

static int
d3dkmt_syncobj_import_sync_file(struct util_sync_provider *p, uint32_t handle,
                                int sync_file_fd)
{
   (void)p, (void)handle, (void)sync_file_fd;
   return -EINVAL;
}

static int
d3dkmt_syncobj_export_sync_file(struct util_sync_provider *p, uint32_t handle,
                                int *out_sync_file_fd)
{
   (void)p, (void)handle, (void)out_sync_file_fd;
   return -EINVAL;
}

/*
 * The one wait implementation, shared by wait() and timeline_wait(): the only
 * difference between them is where the per-handle fence value comes from.
 */
static int
d3dkmt_wait_common(struct util_sync_provider *p, uint32_t *handles,
                   const uint64_t *points, unsigned num_handles,
                   int64_t timeout_nsec, unsigned flags,
                   uint32_t *first_signaled)
{
   struct agx_d3dkmt_winsys *ws = ws_of(p);
   agx_kmt_handle *kmt = NULL;
   uint64_t *values = NULL;
   const volatile uint64_t **current = NULL;
   int ret = -1;

   if (num_handles == 0)
      return 0;

   kmt = malloc((size_t)num_handles * sizeof(*kmt));
   values = malloc((size_t)num_handles * sizeof(*values));
   current = malloc((size_t)num_handles * sizeof(*current));
   if (kmt == NULL || values == NULL || current == NULL) {
      errno = ENOMEM;
      goto out;
   }

   simple_mtx_lock(&ws->lock);
   if (ws->device_lost) {
      simple_mtx_unlock(&ws->lock);
      errno = EIO;
      goto out;
   }
   for (unsigned i = 0; i < num_handles; ++i) {
      struct agx_d3dkmt_sync *s =
         agx_d3dkmt_sync_table_get(&ws->syncs, handles[i]);
      if (s == NULL) {
         simple_mtx_unlock(&ws->lock);
         errno = EINVAL;
         goto out;
      }
      kmt[i] = s->kmt;
      values[i] = points ? points[i] : agx_d3dkmt_binary_wait_value(s);
      current[i] = s->cpu_va;
   }
   simple_mtx_unlock(&ws->lock);

   /*
    * DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT has no analogue and needs none: a
    * monitored fence has no "not yet submitted" state, only a value that has
    * or has not been reached. DRM's flag exists because a syncobj can hold a
    * NULL fence; a monitored fence cannot.
    */
   const bool wait_any = !(flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL);
   uint32_t ms = agx_d3dkmt_timeout_to_ms(timeout_nsec, now_nsec());

   enum agx_d3dkmt_cpu_wait_state state = agx_d3dkmt_cpu_wait_probe(
      current, values, num_handles, wait_any, first_signaled);
   if (state == AGX_D3DKMT_CPU_WAIT_SATISFIED) {
      MemoryBarrier();
      ret = 0;
      goto out;
   }
   if (state == AGX_D3DKMT_CPU_WAIT_INVALID) {
      errno = EIO;
      goto out;
   }

   if (ms != AGX_D3DKMT_INFINITE) {
      ret = agx_d3dkmt_poll_cpu_fences(current, values, num_handles, wait_any,
                                        ms, first_signaled);
      goto out;
   }

   struct agx_kmt_waitsyncfromcpu wait = {
      .hDevice = ws->device,
      .ObjectCount = num_handles,
      .ObjectHandleArray = kmt,
      .FenceValueArray = values,
      .hAsyncEvent = NULL,
      .Flags = wait_any ? AGX_KMT_WAITCPU_WAITANY : 0,
   };

   agx_kmt_status st = ws->fns.WaitForSynchronizationObjectFromCpu(&wait);
   if (!AGX_KMT_SUCCESS(st)) {
      ret = agx_d3dkmt_fail(st);
      goto out;
   }

   MemoryBarrier();
   state = agx_d3dkmt_cpu_wait_probe(current, values, num_handles, wait_any,
                                      first_signaled);
   if (state != AGX_D3DKMT_CPU_WAIT_SATISFIED) {
      errno = EIO;
      goto out;
   }

   ret = 0;

out:
   free(kmt);
   free(values);
   free(current);
   return ret;
}

static int
d3dkmt_syncobj_wait(struct util_sync_provider *p, uint32_t *handles,
                    unsigned num_handles, int64_t timeout_nsec, unsigned flags,
                    uint32_t *first_signaled)
{
   return d3dkmt_wait_common(p, handles, NULL, num_handles, timeout_nsec, flags,
                             first_signaled);
}

static int
d3dkmt_syncobj_timeline_wait(struct util_sync_provider *p, uint32_t *handles,
                             uint64_t *points, unsigned num_handles,
                             int64_t timeout_nsec, unsigned flags,
                             uint32_t *first_signaled)
{
   return d3dkmt_wait_common(p, handles, points, num_handles, timeout_nsec,
                             flags, first_signaled);
}

/*
 * Binary reset. Nothing is asked of the kernel at all: the fence stays where
 * it is and the epoch moves up to meet it.
 */
static int
d3dkmt_syncobj_reset(struct util_sync_provider *p, const uint32_t *handles,
                     uint32_t handle_count)
{
   struct agx_d3dkmt_winsys *ws = ws_of(p);

   simple_mtx_lock(&ws->lock);
   if (ws->device_lost) {
      simple_mtx_unlock(&ws->lock);
      errno = EIO;
      return -1;
   }
   for (uint32_t i = 0; i < handle_count; ++i) {
      struct agx_d3dkmt_sync *s =
         agx_d3dkmt_sync_table_get(&ws->syncs, handles[i]);
      if (s == NULL) {
         simple_mtx_unlock(&ws->lock);
         errno = EINVAL;
         return -1;
      }
      s->base = agx_d3dkmt_binary_reset_base(s);
   }
   simple_mtx_unlock(&ws->lock);

   return 0;
}

static int
d3dkmt_syncobj_signal(struct util_sync_provider *p, const uint32_t *handles,
                      uint32_t handle_count)
{
   struct agx_d3dkmt_winsys *ws = ws_of(p);
   agx_kmt_handle *kmt = NULL;
   uint64_t *values = NULL;
   uint32_t n = 0;
   int ret = -1;

   if (handle_count == 0)
      return 0;

   kmt = malloc((size_t)handle_count * sizeof(*kmt));
   values = malloc((size_t)handle_count * sizeof(*values));
   if (kmt == NULL || values == NULL) {
      errno = ENOMEM;
      goto out;
   }

   simple_mtx_lock(&ws->lock);
   if (ws->device_lost) {
      simple_mtx_unlock(&ws->lock);
      errno = EIO;
      goto out;
   }
   for (uint32_t i = 0; i < handle_count; ++i) {
      struct agx_d3dkmt_sync *s =
         agx_d3dkmt_sync_table_get(&ws->syncs, handles[i]);
      if (s == NULL) {
         simple_mtx_unlock(&ws->lock);
         errno = EINVAL;
         goto out;
      }

      /* Signalling an already-signalled binary syncobj is a no-op in DRM but
       * would be an illegal fence rewind here, so drop it. */
      bool skip = false;
      uint64_t v = agx_d3dkmt_binary_signal_value(s, &skip);
      if (skip)
         continue;

      kmt[n] = s->kmt;
      values[n] = v;
      s->last_request = v;
      n++;
   }
   simple_mtx_unlock(&ws->lock);

   if (n == 0) {
      ret = 0;
      goto out;
   }

   struct agx_kmt_signalsyncfromcpu signal = {
      .hDevice = ws->device,
      .ObjectCount = n,
      .ObjectHandleArray = kmt,
      .FenceValueArray = values,
   };

   agx_kmt_status st = ws->fns.SignalSynchronizationObjectFromCpu(&signal);
   if (!AGX_KMT_SUCCESS(st)) {
      ret = agx_d3dkmt_fail(st);
      agx_d3dkmt_mark_device_lost(ws, NULL);
   } else {
      ret = 0;
   }

out:
   free(kmt);
   free(values);
   return ret;
}

static int
d3dkmt_syncobj_timeline_signal(struct util_sync_provider *p,
                               const uint32_t *handles, uint64_t *points,
                               uint32_t handle_count)
{
   struct agx_d3dkmt_winsys *ws = ws_of(p);
   agx_kmt_handle *kmt = NULL;
   int ret = -1;

   if (handle_count == 0)
      return 0;

   kmt = malloc((size_t)handle_count * sizeof(*kmt));
   if (kmt == NULL) {
      errno = ENOMEM;
      goto out;
   }

   simple_mtx_lock(&ws->lock);
   if (ws->device_lost) {
      simple_mtx_unlock(&ws->lock);
      errno = EIO;
      goto out;
   }
   for (uint32_t i = 0; i < handle_count; ++i) {
      struct agx_d3dkmt_sync *s =
         agx_d3dkmt_sync_table_get(&ws->syncs, handles[i]);
      if (s == NULL) {
         simple_mtx_unlock(&ws->lock);
         errno = EINVAL;
         goto out;
      }
      kmt[i] = s->kmt;
      if (points[i] > s->last_request)
         s->last_request = points[i];
   }
   simple_mtx_unlock(&ws->lock);

   struct agx_kmt_signalsyncfromcpu signal = {
      .hDevice = ws->device,
      .ObjectCount = handle_count,
      .ObjectHandleArray = kmt,
      .FenceValueArray = points,
   };

   agx_kmt_status st = ws->fns.SignalSynchronizationObjectFromCpu(&signal);
   if (!AGX_KMT_SUCCESS(st)) {
      ret = agx_d3dkmt_fail(st);
      agx_d3dkmt_mark_device_lost(ws, NULL);
   } else {
      ret = 0;
   }

out:
   free(kmt);
   return ret;
}

/*
 * Non-blocking read of a timeline value. This is the slot that makes Q5
 * load-bearing: with FenceValueCPUVirtualAddress it is a plain load, and
 * without it there is no non-blocking read in the entire D3DKMT surface --
 * agx_d3dkmt_sync_current() then reports the last value we requested, which
 * is an over-estimate of completion rather than a block.
 */
static int
d3dkmt_syncobj_query(struct util_sync_provider *p, uint32_t *handles,
                     uint64_t *points, uint32_t handle_count, uint32_t flags)
{
   struct agx_d3dkmt_winsys *ws = ws_of(p);

   simple_mtx_lock(&ws->lock);
   if (ws->device_lost) {
      simple_mtx_unlock(&ws->lock);
      errno = EIO;
      return -1;
   }
   for (uint32_t i = 0; i < handle_count; ++i) {
      struct agx_d3dkmt_sync *s =
         agx_d3dkmt_sync_table_get(&ws->syncs, handles[i]);
      if (s == NULL) {
         simple_mtx_unlock(&ws->lock);
         errno = EINVAL;
         return -1;
      }

      /* DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED asks for the last value
       * SUBMITTED rather than the last SIGNALLED; that is exactly what our
       * own bookkeeping tracks. */
      points[i] = (flags & DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED)
                     ? s->last_request
                     : agx_d3dkmt_sync_current(s);
   }
   simple_mtx_unlock(&ws->lock);

   return 0;
}

/* Lazily create the SynchronizationOnly context transfer needs. Caller holds
 * ws->lock. */
static agx_kmt_handle
xfer_context(struct agx_d3dkmt_winsys *ws)
{
   if (ws->xfer_ctx != 0)
      return ws->xfer_ctx;

   struct agxw_context_private priv = {
      .version = AGXW_CONTEXT_PRIVATE_VERSION,
      .struct_size = sizeof(priv),
      .priority = AGXW_PRIORITY_MEDIUM,
      .flags = AGXW_CONTEXT_PRIVATE_FLAG_VA_INTERLOCK,
      .usc_exec_base = ws->policy.usc_base,
   };
   if (agxw_context_private_validate(&priv, sizeof(priv), ws->policy.usc_base,
                                     ws->policy.usc_size) != 0) {
      errno = EINVAL;
      return 0;
   }

   struct agx_kmt_createcontextvirtual create = {
      .hDevice = ws->device,
      .NodeOrdinal = ws->policy.node_ordinal,
      .EngineAffinity = 1,
      .pPrivateDriverData = &priv,
      .PrivateDriverDataSize = sizeof(priv),
   };

   agx_kmt_status st = ws->fns.CreateContextVirtual(&create);
   if (!AGX_KMT_SUCCESS(st)) {
      agx_d3dkmt_fail(st);
      return 0;
   }

   ws->xfer_ctx = create.hContext;
   return ws->xfer_ctx;
}

static int
d3dkmt_syncobj_transfer(struct util_sync_provider *p, uint32_t dst_handle,
                        uint64_t dst_point, uint32_t src_handle,
                        uint64_t src_point, uint32_t flags)
{
   struct agx_d3dkmt_winsys *ws = ws_of(p);
   agx_kmt_handle src_kmt, dst_kmt, ctx;

   (void)flags; /* drm.h defines none */

   simple_mtx_lock(&ws->lock);
   if (ws->device_lost) {
      simple_mtx_unlock(&ws->lock);
      errno = EIO;
      return -1;
   }
   struct agx_d3dkmt_sync *src =
      agx_d3dkmt_sync_table_get(&ws->syncs, src_handle);
   struct agx_d3dkmt_sync *dst =
      agx_d3dkmt_sync_table_get(&ws->syncs, dst_handle);
   if (src == NULL || dst == NULL) {
      simple_mtx_unlock(&ws->lock);
      errno = EINVAL;
      return -1;
   }

   uint64_t src_value, dst_value;
   agx_d3dkmt_sync_transfer_values(src, dst, src_point, dst_point,
                                   &src_value, &dst_value);
   src_kmt = src->kmt;
   dst_kmt = dst->kmt;
   ctx = xfer_context(ws);
   if (ctx != 0 && dst_value > dst->last_request)
      dst->last_request = dst_value;
   simple_mtx_unlock(&ws->lock);

   if (ctx == 0) {
      errno = ENOTSUP;
      return -1;
   }

   const agx_kmt_handle src_arr[1] = {src_kmt};
   const uint64_t src_vals[1] = {src_value};
   struct agx_kmt_waitsyncfromgpu wait = {
      .hContext = ctx,
      .ObjectCount = 1,
      .ObjectHandleArray = src_arr,
      .MonitoredFenceValueArray = src_vals,
   };

   agx_kmt_status st = ws->fns.WaitForSynchronizationObjectFromGpu(&wait);
   if (!AGX_KMT_SUCCESS(st)) {
      int ret = agx_d3dkmt_fail(st);
      agx_d3dkmt_mark_device_lost(ws, NULL);
      return ret;
   }

   const agx_kmt_handle dst_arr[1] = {dst_kmt};
   const uint64_t dst_vals[1] = {dst_value};
   struct agx_kmt_signalsyncfromgpu signal = {
      .hContext = ctx,
      .ObjectCount = 1,
      .ObjectHandleArray = dst_arr,
      .MonitoredFenceValueArray = dst_vals,
   };

   st = ws->fns.SignalSynchronizationObjectFromGpu(&signal);
   if (!AGX_KMT_SUCCESS(st)) {
      int ret = agx_d3dkmt_fail(st);
      agx_d3dkmt_mark_device_lost(ws, NULL);
      return ret;
   }
   return 0;
}

static void
d3dkmt_sync_provider_finalize(struct util_sync_provider *p)
{
   free(p);
}

static struct util_sync_provider *
d3dkmt_sync_provider_clone(struct util_sync_provider *p)
{
   struct util_sync_provider_d3dkmt *d = (struct util_sync_provider_d3dkmt *)p;

   /* All state lives in the winsys, so a clone is another view of the same
    * device -- no handle duplication and no second syncobj table, which is
    * what makes handles minted by one provider usable by the other and by
    * submit(). */
   return util_sync_provider_d3dkmt(d->dev);
}

struct util_sync_provider *
util_sync_provider_d3dkmt(struct agx_device *dev)
{
   struct util_sync_provider_d3dkmt *d =
      CALLOC_STRUCT(util_sync_provider_d3dkmt);
   if (d == NULL)
      return NULL;

   d->dev = dev;
   d->base = (struct util_sync_provider){
      .create = d3dkmt_syncobj_create,
      .destroy = d3dkmt_syncobj_destroy,
      .handle_to_fd = d3dkmt_syncobj_handle_to_fd,
      .fd_to_handle = d3dkmt_syncobj_fd_to_handle,
      .import_sync_file = d3dkmt_syncobj_import_sync_file,
      .export_sync_file = d3dkmt_syncobj_export_sync_file,
      .wait = d3dkmt_syncobj_wait,
      .reset = d3dkmt_syncobj_reset,
      .signal = d3dkmt_syncobj_signal,
      /* Unconditional, unlike util_sync_provider_drm's DRM_CAP probe:
       * monitored fences ARE timelines, so VK_SYNC_FEATURE_TIMELINE is
       * always available (vk_drm_syncobj.c requires both slots non-NULL). */
      .timeline_signal = d3dkmt_syncobj_timeline_signal,
      .timeline_wait = d3dkmt_syncobj_timeline_wait,
      .query = d3dkmt_syncobj_query,
      .transfer = d3dkmt_syncobj_transfer,
      .finalize = d3dkmt_sync_provider_finalize,
      .clone = d3dkmt_sync_provider_clone,
   };

   return &d->base;
}

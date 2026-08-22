/*
 * Copyright 2026 the Asahi Linux contributors
 * SPDX-License-Identifier: MIT
 */

/*
 * honeykrisp-h1-execution.md SS3.1: `util_sync_provider_ntagx`, the Windows
 * counterpart of `util_sync_provider_drm` (src/util/u_sync_provider.c,
 * VERIFIED template -- one drmSyncobj* call per function, copied here
 * verb-for-verb but issuing ntagx_ioctl()/DeviceIoControl instead). Kept
 * with the winsys (src/asahi/lib/, alongside agx_device_win.c) rather than
 * src/util/ so it can reuse agx_device_win.h's ntagx_ioctl() without a
 * second copy of the DeviceIoControl transport (plan SS3.1: "keep with the
 * winsys so it can use ntagx_ioctl").
 *
 * Every entry point below issues exactly one of the nine syncobj ioctls
 * kmd-uapi-core/kmd_uapi.h pins (ids 11-19, PART 2), using the SAME
 * `struct ntasi_kmd_uapi_syncobj_*` wire shapes the kmd-syncobj-core replay
 * tests already exercise against the KMD-side semantic model
 * (kmd-syncobj-core/kmd_syncobj.h) -- this file is the marshalling layer
 * over that model's real ioctl surface, not a reimplementation of the
 * model itself. See plan SS3.1's provider-fp -> ioctl -> model-op table for
 * the full citation chain.
 *
 * fd-based ops (handle_to_fd, fd_to_handle, import_sync_file,
 * export_sync_file) are hard stubs returning -EINVAL: fd export/import is
 * Linux-only, and every extension that could reach them
 * (KHR_external_fence_fd, KHR_external_semaphore_fd, ...) is compiled out
 * on Windows (honeykrisp-h1-execution.md SS2.2 row P2), so no ioctl id was
 * ever minted for them (kmd_uapi.h's own header comment says as much).
 *
 * NOT build-verified: no llvm-mingw/MSVC toolchain is installed on this
 * host. See the H1 patch series verification notes for exactly what WAS
 * checked on this macOS/no-mingw host.
 */

#include "agx_device_win.h"

#include <windows.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include "kmd_uapi.h"
#include "util/u_memory.h"
#include "util/u_sync_provider.h"

struct util_sync_provider_ntagx {
   struct util_sync_provider base;
   void *hdev;
   bool owns_hdev;
};

static struct util_sync_provider *
ntagx_sync_provider_create(void *hdev, bool owns_hdev);

static void *
ntagx_hdev(struct util_sync_provider *p)
{
   struct util_sync_provider_ntagx *d = (struct util_sync_provider_ntagx *)p;
   return d->hdev;
}

static int
ntagx_syncobj_create(struct util_sync_provider *p, uint32_t flags,
                      uint32_t *handle)
{
   struct ntasi_kmd_uapi_syncobj_create req = {.flags = flags};

   int ret =
      ntagx_ioctl(ntagx_hdev(p), NTASI_KMD_UAPI_IOCTL_SYNCOBJ_CREATE, &req);
   if (ret)
      return ret;

   *handle = req.handle;
   return 0;
}

static int
ntagx_syncobj_destroy(struct util_sync_provider *p, uint32_t handle)
{
   struct ntasi_kmd_uapi_syncobj_destroy req = {.handle = handle};

   return ntagx_ioctl(ntagx_hdev(p), NTASI_KMD_UAPI_IOCTL_SYNCOBJ_DESTROY,
                       &req);
}

/*
 * fd-based ops: hard stubs, see file header comment. Signatures match
 * u_sync_provider.h exactly so they slot into the base struct below.
 */
static int
ntagx_syncobj_handle_to_fd(struct util_sync_provider *p, uint32_t handle,
                            int *out_obj_fd)
{
   (void)p;
   (void)handle;
   (void)out_obj_fd;
   return -EINVAL;
}

static int
ntagx_syncobj_fd_to_handle(struct util_sync_provider *p, int obj_fd,
                            uint32_t *handle)
{
   (void)p;
   (void)obj_fd;
   (void)handle;
   return -EINVAL;
}

static int
ntagx_syncobj_import_sync_file(struct util_sync_provider *p, uint32_t handle,
                                int sync_file_fd)
{
   (void)p;
   (void)handle;
   (void)sync_file_fd;
   return -EINVAL;
}

static int
ntagx_syncobj_export_sync_file(struct util_sync_provider *p, uint32_t handle,
                                int *out_sync_file_fd)
{
   (void)p;
   (void)handle;
   (void)out_sync_file_fd;
   return -EINVAL;
}

static int
ntagx_syncobj_wait(struct util_sync_provider *p, uint32_t *handles,
                    unsigned num_handles, int64_t timeout_nsec,
                    unsigned flags, uint32_t *first_signaled)
{
   struct ntasi_kmd_uapi_syncobj_wait req = {
      .handles = (uint64_t)(uintptr_t)handles,
      .timeout_nsec = timeout_nsec,
      .count_handles = num_handles,
      .flags = flags,
   };

   int ret =
      ntagx_ioctl(ntagx_hdev(p), NTASI_KMD_UAPI_IOCTL_SYNCOBJ_WAIT, &req);

   /*
    * What was actually asked for, and what came back. A userspace-only fact,
    * so it is still true when the KMD reports nothing -- which matters
    * because "the wait was satisfied inline" and "no wait ioctl was ever
    * issued" are indistinguishable from the kernel's side.
    */
   if (ntagx_fence_log_enabled()) {
      fprintf(stderr,
              "NTAGX_FENCE: SYNCOBJ_WAIT n=%u flags=0x%x timeout=%lld "
              "handle[0]=%u -> ret=%d errno=%d\n",
              num_handles, flags, (long long)timeout_nsec,
              num_handles ? handles[0] : 0u, ret, ret ? errno : 0);
   }

   /*
    * THE AFTER HALF OF THE BRACKET, AND IT MUST BE HERE.
    *
    * This is the first instruction in userspace that runs once the fence has
    * actually resolved. Sampling anywhere earlier -- in win_submit, or before
    * this ioctl -- would publish a record of the completion path BEFORE the
    * completion it is supposed to describe, which reads as "nothing ran"
    * regardless of what the hardware did. The KMD increments its
    * sync_wait_completions counter before it completes this request precisely
    * so this sample cannot outrun its own wait.
    *
    * Sampled on the failure path too: a wait that timed out or errored is
    * exactly when "what did the completion path do while we waited?" matters
    * most.
    */
   ntagx_completion_counters_report(ntagx_hdev(p), "after syncobj_wait");

   if (ret)
      return ret;

   if (first_signaled)
      *first_signaled = req.first_signaled;

   return 0;
}

static int
ntagx_syncobj_reset(struct util_sync_provider *p, const uint32_t *handles,
                     uint32_t handle_count)
{
   struct ntasi_kmd_uapi_syncobj_array req = {
      .handles = (uint64_t)(uintptr_t)handles,
      .count_handles = handle_count,
   };

   return ntagx_ioctl(ntagx_hdev(p), NTASI_KMD_UAPI_IOCTL_SYNCOBJ_RESET,
                       &req);
}

static int
ntagx_syncobj_signal(struct util_sync_provider *p, const uint32_t *handles,
                      uint32_t handle_count)
{
   struct ntasi_kmd_uapi_syncobj_array req = {
      .handles = (uint64_t)(uintptr_t)handles,
      .count_handles = handle_count,
   };

   return ntagx_ioctl(ntagx_hdev(p), NTASI_KMD_UAPI_IOCTL_SYNCOBJ_SIGNAL,
                       &req);
}

static int
ntagx_syncobj_timeline_signal(struct util_sync_provider *p,
                               const uint32_t *handles, uint64_t *points,
                               uint32_t handle_count)
{
   struct ntasi_kmd_uapi_syncobj_timeline_array req = {
      .handles = (uint64_t)(uintptr_t)handles,
      .points = (uint64_t)(uintptr_t)points,
      .count_handles = handle_count,
      /* .flags = 0 (MBZ for TIMELINE_SIGNAL, kmd_uapi.h). */
   };

   return ntagx_ioctl(ntagx_hdev(p),
                       NTASI_KMD_UAPI_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &req);
}

static int
ntagx_syncobj_timeline_wait(struct util_sync_provider *p, uint32_t *handles,
                             uint64_t *points, unsigned num_handles,
                             int64_t timeout_nsec, unsigned flags,
                             uint32_t *first_signaled)
{
   struct ntasi_kmd_uapi_syncobj_timeline_wait req = {
      .handles = (uint64_t)(uintptr_t)handles,
      .points = (uint64_t)(uintptr_t)points,
      .timeout_nsec = timeout_nsec,
      .count_handles = num_handles,
      .flags = flags,
   };

   int ret = ntagx_ioctl(ntagx_hdev(p),
                          NTASI_KMD_UAPI_IOCTL_SYNCOBJ_TIMELINE_WAIT, &req);

   if (ntagx_fence_log_enabled()) {
      fprintf(stderr,
              "NTAGX_FENCE: TIMELINE_WAIT n=%u flags=0x%x timeout=%lld "
              "handle[0]=%u point[0]=%llu -> ret=%d errno=%d\n",
              num_handles, flags, (long long)timeout_nsec,
              num_handles ? handles[0] : 0u,
              num_handles ? (unsigned long long)points[0] : 0ull, ret,
              ret ? errno : 0);
   }

   /* Same contract as ntagx_syncobj_wait above: sampled after the wait has
    * resolved, never before. */
   ntagx_completion_counters_report(ntagx_hdev(p),
                                    "after syncobj_timeline_wait");

   if (ret)
      return ret;

   if (first_signaled)
      *first_signaled = req.first_signaled;

   return 0;
}

static int
ntagx_syncobj_query(struct util_sync_provider *p, uint32_t *handles,
                     uint64_t *points, uint32_t handle_count, uint32_t flags)
{
   struct ntasi_kmd_uapi_syncobj_timeline_array req = {
      .handles = (uint64_t)(uintptr_t)handles,
      .points = (uint64_t)(uintptr_t)points,
      .count_handles = handle_count,
      .flags = flags,
   };

   return ntagx_ioctl(ntagx_hdev(p), NTASI_KMD_UAPI_IOCTL_SYNCOBJ_QUERY,
                       &req);
}

static int
ntagx_syncobj_transfer(struct util_sync_provider *p, uint32_t dst_handle,
                        uint64_t dst_point, uint32_t src_handle,
                        uint64_t src_point, uint32_t flags)
{
   /* Field order is src-then-dst, NOT dst-then-src -- matches the real
    * struct drm_syncobj_transfer exactly (drm.h:915-922), NOT this
    * function's own dst-first argument order. See kmd_uapi.h's
    * ntasi_kmd_uapi_syncobj_transfer comment. */
   struct ntasi_kmd_uapi_syncobj_transfer req = {
      .src_handle = src_handle,
      .dst_handle = dst_handle,
      .src_point = src_point,
      .dst_point = dst_point,
      .flags = flags,
   };

   return ntagx_ioctl(ntagx_hdev(p), NTASI_KMD_UAPI_IOCTL_SYNCOBJ_TRANSFER,
                       &req);
}

static void
ntagx_sync_provider_finalize(struct util_sync_provider *p)
{
   struct util_sync_provider_ntagx *d = (struct util_sync_provider_ntagx *)p;

   if (d->owns_hdev)
      CloseHandle((HANDLE)d->hdev);

   free(p);
}

static struct util_sync_provider *
ntagx_sync_provider_clone(struct util_sync_provider *p)
{
   struct util_sync_provider_ntagx *d = (struct util_sync_provider_ntagx *)p;
   HANDLE new_hdev = NULL;

   /* drm template re-creates from fd (u_sync_provider.c
    * drm_sync_provider_clone); the NT analogue of "a second reference to
    * the same underlying device" is DuplicateHandle(), not a re-open. */
   if (!DuplicateHandle(GetCurrentProcess(), (HANDLE)d->hdev,
                         GetCurrentProcess(), &new_hdev, 0, FALSE,
                         DUPLICATE_SAME_ACCESS))
      return NULL;

   struct util_sync_provider *clone =
      ntagx_sync_provider_create(new_hdev, true);
   if (!clone)
      CloseHandle(new_hdev);

   return clone;
}

static struct util_sync_provider *
ntagx_sync_provider_create(void *hdev, bool owns_hdev)
{
   struct util_sync_provider_ntagx *d =
      CALLOC_STRUCT(util_sync_provider_ntagx);
   if (!d)
      return NULL;

   d->hdev = hdev;
   d->owns_hdev = owns_hdev;
   d->base = (struct util_sync_provider){
      .create = ntagx_syncobj_create,
      .destroy = ntagx_syncobj_destroy,
      .handle_to_fd = ntagx_syncobj_handle_to_fd,
      .fd_to_handle = ntagx_syncobj_fd_to_handle,
      .import_sync_file = ntagx_syncobj_import_sync_file,
      .export_sync_file = ntagx_syncobj_export_sync_file,
      .wait = ntagx_syncobj_wait,
      .reset = ntagx_syncobj_reset,
      .signal = ntagx_syncobj_signal,
      /*
       * Unlike util_sync_provider_drm (which installs these two only after
       * an explicit DRM_CAP_SYNCOBJ_TIMELINE capability probe), the ntagx
       * KMD contract requires timeline support unconditionally -- these
       * must be non-NULL or VK_SYNC_FEATURE_TIMELINE never advertises
       * (honeykrisp-h1-execution.md SS3.1 table, vk_drm_syncobj.c:696-699).
       */
      .timeline_signal = ntagx_syncobj_timeline_signal,
      .timeline_wait = ntagx_syncobj_timeline_wait,
      .query = ntagx_syncobj_query,
      .transfer = ntagx_syncobj_transfer,
      .finalize = ntagx_sync_provider_finalize,
      .clone = ntagx_sync_provider_clone,
   };

   return &d->base;
}

struct util_sync_provider *
util_sync_provider_ntagx(void *hdev)
{
   return ntagx_sync_provider_create(hdev, false);
}

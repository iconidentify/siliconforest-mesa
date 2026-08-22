/*
 * Copyright 2026 the Asahi Linux contributors
 * SPDX-License-Identifier: MIT
 */

/*
 * honeykrisp-h1-execution.md SS1: the Windows winsys backend for
 * agx_device_ops_t. Every op below clones the shape of its Linux
 * counterpart in agx_device.c (bo_alloc <-> agx_bo_alloc, bo_bind <->
 * agx_drm_bo_bind, get_params <-> agx_get_params, submit <-> agx_submit,
 * bo_bind_object <-> agx_bo_bind_object, bo_unbind_object <->
 * agx_bo_unbind_object, simple_ioctl <-> asahi_simple_ioctl) with exactly
 * one substitution throughout: `drmIoctl(dev->fd, CMD, &req)` becomes
 * `ntagx_ioctl(dev->hdev, CMD, &req)`. This is safe because `struct
 * drm_asahi_*` (the real, MIT, unmodified uapi structs, reached here via the
 * win-include overlay's shadow of drm-uapi/asahi_drm.h, SS5.3) and `struct
 * ntasi_kmd_uapi_*` (kmd-uapi-core/kmd_uapi.h) are pinned byte-identical
 * (cross-checked below), and DRM_IOCTL_ASAHI_* literals recognized by
 * ntasi_kmd_uapi_cmd_to_ctl() are numerically identical to the
 * NTASI_KMD_UAPI_IOCTL_* constants built from the same (dir, id, sizeof)
 * triple (kmd_uapi.h PART 1 / SS5.4) -- so every existing call site's wire
 * struct just works unmodified.
 *
 * Three ops have no Linux/virtio body to clone (new, OUR DESIGN,
 * Windows-only ioctls -- honeykrisp-h1-execution.md SS1, SS5.4):
 *   - bo_mmap: Linux does GEM_MMAP_OFFSET + a separate mmap(2); Windows maps
 *     the BO's MDL into the caller in one round trip
 *     (NTASI_KMD_UAPI_IOCTL_GEM_MMAP, kmd_uapi.h id 21).
 *   - bo_munmap / bo_free: Linux's munmap(2) / DRM_IOCTL_GEM_CLOSE are
 *     DRM-core/OS primitives with no NT equivalent reachable via
 *     DeviceIoControl; Windows uses NTASI_KMD_UAPI_IOCTL_GEM_MUNMAP /
 *     GEM_CLOSE (ids 22 / 20) instead.
 *
 * KNOWN INHERITED GAP (not introduced here, not yet fixed): win_bo_alloc()
 * below clones agx_bo_alloc()'s pthread_mutex_lock/unlock(&dev->bo_map_lock)
 * calls verbatim, per "clone the body". `bo_map_lock`'s pthread_mutex_t ->
 * simple_mtx_t swap (honeykrisp-h1-execution.md SS2.1 "pthread inventory")
 * is scoped to a later header-hygiene step, not this file; until that
 * lands, this file has the same pthread portability gap agx_device.h/.c
 * already has under a real mingw target.
 *
 * NOT build-verified: no llvm-mingw/MSVC toolchain is installed on this
 * host (checked: `aarch64-w64-mingw32-clang`/`-gcc` absent). See the H1
 * patch series verification notes for exactly what WAS checked (syntax-only
 * against the win-include overlay where reachable; narrowed, explicitly-
 * labeled shape checks elsewhere; a full TU compile needs both a real mingw
 * sysroot for <windows.h> and a meson configure for agx_device.h's other
 * generated-header dependencies, e.g. agx_pack.h).
 */

#include <inttypes.h>
#include "agx_device_win.h"

#include <windows.h>

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "kmd_uapi.h"
#include "kmd_diag.h"
#include "util/macros.h"
#include "util/simple_mtx.h"

/*
 * Cross-check the "byte-identical mirror" claim this file relies on
 * (kmd_uapi.h's own header comment, VERIFIED against the real asahi_drm.h
 * for every field used by this port -- see the H1 patch series notes) for
 * every struct this file passes through ntagx_ioctl() as a `struct
 * drm_asahi_*` but which the DeviceIoControl transport underneath sizes via
 * `struct ntasi_kmd_uapi_*` (kmd_uapi.c's cmd table). If a future
 * asahi_drm.h changes any of these layouts without re-pinning kmd_uapi.h,
 * this is where it fails to compile instead of silently mis-marshalling.
 */
_Static_assert(sizeof(struct drm_asahi_gem_create) ==
                  sizeof(struct ntasi_kmd_uapi_gem_create),
               "drm_asahi_gem_create / ntasi_kmd_uapi_gem_create");
_Static_assert(sizeof(struct drm_asahi_vm_bind) ==
                  sizeof(struct ntasi_kmd_uapi_vm_bind),
               "drm_asahi_vm_bind / ntasi_kmd_uapi_vm_bind");
_Static_assert(sizeof(struct drm_asahi_gem_bind_object) ==
                  sizeof(struct ntasi_kmd_uapi_gem_bind_object),
               "drm_asahi_gem_bind_object / ntasi_kmd_uapi_gem_bind_object");
_Static_assert(sizeof(struct drm_asahi_get_params) ==
                  sizeof(struct ntasi_kmd_uapi_get_params),
               "drm_asahi_get_params / ntasi_kmd_uapi_get_params");
_Static_assert(sizeof(struct drm_asahi_submit) ==
                  sizeof(struct ntasi_kmd_uapi_submit),
               "drm_asahi_submit / ntasi_kmd_uapi_submit");
_Static_assert(sizeof(struct drm_asahi_vm_create) ==
                  sizeof(struct ntasi_kmd_uapi_vm_create),
               "drm_asahi_vm_create / ntasi_kmd_uapi_vm_create");
_Static_assert(sizeof(struct drm_asahi_queue_create) ==
                  sizeof(struct ntasi_kmd_uapi_queue_create),
               "drm_asahi_queue_create / ntasi_kmd_uapi_queue_create");
_Static_assert(sizeof(struct drm_asahi_queue_destroy) ==
                  sizeof(struct ntasi_kmd_uapi_queue_destroy),
               "drm_asahi_queue_destroy / ntasi_kmd_uapi_queue_destroy");
_Static_assert(sizeof(struct drm_asahi_get_time) ==
                  sizeof(struct ntasi_kmd_uapi_get_time),
               "drm_asahi_get_time / ntasi_kmd_uapi_get_time");

/*
 * honeykrisp-h1-execution.md SS5.4: pinned Linux ioctl number -> NT
 * DeviceIoControl CTL_CODE + {in,out}_size, then the actual DeviceIoControl
 * call. Declared non-static in agx_device_win.h so agx_sync_win.c's
 * util_sync_provider_ntagx can reuse it (plan SS3.1: "keep with the winsys
 * so it can use ntagx_ioctl").
 */
int
ntagx_ioctl(void *hdev, uint32_t drm_cmd, void *req)
{
   uint32_t ctl, in_size, out_size;

   if (ntasi_kmd_uapi_cmd_to_ctl(drm_cmd, &ctl, &in_size, &out_size)) {
      errno = EINVAL;
      return -1;
   }

   DWORD bytes_returned = 0;
   BOOL ok = DeviceIoControl((HANDLE)hdev, (DWORD)ctl, req, in_size, req,
                              out_size, &bytes_returned, NULL);
   if (!ok) {
      DWORD error = GetLastError();

      switch (error) {
      case ERROR_SEM_TIMEOUT:
      case WAIT_TIMEOUT:
      case ERROR_TIMEOUT:
         errno = ETIME;
         break;
      case ERROR_INVALID_HANDLE:
         errno = EBADF;
         break;
      case ERROR_NOT_ENOUGH_MEMORY:
      case ERROR_OUTOFMEMORY:
         errno = ENOMEM;
         break;
      case ERROR_INVALID_PARAMETER:
         errno = EINVAL;
         break;
      case ERROR_ACCESS_DENIED:
         errno = EACCES;
         break;
      case ERROR_BUSY:
      case ERROR_DEVICE_IN_USE:
         errno = EBUSY;
         break;
      case ERROR_OPERATION_ABORTED:
         errno = ECANCELED;
         break;
      default:
         errno = EIO;
         break;
      }
      return -1;
   }

   return 0;
}

static struct agx_bo *
win_bo_alloc(struct agx_device *dev, size_t size, size_t align,
             enum agx_bo_flags flags)
{
   struct agx_bo *bo;
   unsigned handle = 0;

   /* executable implies low va */
   assert(!(flags & AGX_BO_EXEC) || (flags & AGX_BO_LOW_VA));

   struct drm_asahi_gem_create gem_create = {.size = size};

   if (flags & AGX_BO_WRITEBACK)
      gem_create.flags |= DRM_ASAHI_GEM_WRITEBACK;

   if (!(flags & (AGX_BO_SHARED | AGX_BO_SHAREABLE))) {
      gem_create.flags |= DRM_ASAHI_GEM_VM_PRIVATE;
      gem_create.vm_id = dev->vm_id;
   }

   int ret = ntagx_ioctl(dev->hdev, DRM_IOCTL_ASAHI_GEM_CREATE, &gem_create);
   if (ret) {
      fprintf(stderr, "DRM_IOCTL_ASAHI_GEM_CREATE failed\n");
      return NULL;
   }

   handle = gem_create.handle;

   simple_mtx_lock(&dev->bo_map_lock);
   bo = agx_lookup_bo(dev, handle);
   dev->max_handle = MAX2(dev->max_handle, handle);
   simple_mtx_unlock(&dev->bo_map_lock);

   /* Fresh handle */
   assert(!memcmp(bo, &((struct agx_bo){}), sizeof(*bo)));

   bo->dev = dev;
   bo->size = gem_create.size;
   bo->align = align;
   bo->flags = flags;
   bo->handle = bo->uapi_handle = handle;
   bo->prime_fd = -1;

   enum agx_va_flags va_flags = flags & AGX_BO_LOW_VA ? AGX_VA_USC : 0;
   bo->va = agx_va_alloc(dev, size, bo->align, va_flags, 0);
   if (!bo->va) {
      fprintf(stderr, "Failed to allocate BO VMA\n");
      agx_bo_free(dev, bo);
      return NULL;
   }

   uint32_t bind = DRM_ASAHI_BIND_READ;
   if (!(flags & AGX_BO_READONLY)) {
      bind |= DRM_ASAHI_BIND_WRITE;
   }

   ret = agx_bo_bind(dev, bo, bo->va->addr, bo->size, 0, bind);
   if (ret) {
      agx_bo_free(dev, bo);
      return NULL;
   }

   return bo;
}

/*
 * Ask the KMD what a just-bound GPU VA actually translates to, and whether
 * that is the BO's own physical memory.
 *
 * WHY THE KERNEL COMPUTES THE VERDICT AND NOT US: the previous round of this
 * investigation was derailed by a userspace check that compared two values
 * sampled at different times and declared them aliased. Every comparison that
 * matters here -- firmware's page-table root versus the driver's, and the
 * leaf PTE's output page versus the BO's real page -- is made inside the KMD
 * under one lock, against live state, and arrives as a single verdict bit.
 * Userspace only prints it.
 *
 * This is the discriminator for "the GPU ran for 12-16 ms and wrote nothing":
 * a VALID leaf pointing at the WRONG physical page is a write to real,
 * mapped, non-faulting memory that the CPU simply cannot see -- which looks
 * identical to a dead GPU from every other vantage point.
 */
static void
ntagx_uat_verify(struct agx_device *dev, const struct drm_asahi_gem_bind_op *op)
{
   struct agxkmd_diag_uat_query q = {
      .vm_id = dev->vm_id,
      .bo_handle = op->handle,
      .gpu_va = op->addr,
      .bo_offset = op->offset,
   };
   DWORD returned = 0;

   if (!DeviceIoControl((HANDLE)dev->hdev, (DWORD)AGXKMD_DIAG_UAT_QUERY_CTL,
                        &q, sizeof(q), &q, sizeof(q), &returned, NULL) ||
       returned != sizeof(q)) {
      /* An older KMD without the diagnostic returns ERROR_INVALID_FUNCTION /
       * ERROR_NOT_SUPPORTED. Say so once rather than implying a bind fault. */
      static bool complained;
      if (!complained) {
         complained = true;
         fprintf(stderr,
                 "NTAGX_UAT: diagnostic unavailable (win32 error %lu) -- KMD "
                 "predates AGXKMD_DIAG_UAT_QUERY_CTL; bind itself succeeded\n",
                 (unsigned long)GetLastError());
      }
      return;
   }

   bool pa_ok = (q.flags & AGXKMD_DIAG_F_BO_PA_MATCH) != 0;
   bool root_ok = (q.flags & AGXKMD_DIAG_F_TTBAT_ROOT_MATCH) != 0;
   bool leaf_ok = (q.flags & AGXKMD_DIAG_F_LEAF_VALID) != 0;

   if (ntagx_bo_log_enabled()) {
      fprintf(stderr,
              "NTAGX_UAT: vm=%u handle=%u gpu_va=0x%" PRIx64 " pte=0x%" PRIx64
              " pa=0x%" PRIx64 " bo_pa=0x%" PRIx64 " ctx_root=0x%" PRIx64
              " ttbat_root=0x%" PRIx64 " asid=%u levels=%u perm=0x%x "
              "flags=0x%x walk=%d %s\n",
              (unsigned)q.vm_id, (unsigned)q.bo_handle, q.gpu_va, q.leaf_pte,
              q.translated_pa, q.bo_expected_pa, q.context_root_pa,
              q.ttbat_root_pa, (unsigned)q.ttbat_asid,
              (unsigned)q.walk_level_count, (unsigned)q.gpu_perm,
              (unsigned)q.flags, (int)q.walk_status,
              (pa_ok && root_ok) ? "OK" : "PROBLEM");
   }

   /* Unconditional, because a wrong translation is never acceptable and is
    * invisible everywhere else. Once per process: the failure is systematic. */
   static bool reported;
   if (!(pa_ok && root_ok && leaf_ok) && !reported) {
      reported = true;
      fprintf(stderr,
              "NTAGX FATAL: GPU VA 0x%" PRIx64 " (vm %u, handle %u) does not "
              "resolve to that BO's memory.\n",
              q.gpu_va, (unsigned)q.vm_id, (unsigned)q.bo_handle);
      if (!leaf_ok)
         fprintf(stderr,
                 "  no valid leaf PTE (walk reached %u level(s), status %d): "
                 "the GPU has no mapping here at all.\n",
                 (unsigned)q.walk_level_count, (int)q.walk_status);
      else if (!pa_ok)
         fprintf(stderr,
                 "  leaf is VALID but points at 0x%" PRIx64 " while the BO's "
                 "own page is 0x%" PRIx64 ". The GPU is writing real, mapped, "
                 "non-faulting memory that is NOT this buffer -- work runs, "
                 "time passes, and the CPU sees nothing.\n",
                 q.translated_pa, q.bo_expected_pa);
      if (!root_ok)
         fprintf(stderr,
                 "  firmware TTBAT root 0x%" PRIx64 " != driver page-table "
                 "root 0x%" PRIx64 ": the firmware is walking tables the "
                 "driver does not maintain.\n",
                 q.ttbat_root_pa, q.context_root_pa);
      if (!(q.flags & AGXKMD_DIAG_F_SOFT_FAULTS))
         fprintf(stderr,
                 "  soft faults are OFF, so an absent mapping would fault "
                 "rather than be dropped silently.\n");
   }
}

static int
win_bo_bind(struct agx_device *dev, struct drm_asahi_gem_bind_op *ops,
            uint32_t count)
{
   struct drm_asahi_vm_bind vm_bind = {
      .num_binds = count,
      .vm_id = dev->vm_id,
      .userptr = (uintptr_t)ops,
      .stride = sizeof(*ops),
   };

   int ret = ntagx_ioctl(dev->hdev, DRM_IOCTL_ASAHI_VM_BIND, &vm_bind);
   if (ret) {
      fprintf(stderr, "DRM_IOCTL_ASAHI_VM_BIND failed\n");
      return ret;
   }

   for (uint32_t i = 0; i < count; i++) {
      if (!(ops[i].flags & DRM_ASAHI_BIND_UNBIND))
         ntagx_uat_verify(dev, &ops[i]);
   }

   return ret;
}

/*
 * OUR DESIGN (honeykrisp-h1-execution.md SS1 "bo_mmap" row): no Linux body
 * to clone -- GEM_MMAP_OFFSET + mmap(2) doesn't exist on Windows (os_mmap()
 * is `#error Unsupported OS` there, VERIFIED src/util/os_mman.h). The KMD
 * maps the BO's MDL into the caller in one round trip, honoring a placed
 * (fixed_addr != NULL) request the way agx_bo_map_placed()
 * (agx_device.h) requires.
 */
static void
win_bo_mmap(struct agx_device *dev, struct agx_bo *bo, void *fixed_addr)
{
   assert(bo->_map == NULL && "not double mapped");

   struct ntasi_kmd_uapi_gem_mmap gem_mmap = {
      .handle = bo->uapi_handle,
      .fixed_va = (uint64_t)(uintptr_t)fixed_addr,
   };

   int ret = ntagx_ioctl(dev->hdev, NTASI_KMD_UAPI_IOCTL_GEM_MMAP, &gem_mmap);
   if (ret) {
      int error_number = errno;
      DWORD win32_error = GetLastError();

      bo->_map = NULL;
      fprintf(stderr, "NTASI_KMD_UAPI_IOCTL_GEM_MMAP failed: handle=%u "
                      "errno=%d win32=%lu\n", bo->uapi_handle,
              error_number, (unsigned long)win32_error);
      return;
   }

   bo->_map = (void *)(uintptr_t)gem_mmap.user_va;

   if (ntagx_bo_log_enabled()) {
      fprintf(stderr,
              "NTAGX_MMAP: handle=%u size=%zu -> user_va=%p\n",
              (unsigned)bo->uapi_handle, (size_t)bo->size, bo->_map);
   }

   /*
    * NO ALIAS DETECTOR HERE, DELIBERATELY -- and this is a correction, not an
    * omission.
    *
    * An earlier revision of this file compared each GEM_MMAP result against
    * the previous call's and printed a fatal-looking "kernel-side GEM_MMAP
    * defect" when two handles produced one address. That check was WRONG and
    * its verdict has been retired: GEM_MUNMAP releases the user VA, and
    * Windows hands the freed 64 KiB slot straight back to the next mapping,
    * so a caller that maps and unmaps serially -- which vkMapMemory /
    * vkUnmapMemory callers routinely do -- is SUPPOSED to see the same
    * address repeatedly. The check fired on healthy runs, its text leaked
    * into unrelated captures, and it cost a debugging round on its own.
    *
    * Aliasing is only observable while the mappings are simultaneously live,
    * which is a property of the caller's map/unmap discipline and not
    * something this function can see one call at a time. The probe checks it
    * where it is actually checkable (gpu/vktests/vkcompute.c maps all four
    * buffers at once before comparing). Do not reintroduce a per-call
    * version here.
    */
}

static ssize_t
win_get_params(struct agx_device *dev, void *buf, size_t size)
{
   struct drm_asahi_get_params get_param = {
      .param_group = 0,
      .pointer = (uint64_t)(uintptr_t)buf,
      .size = size,
   };

   memset(buf, 0, size);

   int ret = ntagx_ioctl(dev->hdev, DRM_IOCTL_ASAHI_GET_PARAMS, &get_param);
   if (ret) {
      fprintf(stderr, "DRM_IOCTL_ASAHI_GET_PARAMS failed\n");
      return -EINVAL;
   }

   return get_param.size;
}

/*
 * Ask the KMD what firmware work item it actually materialized from the last
 * submission, and whether the addresses that dispatch depends on resolve.
 *
 * WHY THIS EXISTS. The submit path was believed correct on the strength of
 * "cmdbuf_size=72, ioctl returned 0", which proves only that 72 bytes were
 * handed over and the ioctl succeeded. Meanwhile the UAT probe proved every
 * BO's translation byte-identical to the reference driver's -- so the
 * remaining suspicion is not where the buffers live but what the GPU is told
 * to do with them.
 *
 * The specific shape being hunted: a USC execution base that does not resolve
 * in the queue's VM. That is a shader running from nowhere -- it executes,
 * takes real and variable time, stores nothing observable, and never faults,
 * which is the measured signature exactly. Note that vkCmdFillBuffer is NOT
 * shader-free: hk_CmdFillBuffer dispatches libagx_fill_uint4, so every rung
 * of the vkcompute ladder except `empty` runs a compute shader.
 *
 * As with the bind-time check, every verdict is computed inside the KMD
 * against live state and arrives as a flag; userspace only prints it.
 */
#if AGX_GPU_DIAG >= 2
static uint32_t ntagx_last_work_generation;
static uint64_t ntagx_shader_probe_va;

static void
ntagx_submit_verify(struct agx_device *dev)
{
   struct agxkmd_diag_submit_query q = {0};
   DWORD returned = 0;
   int waited_ms = 0;

   if (!ntagx_bo_log_enabled())
      return;

   /*
    * SUBMISSION IS ASYNCHRONOUS. The KMD completes the submit ioctl as soon
    * as the work is enqueued to its passive worker ("Match DRM submit
    * semantics: return success now"), and materializes the firmware work item
    * on that worker afterwards. Querying the instant the ioctl returns
    * therefore races the capture -- which is exactly what produced a bare
    * "no submission captured by the KMD" on a run whose fence signalled after
    * 14 ms.
    *
    * So wait for the generation to actually advance past what we last saw,
    * bounded, and report how long it took. A timeout here is a real finding
    * (the work item is never built); a short wait is just the async path
    * doing its job. Either way the counters below say which.
    */
   for (;;) {
      memset(&q, 0, sizeof(q));
      q.prior_generation = ntagx_last_work_generation;
      /*
       * Ask for a dump of the shader binary too, at
       * usc_exec_base + <the word the CDM stream carries at +0x004>.
       *
       * THIS IS AN OBSERVATION, NOT A DECODE. Measured on J414s: that word
       * read 0xfff88000, the low 32 bits of 0x11fff88000, which is exactly
       * the base of the USC carveout range the KMD independently reports as
       * occupied by bindings. Two unrelated sources agreeing on one address
       * is good evidence it is the shader's, but it is not the CDM opcode
       * tables and is not claimed to be. If the two disagree, the label
       * below says so rather than dumping a confidently-wrong address.
       */
      if (ntagx_shader_probe_va)
         q.probe_va = ntagx_shader_probe_va;

      if (!DeviceIoControl((HANDLE)dev->hdev,
                           (DWORD)AGXKMD_DIAG_SUBMIT_QUERY_CTL, &q, sizeof(q),
                           &q, sizeof(q), &returned, NULL) ||
          returned != sizeof(q))
         break;

      if ((q.flags & AGXKMD_DIAG_S_VALID) &&
          q.generation != ntagx_last_work_generation)
         break;
      if (waited_ms >= 2000)
         break;
      Sleep(2);
      waited_ms += 2;
   }

   if (returned != sizeof(q)) {
      static bool complained;
      if (!complained) {
         complained = true;
         fprintf(stderr,
                 "NTAGX_WORK: diagnostic unavailable (win32 error %lu) -- KMD "
                 "predates AGXKMD_DIAG_SUBMIT_QUERY_CTL\n",
                 (unsigned long)GetLastError());
      }
      return;
   }
   ntagx_last_work_generation = q.generation;

   if (!(q.flags & AGXKMD_DIAG_S_VALID) ||
       q.generation == q.prior_generation_echo) {
      /* Say what was looked for and what was found, so the negative names
       * itself instead of collapsing to "nothing captured". */
      fprintf(stderr,
              "NTAGX_WORK: no NEW work item after %d ms  (generation: wanted "
              ">%u, got %u; requested queue %u)  submit_ioctl_entries=%u "
              "materialize_entries=%u\n",
              waited_ms, (unsigned)q.prior_generation_echo,
              (unsigned)q.generation, (unsigned)q.requested_queue_id_echo,
              (unsigned)q.submit_ioctl_entries,
              (unsigned)q.materialize_entries);
      if (q.submit_ioctl_entries > 0 && q.materialize_entries == 0) {
         /* Deliberately NOT "never": these counters are cumulative and this
          * reader waited a bounded time. A materialize that lands after the
          * budget looks identical here to one that never happens, and saying
          * "NEVER" asserts more than the data supports -- measured, a first
          * submission after boot exceeded a 250 ms budget and then showed up
          * on the next run. Re-run and compare the counters before concluding
          * anything. */
         fprintf(stderr,
                 "NTAGX WARN: the submit ioctl was entered %u time(s) and no "
                 "work item was materialized WITHIN %d ms. That is a timeout, "
                 "not proof of absence -- re-run and see whether "
                 "materialize_entries advances.\n",
                 (unsigned)q.submit_ioctl_entries, waited_ms);
      } else if (q.materialize_entries > 0) {
         fprintf(stderr,
                 "NTAGX_WORK: materialize ran %u time(s), so the work item IS "
                 "built; this reader simply did not observe a new one.\n",
                 (unsigned)q.materialize_entries);
      }
      return;
   }

   /*
    * usc_exec_base is a REGISTER VALUE, not a pointer the GPU dereferences:
    * "USC addresses are 32-bit relative to this 64-bit base [...] userspace
    * fix[es] a 4GiB VA carveout for USC memory and pass[es] its base address
    * here" (asahi_drm.h, drm_asahi_queue_create::usc_exec_base). Mesa picks
    * the BOTTOM of that carveout and allocates shaders near the top, so the
    * base page is normally UNMAPPED and that is correct.
    *
    * An earlier revision of this check called that unmapped base fatal and
    * sent a hardware round chasing a non-bug. The real question is whether
    * the shaders the base addresses live inside the carveout at all, which
    * the KMD answers by counting this VM's bindings that fall within it.
    */
   bool usc_base_mapped = (q.flags & AGXKMD_DIAG_S_USC_MAPPED) != 0;
   bool usc_ok = (q.flags & AGXKMD_DIAG_S_USC_WINDOW_OK) != 0;
   bool cdm_ok = (q.flags & AGXKMD_DIAG_S_CDM_MAPPED) != 0;
   bool vm_ok = (q.flags & AGXKMD_DIAG_S_QUEUE_VM_MATCH) != 0;
   bool compute = (q.flags & AGXKMD_DIAG_S_HAS_COMPUTE) != 0;

   fprintf(stderr,
           "NTAGX_WORK: gen=%u queue=%u queue_vm=%u vm_slot=%u cmdbuf=%u "
           "hw_cmds=%u cmds=%u/%u/%u events=%u/%u/%u\n",
           (unsigned)q.generation, (unsigned)q.captured_queue_id,
           (unsigned)q.queue_vm_id, (unsigned)q.vm_slot,
           (unsigned)q.cmdbuf_size, (unsigned)q.hardware_command_count,
           (unsigned)q.command_count[0], (unsigned)q.command_count[1],
           (unsigned)q.command_count[2], (unsigned)q.event_count[0],
           (unsigned)q.event_count[1], (unsigned)q.event_count[2]);
   fprintf(stderr,
           "NTAGX_WORK: usc_exec_base=0x%" PRIx64 " -> pa=0x%" PRIx64
           " (%s base page, walk=%d)  cdm=0x%" PRIx64 "..0x%" PRIx64 " -> pa=0x%" PRIx64
           " (%s, walk=%d)  helper=0x%x/0x%" PRIx64 "/0x%x sampler_heap=0x%"
           PRIx64 " flags=0x%x %s\n",
           q.usc_exec_base, q.usc_exec_base_pa,
           usc_base_mapped ? "mapped" : "unmapped (normal)",
           (int)q.usc_walk_status, q.cdm_ctrl_stream_base,
           q.cdm_ctrl_stream_end, q.cdm_ctrl_stream_base_pa,
           cdm_ok ? "MAPPED" : "UNMAPPED", (int)q.cdm_walk_status,
           (unsigned)q.helper_binary, q.helper_data, (unsigned)q.helper_cfg,
           q.sampler_heap, (unsigned)q.flags,
           (usc_ok && cdm_ok && vm_ok) ? "OK" : "PROBLEM");
   fprintf(stderr,
           "NTAGX_WORK: usc carveout [0x%" PRIx64 "..0x%" PRIx64 ") holds %u "
           "binding(s), occupied 0x%" PRIx64 "..0x%" PRIx64 " (%s)\n",
           q.usc_exec_base, q.usc_exec_base + 0x100000000ull,
           (unsigned)q.usc_window_bindings, q.usc_window_lowest_va,
           q.usc_window_highest_end, usc_ok ? "OK" : "EMPTY");
   if (waited_ms)
      fprintf(stderr,
              "NTAGX_WORK: (work item appeared %d ms after the ioctl returned "
              "-- submission is asynchronous)  submit_ioctl_entries=%u "
              "materialize_entries=%u\n",
              waited_ms, (unsigned)q.submit_ioctl_entries,
              (unsigned)q.materialize_entries);

   /* helper.* is populated only when the command stream uses scratch, and
    * sampler_heap only when it uses a sampler heap (hk_queue.c:92-102). A
    * bare libagx fill uses neither, so all-zero here is CORRECT, not a clue. */
   if (compute && q.helper_binary == 0 && q.helper_data == 0 &&
       q.sampler_heap == 0) {
      fprintf(stderr,
              "NTAGX_WORK: helper and sampler_heap are null -- expected for a "
              "dispatch that uses neither scratch nor a sampler heap "
              "(hk_queue.c sets them only under those conditions). Not a "
              "defect.\n");
   }

   if (q.cdm_dump_bytes && q.cdm_dump_status == AGXKMD_DIAG_CDM_OK) {
      fprintf(stderr,
              "NTAGX_CDM: %u bytes at gpu_va 0x%" PRIx64 " (pa 0x%" PRIx64
              ", bo handle %u), read through the physical page the leaf PTE "
              "names:\n",
              (unsigned)q.cdm_dump_bytes, q.cdm_ctrl_stream_base,
              q.cdm_ctrl_stream_base_pa, (unsigned)q.cdm_bo_handle);
      for (uint32_t off = 0; off < q.cdm_dump_bytes / 4; off += 8) {
         fprintf(stderr, "NTAGX_CDM: +0x%03x ", off * 4);
         for (uint32_t k = 0; k < 8 && (off + k) < q.cdm_dump_bytes / 4; k++)
            fprintf(stderr, " %08x", q.cdm_dump[off + k]);
         fprintf(stderr, "\n");
      }
      bool all_zero = true;
      for (uint32_t k = 0; k < q.cdm_dump_bytes / 4; k++)
         if (q.cdm_dump[k] != 0)
            all_zero = false;
      if (all_zero)
         fprintf(stderr,
                 "NTAGX FATAL: the CDM control stream is ALL ZEROES where the "
                 "GPU reads it. The dispatch describes no work: it would "
                 "complete instantly, store nothing and fault nowhere. "
                 "Whatever userspace built did not reach these pages.\n");

      /* Derive the shader probe address for the NEXT submission's query --
       * observation, not decode; see the note at the query site. */
      if (q.cdm_dump_bytes >= 8 && q.usc_exec_base) {
         uint64_t derived = q.usc_exec_base + (uint64_t)q.cdm_dump[1];
         bool in_window = q.usc_window_bindings &&
                          derived >= q.usc_window_lowest_va &&
                          derived < q.usc_window_highest_end;
         ntagx_shader_probe_va = derived;
         fprintf(stderr,
                 "NTAGX_CDM: shader probe address = usc_exec_base + word[+0x004]"
                 " = 0x%" PRIx64 " (%s the occupied carveout 0x%" PRIx64
                 "..0x%" PRIx64 ") -- OBSERVED, not decoded\n",
                 derived,
                 in_window ? "inside" : "OUTSIDE",
                 q.usc_window_lowest_va, q.usc_window_highest_end);
         if (!in_window)
            fprintf(stderr,
                    "NTAGX_CDM: that address is outside every binding, so the "
                    "word at +0x004 is probably not a shader pointer -- do not "
                    "read the dump below as shader code.\n");
      }
   }

   if (q.probe_dump_bytes && q.probe_dump_status == AGXKMD_DIAG_CDM_OK) {
      uint32_t nonzero = 0;
      for (uint32_t k = 0; k < q.probe_dump_bytes / 4; k++)
         if (q.probe_dump[k])
            nonzero++;
      fprintf(stderr,
              "NTAGX_SHADER: %u bytes at gpu_va 0x%" PRIx64 " (pa 0x%" PRIx64
              "), %u/%u words non-zero:\n",
              (unsigned)q.probe_dump_bytes, q.probe_va, q.probe_pa,
              (unsigned)nonzero, (unsigned)(q.probe_dump_bytes / 4));
      for (uint32_t off = 0; off < q.probe_dump_bytes / 4; off += 8) {
         fprintf(stderr, "NTAGX_SHADER: +0x%03x ", off * 4);
         for (uint32_t k = 0; k < 8 && (off + k) < q.probe_dump_bytes / 4; k++)
            fprintf(stderr, " %08x", q.probe_dump[off + k]);
         fprintf(stderr, "\n");
      }
      if (nonzero == 0)
         fprintf(stderr,
                 "NTAGX FATAL: the shader binary is ALL ZEROES where the GPU "
                 "fetches it. The USC would execute nothing: real dispatch, "
                 "real time, no stores, no fault.\n");
   } else if (q.probe_va) {
      const char *why;
      switch (q.probe_dump_status) {
      case AGXKMD_DIAG_CDM_UNTRANSLATED:
         why = "no valid leaf PTE at that address -- the GPU cannot fetch "
               "shader code from there (a finding)";
         break;
      case AGXKMD_DIAG_CDM_MAP_FAILED:
         why = "the physical page could not be mapped -- the DIAGNOSTIC "
               "failed; nothing is implied about the contents";
         break;
      case AGXKMD_DIAG_CDM_RANGE_EMPTY:
         why = "no readable bytes in the page window";
         break;
      case AGXKMD_DIAG_CDM_ATTR_UNKNOWN:
         why = "REFUSED: no binding owns that address, so the page's existing "
               "cache attribute is unknown. Mapping it under the wrong "
               "cacheability bugchecks ARM64, so the read was not attempted "
               "-- this is a refusal, not a failure, and says nothing about "
               "the contents";
         break;
      default:
         why = "not attempted";
         break;
      }
      fprintf(stderr, "NTAGX_SHADER: NOT READ (status=%u) at 0x%" PRIx64
              ": %s\n", (unsigned)q.probe_dump_status, q.probe_va, why);
   } else if (compute) {
      /*
       * NAME THE NON-READ. An all-zero dump is the headline hypothesis for
       * this bug, so a failed read must never be presentable as a zeroed
       * stream. Each reason is distinct and none of them prints bytes.
       */
      const char *why;
      switch (q.cdm_dump_status) {
      case AGXKMD_DIAG_CDM_UNTRANSLATED:
         why = "the stream base has NO VALID LEAF PTE -- the GPU cannot "
               "translate it, so there are no bytes to read (this is a "
               "finding, not a missing measurement)";
         break;
      case AGXKMD_DIAG_CDM_MAP_FAILED:
         why = "the physical page could not be mapped for reading -- the "
               "diagnostic failed, and NOTHING is implied about the stream's "
               "contents";
         break;
      case AGXKMD_DIAG_CDM_RANGE_EMPTY:
         why = "the stream range is under 4 bytes -- the dispatch describes "
               "an empty control stream";
         break;
      case AGXKMD_DIAG_CDM_ATTR_UNKNOWN:
         why = "REFUSED: the page's existing cache attribute could not be "
               "established, and mapping it under the wrong cacheability "
               "bugchecks ARM64 rather than reading wrong. Nothing is implied "
               "about the contents";
         break;
      case AGXKMD_DIAG_CDM_NOT_ATTEMPTED:
         why = "not attempted (no compute command carried a stream base)";
         break;
      default:
         why = "unrecognised status";
         break;
      }
      fprintf(stderr, "NTAGX_CDM: NOT READ (status=%u): %s\n",
              (unsigned)q.cdm_dump_status, why);
   }

   static bool reported;
   if (!(usc_ok && cdm_ok && vm_ok) && !reported) {
      reported = true;
      fprintf(stderr, "NTAGX FATAL: the materialized firmware work item is "
                      "not executable as described.\n");
      if (!vm_ok)
         fprintf(stderr,
                 "  vm_slot %u != the queue's VM %u: every address in the "
                 "command stream is resolved in the wrong address space.\n",
                 (unsigned)q.vm_slot, (unsigned)q.queue_vm_id);
      if (compute && !usc_ok)
         fprintf(stderr,
                 "  the USC carveout [0x%" PRIx64 "..0x%" PRIx64 ") contains "
                 "NO bindings in vm %u. Every 32-bit USC address on this "
                 "queue resolves outside every mapping, so the shader "
                 "executes from nowhere: it runs, stores nothing, and never "
                 "faults. (The base page itself being unmapped is normal and "
                 "is NOT the defect.)\n",
                 q.usc_exec_base, q.usc_exec_base + 0x100000000ull,
                 (unsigned)q.vm_slot);
      if (compute && !cdm_ok)
         fprintf(stderr,
                 "  cdm_ctrl_stream_base 0x%" PRIx64 " has no valid mapping "
                 "in vm %u (walk=%d). The dispatch parameters are "
                 "unreadable.\n",
                 q.cdm_ctrl_stream_base, (unsigned)q.vm_slot,
                 (int)q.cdm_walk_status);
   }
}
#endif

/*
 * ---------------------------------------------------------------------------
 * The completion-path counter readout.
 *
 * WHAT IT IS FOR. The fence signals and nothing is written, and the fence's
 * latency is CONSTANT within one process while differing sharply across
 * processes (~0.02 ms, ~3.2 ms, ~10-15 ms; eight submissions in one process
 * spanned 3.19-3.30 ms). A GPU doing variable work does not look like that; a
 * mechanism whose phase is fixed once per process or per device state does --
 * a timer, a deferred drain, or completions noticed by something other than a
 * per-dispatch interrupt.
 *
 * Nothing observable from userspace could separate "the mailbox interrupt
 * delivers and the completion path runs" from "the interrupt never fires and
 * the fence reports whatever else noticed". The KMD's own
 * AgxkmdEvtDeviceD0EntryPostInterruptsEnabled cannot settle it either: it
 * records that IoConnectInterruptEx ACCEPTED the line, not that the line ever
 * asserts. These counters are incremented inside the driver at the top of each
 * path, before any validation, and are read here.
 * ---------------------------------------------------------------------------
 */
#if AGX_GPU_DIAG >= 2
bool
ntagx_fence_log_enabled(void)
{
   static int enabled = -1;

   if (enabled < 0) {
      const char *v = getenv("NTAGX_FENCE_LOG");
      enabled = (v && v[0] && v[0] != '0') ? 1 : 0;
   }
   return enabled == 1;
}

/* Previous sample, so every line can carry a delta as well as a total.
 * `have_previous` is separate from an all-zero sample on purpose: the first
 * report must not present its absolute values as if they were a delta. */
static simple_mtx_t ntagx_counter_mtx = SIMPLE_MTX_INITIALIZER;
static struct agxkmd_diag_submit_query ntagx_counter_previous;
static bool ntagx_counter_have_previous;

void
ntagx_completion_counters_report(void *hdev, const char *when)
{
   struct agxkmd_diag_submit_query q;
   struct agxkmd_diag_submit_query p;
   DWORD returned = 0;
   bool have_previous;

   if (!ntagx_fence_log_enabled())
      return;

   memset(&q, 0, sizeof(q));
   if (!DeviceIoControl((HANDLE)hdev, (DWORD)AGXKMD_DIAG_SUBMIT_QUERY_CTL, &q,
                        sizeof(q), &q, sizeof(q), &returned, NULL) ||
       returned != sizeof(q)) {
      static bool complained;
      if (!complained) {
         complained = true;
         fprintf(stderr,
                 "NTAGX_FENCE: counters UNAVAILABLE (win32 error %lu). The KMD "
                 "does not implement this query's current shape -- deploy the "
                 "matching AppleAgxGpu.sys. NOTHING BELOW IS A MEASUREMENT.\n",
                 (unsigned long)GetLastError());
      }
      return;
   }

   /*
    * THE "I WAS NOT TAKEN" GATE. Every counter's headline reading is a zero,
    * so a block the driver never wrote must never be reported as a block full
    * of zeroes. counters_version is written last by the KMD and is compared
    * for equality, not >=: an unrecognised version means the fields do not
    * mean what this reader thinks they mean.
    */
   if (q.counters_version != AGXKMD_DIAG_COUNTERS_VERSION) {
      static bool complained;
      if (!complained) {
         complained = true;
         fprintf(stderr,
                 "NTAGX_FENCE: counters NOT MEASURED (counters_version=%u, "
                 "this reader speaks %u). The driver never wrote the block; "
                 "its zeroes are absence, not evidence.\n",
                 (unsigned)q.counters_version,
                 (unsigned)AGXKMD_DIAG_COUNTERS_VERSION);
      }
      return;
   }

   simple_mtx_lock(&ntagx_counter_mtx);
   p = ntagx_counter_previous;
   have_previous = ntagx_counter_have_previous;
   ntagx_counter_previous = q;
   ntagx_counter_have_previous = true;

#define D(field) (have_previous ? (q.field - p.field) : 0u)

   fprintf(stderr,
           "NTAGX_FENCE[%s]: isr=%u(+%u) claims=%u(+%u) msgs=%u(+%u) "
           "wi_queued=%u(+%u) | drain=%u(+%u) polls=%u(+%u) events=%u(+%u)\n",
           when, (unsigned)q.isr_entries, D(isr_entries),
           (unsigned)q.isr_claims, D(isr_claims), (unsigned)q.isr_messages,
           D(isr_messages), (unsigned)q.isr_workitem_queues,
           D(isr_workitem_queues), (unsigned)q.interrupt_workitem_entries,
           D(interrupt_workitem_entries), (unsigned)q.event_drain_polls,
           D(event_drain_polls), (unsigned)q.event_drain_events,
           D(event_drain_events));
   fprintf(stderr,
           "NTAGX_FENCE[%s]: outcomes=%u(+%u) signals_hw=%u(+%u) "
           "signals_host=%u(+%u) | pends=%u(+%u) timer_arm=%u(+%u) "
           "timer_fire=%u(+%u) waits_done=%u(+%u) of_which_timer=%u(+%u) "
           "flags=0x%02x\n",
           when, (unsigned)q.outcome_dispatch_entries,
           D(outcome_dispatch_entries), (unsigned)q.outcome_signals_hardware,
           D(outcome_signals_hardware), (unsigned)q.outcome_signals_host,
           D(outcome_signals_host), (unsigned)q.sync_wait_pends,
           D(sync_wait_pends), (unsigned)q.sync_wait_timer_starts,
           D(sync_wait_timer_starts), (unsigned)q.sync_wait_timer_entries,
           D(sync_wait_timer_entries), (unsigned)q.sync_wait_completions,
           D(sync_wait_completions),
           (unsigned)q.sync_wait_completions_from_timer,
           D(sync_wait_completions_from_timer), (unsigned)q.counter_flags);

#undef D

   fprintf(stderr,
           "NTAGX_FENCE[%s]: wait_ioctl=%u/%u eval=%u -> sat=%u pend=%u "
           "enoent=%u timeout=%u err=%u | create=%u(sig=%u,sig_pre=%u) "
           "signal=%u reset=%u tl_signal=%u out_signal=%u\n"
           "NTAGX_FENCE[%s]: satisfied inline: pre_submit=%u post_submit=%u\n",
           when, (unsigned)q.syncobj_wait_ioctl_entries,
           (unsigned)q.syncobj_timeline_wait_ioctl_entries,
           (unsigned)q.wait_eval_entries, (unsigned)q.wait_eval_satisfied,
           (unsigned)q.wait_eval_pending, (unsigned)q.wait_eval_enoent,
           (unsigned)q.wait_eval_timeout, (unsigned)q.wait_eval_error,
           (unsigned)q.syncobj_create_calls,
           (unsigned)q.syncobj_create_signaled,
           (unsigned)q.syncobj_create_signaled_pre_submit,
           (unsigned)q.syncobj_signal_calls, (unsigned)q.syncobj_reset_calls,
           (unsigned)q.syncobj_timeline_signal_calls,
           (unsigned)q.output_signal_calls, when,
           (unsigned)q.wait_eval_satisfied_pre_submit,
           (unsigned)q.wait_eval_satisfied_post_submit);
   fprintf(stderr,
           "NTAGX_FENCE[%s]: out_syncs armed by submit: calls=%u binary=%u "
           "timeline=%u\n",
           when, (unsigned)q.output_arm_calls, (unsigned)q.output_arm_binary,
           (unsigned)q.output_arm_timeline);
   if (q.output_arm_binary == 0 && q.output_arm_calls > 0) {
      static bool no_binary_out;
      if (!no_binary_out) {
         no_binary_out = true;
         fprintf(stderr,
                 "NTAGX FATAL: %u submission(s) reached the driver and NOT ONE "
                 "carried a binary out-sync. The VkFence was never registered "
                 "as an output of anything: nothing un-signalled it, nothing "
                 "raised its submission_refs, and no completion could ever "
                 "have signalled it.\n",
                 (unsigned)q.output_arm_calls);
      }
   }

   /*
    * The one-shot record, printed once. This is the whole point of the
    * build: it names which clause of the evaluator satisfied the fence,
    * instead of leaving it to be inferred from a set of alternatives that
    * reading the source has already exhausted.
    */
   static bool inline_reported;
   if (q.inline_valid && !inline_reported) {
      inline_reported = true;
      fprintf(stderr,
              "NTAGX_FENCE: FIRST POST-SUBMIT INLINE-SATISFIED WAIT "
              "(submits=%u at capture) -- ioctl=%s handle=%u "
              "count=%u flags=0x%x first_index=%u timeout=%lld\n"
              "             object: in_use=%u kind=%u(0=untyped,1=binary,"
              "2=timeline) binary_signaled=%u submission_refs=%u\n"
              "             points: requested=%llu signaled=%llu "
              "submitted=%llu\n",
              (unsigned)q.inline_submit_entries_at_capture,
              q.inline_from_timeline_ioctl ? "TIMELINE_WAIT" : "SYNCOBJ_WAIT",
              (unsigned)q.inline_handle, (unsigned)q.inline_count,
              (unsigned)q.inline_flags, (unsigned)q.inline_first_index,
              (long long)q.inline_timeout_nsec, (unsigned)q.inline_obj_in_use,
              (unsigned)q.inline_obj_kind, (unsigned)q.inline_binary_signaled,
              (unsigned)q.inline_submission_refs,
              (unsigned long long)q.inline_requested_point,
              (unsigned long long)q.inline_signaled_point,
              (unsigned long long)q.inline_submitted_point);

      if (q.inline_obj_kind == 2 && q.inline_requested_point == 0) {
         fprintf(stderr,
                 "NTAGX FATAL: the evaluator's TIMELINE clause treats point 0 "
                 "as unconditionally satisfied, and this wait asked for point "
                 "0 on a TIMELINE object. Every fence on this object is "
                 "satisfied the instant it is asked about, forever, whatever "
                 "the GPU did.\n");
      } else if (q.inline_obj_kind != 2 && q.inline_binary_signaled) {
         fprintf(stderr,
                 "NTAGX FATAL: the fence's binary payload was ALREADY "
                 "SIGNALLED when Mesa asked. Compare signal=%u reset=%u "
                 "create_signaled=%u out_signal=%u above: if all four are 0, "
                 "nothing in the driver signalled it and the payload's "
                 "initial state is the bug.\n",
                 (unsigned)q.syncobj_signal_calls,
                 (unsigned)q.syncobj_reset_calls,
                 (unsigned)q.syncobj_create_signaled,
                 (unsigned)q.output_signal_calls);
      } else if (q.inline_obj_kind != 2 && !q.inline_binary_signaled) {
         fprintf(stderr,
                 "NTAGX FATAL: an UNSIGNALLED binary object was reported "
                 "SATISFIED. The evaluator itself is wrong -- this is in "
                 "kmd-syncobj-core, not in the driver wiring around it.\n");
      }
   }
   /*
    * FIRMWARE LIVENESS -- measured without the interrupt.
    *
    * Every counter above that describes firmware behaviour sits downstream of
    * the mailbox ISR, so they all read zero together when the interrupt is
    * what failed. These two do not. DeviceControl is the channel the driver
    * writes and firmware reads; the Event channel is the one firmware writes
    * and the driver reads. Between them they say whether firmware has ever
    * done anything at all.
    */
   {
      static const char *const evchan[] = {
         "NOT_ATTEMPTED", "OK", "NO_INITDATA", "NOT_MAPPED",
         "LAYOUT_FAILED", "OUT_OF_RANGE",
      };
      uint32_t st = q.event_chan_status;

      fprintf(stderr,
              "NTAGX_FENCE[%s]: devctrl rptr=%u wptr=%u polls=%u%s | "
              "event_chan %s rptr=%u wptr=%u cache=%u state_dva=0x%" PRIx64
              " ring_dva=0x%" PRIx64 "\n",
              when, (unsigned)q.devctrl_rptr, (unsigned)q.devctrl_wptr,
              (unsigned)q.devctrl_poll_count,
              q.devctrl_valid ? "" : " (NEVER SAMPLED)",
              st < ARRAY_SIZE(evchan) ? evchan[st] : "?",
              (unsigned)q.event_chan_rptr, (unsigned)q.event_chan_wptr,
              (unsigned)q.event_chan_cache_type, q.event_chan_state_dva,
              q.event_chan_ring_dva);

      static bool liveness_reported;
      if (!liveness_reported && q.submit_ioctl_entries > 0 &&
          st == AGXKMD_DIAG_EVCHAN_OK) {
         liveness_reported = true;
         if (q.event_chan_wptr != 0) {
            fprintf(stderr,
                    "NTAGX FATAL: firmware DID post %u Event message(s) -- the "
                    "work completed and the NOTIFICATION is what went missing. "
                    "The GPU is fine; the RX doorbell or the mailbox interrupt "
                    "is the defect, and every drain counter reading zero is a "
                    "consequence of that, not evidence about the GPU.\n",
                    (unsigned)q.event_chan_wptr);
         } else if (q.devctrl_valid && q.devctrl_rptr == 0 &&
                    q.devctrl_wptr != 0) {
            fprintf(stderr,
                    "NTAGX FATAL: firmware has never consumed a message from "
                    "ANY channel (devctrl rptr=0, wptr=%u after %u polls) and "
                    "has posted no Event. It is not executing the channel "
                    "protocol at all -- look at boot/initdata, not at the "
                    "submission path.\n",
                    (unsigned)q.devctrl_wptr,
                    (unsigned)q.devctrl_poll_count);
         } else {
            fprintf(stderr,
                    "NTAGX FATAL: firmware consumed channel messages "
                    "(devctrl rptr=%u) but has posted NO Event after %u "
                    "submission(s). It is reading the work and not completing "
                    "it -- the defect is in what the work item asks for, not "
                    "in the notification path.\n",
                    (unsigned)q.devctrl_rptr,
                    (unsigned)q.submit_ioctl_entries);
         }
      }
   }

   /*
    * NAME THE PROBE, so nobody chases it a second time.
    *
    * vk_drm_syncobj_get_type_from_provider() (vk_drm_syncobj.c:718) creates a
    * syncobj with DRM_SYNCOBJ_CREATE_SIGNALED, waits on it with timeout 0 and
    * WAIT_ALL, and destroys it -- all before any submission exists. That wait
    * MUST be satisfied inline: if it is not, Mesa clears
    * VK_SYNC_FEATURE_CPU_WAIT and wait_many, and vkWaitForFences stops
    * working entirely. It is required behaviour. An earlier build of this
    * reader reported exactly this sequence as the defect.
    */
   static bool probe_named;
   if (!probe_named && q.syncobj_create_signaled_pre_submit > 0) {
      probe_named = true;
      fprintf(stderr,
              "NTAGX_FENCE: create_signaled=%u of which %u before any submit "
              "-- that is Mesa's vk_drm_syncobj_get_type feature probe "
              "(create SIGNALED, wait timeout=0 WAIT_ALL, destroy). It is "
              "REQUIRED to be satisfied inline and is NOT a defect.\n",
              (unsigned)q.syncobj_create_signaled,
              (unsigned)q.syncobj_create_signaled_pre_submit);
   }

   /*
    * The fence waits themselves. The probe accounts for exactly one wait
    * ioctl; anything the application waits on must show up beyond it.
    */
   static bool wait_verdict;
   if (!wait_verdict && q.submit_ioctl_entries > 0) {
      uint32_t waits = q.syncobj_wait_ioctl_entries +
                       q.syncobj_timeline_wait_ioctl_entries;
      uint32_t probe_waits = q.wait_eval_satisfied_pre_submit;

      if (waits <= probe_waits) {
         wait_verdict = true;
         fprintf(stderr,
                 "NTAGX FATAL: %u submission(s) have been issued and NO wait "
                 "ioctl beyond the init-time probe has ever reached the "
                 "driver (waits=%u, probe accounts for %u). Every "
                 "vkWaitForFences is being resolved inside Mesa without "
                 "asking the kernel anything -- look above the sync provider "
                 "(vk_drm_syncobj_wait_many's wait_count==0 short circuit), "
                 "not at the KMD.\n",
                 (unsigned)q.submit_ioctl_entries, waits, probe_waits);
      } else if (q.wait_eval_pending == 0 &&
                 q.wait_eval_satisfied_post_submit > 0) {
         wait_verdict = true;
         fprintf(stderr,
                 "NTAGX FATAL: %u wait(s) after a submission were ALL "
                 "satisfied inline and not one ever pended, while "
                 "out_signal=%u. The fence is resolved without any completion "
                 "having signalled it.\n",
                 (unsigned)q.wait_eval_satisfied_post_submit,
                 (unsigned)q.output_signal_calls);
      }
   }

   /*
    * The verdicts. Each names the alternative it rules out, because a bare
    * number here has already been misread once per debugging round on this
    * project. Reported once per process: this runs on the fence path.
    */
   static bool verdict_reported;
   if (!verdict_reported) {
      bool have_object = (q.counter_flags & AGXKMD_DIAG_C_MAILBOX_INTERRUPT);
      bool connected = (q.counter_flags & AGXKMD_DIAG_C_INTERRUPTS_CONNECTED);

      if (q.isr_entries == 0 && !have_object) {
         verdict_reported = true;
         fprintf(stderr,
                 "NTAGX_FENCE: no WDFINTERRUPT was ever created (calibration "
                 "or stage gate refused it), so isr=0 is EXPECTED and says "
                 "nothing about interrupt delivery. Firmware cannot complete "
                 "work through the mailbox on this boot.\n");
      } else if (q.isr_entries == 0 && connected) {
         verdict_reported = true;
         fprintf(stderr,
                 "NTAGX FATAL: the mailbox interrupt was CONNECTED and has "
                 "NEVER FIRED (isr_entries=0). Whatever signalled the fence, "
                 "it was not a firmware completion event -- so the fence's "
                 "latency is that other mechanism's phase, which is exactly "
                 "why it is constant per process. This is a new bug and it is "
                 "upstream of everything the memory path already exonerated.\n");
      } else if (q.isr_entries > 0 && q.event_drain_events == 0) {
         verdict_reported = true;
         fprintf(stderr,
                 "NTAGX FATAL: the interrupt fires (isr_entries=%u) but the "
                 "Event ring has never once held an event (polls=%u, "
                 "events=0). Firmware is talking on the mailbox and never "
                 "reporting a completion.\n",
                 (unsigned)q.isr_entries, (unsigned)q.event_drain_polls);
      } else if (q.outcome_signals_hardware == 0 &&
                 q.outcome_signals_host > 0) {
         verdict_reported = true;
         fprintf(stderr,
                 "NTAGX FATAL: every fence so far was signalled by a HOST-side "
                 "failure or recovery path (%u), never by a firmware "
                 "completion event (0). The syncobj is signalled identically "
                 "either way, so Vulkan cannot see the difference -- the fence "
                 "is truthful about being resolved and silent about what "
                 "resolved it.\n",
                 (unsigned)q.outcome_signals_host);
      }
      if (q.counter_flags & AGXKMD_DIAG_C_COMPLETION_POISONED)
         fprintf(stderr, "NTAGX_FENCE: the KMD completion path is POISONED "
                         "(fail-closed); no further event will be "
                         "interpreted.\n");
      if (q.counter_flags & AGXKMD_DIAG_C_MAILBOX_OVERFLOW)
         fprintf(stderr, "NTAGX_FENCE: the KMD saw a mailbox ring overflow -- "
                         "the RTKit protocol is desynchronized.\n");
   }
   simple_mtx_unlock(&ntagx_counter_mtx);
}
#endif

static int
win_submit(struct agx_device *dev, struct drm_asahi_submit *submit,
           struct agx_submit_virt *virt)
{
   /* `virt` is unused, matching agx_submit()'s Linux/drm body exactly --
    * the win backend has no vdrm concept to route it through. */
   (void)virt;

   /* NTAGX_SUBMIT_LOG=1 reports what was actually handed to the kernel and
    * what the kernel said about it: a cmdbuf_size of 0 or a null cmdbuf means
    * the GPU was asked to do nothing and signalled accordingly. */
#if AGX_GPU_DIAG >= 2
   static int submit_log = -1;
   if (submit_log < 0) {
      const char *v = getenv("NTAGX_SUBMIT_LOG");
      submit_log = (v && v[0] && v[0] != '0') ? 1 : 0;
   }
#else
   const int submit_log = 0;
#endif

   if (submit_log) {
      fprintf(stderr,
              "NTAGX_SUBMIT: queue_id=%u flags=0x%x cmdbuf=0x%" PRIx64
              " cmdbuf_size=%u syncs=0x%" PRIx64 " in=%u out=%u\n",
              (unsigned)submit->queue_id, (unsigned)submit->flags,
              (uint64_t)submit->cmdbuf, (unsigned)submit->cmdbuf_size,
              (uint64_t)submit->syncs, (unsigned)submit->in_sync_count,
              (unsigned)submit->out_sync_count);
   }

   int submit_ret = ntagx_ioctl(dev->hdev, DRM_IOCTL_ASAHI_SUBMIT, submit);

   if (submit_log)
      fprintf(stderr, "NTAGX_SUBMIT: ioctl returned %d\n", submit_ret);

#if AGX_GPU_DIAG >= 2
   if (submit_ret == 0)
      ntagx_submit_verify(dev);
#endif

   /*
    * The BEFORE half of the bracket. Nothing of this submission has completed
    * yet -- the submit ioctl returns as soon as the work is enqueued to the
    * KMD's passive worker -- so this sample describes the state the fence wait
    * is about to start from. The matching AFTER half is taken in
    * agx_sync_win.c the instant the wait ioctl returns, and the delta between
    * them is what actually ran while the fence was outstanding.
    */
   if (submit_ret == 0)
      ntagx_completion_counters_report(dev->hdev, "after submit");

   return submit_ret;
}

static int
win_bo_bind_object(struct agx_device *dev,
                    struct drm_asahi_gem_bind_object *bind)
{
   int ret = ntagx_ioctl(dev->hdev, DRM_IOCTL_ASAHI_GEM_BIND_OBJECT, bind);
   if (ret) {
      fprintf(stderr, "DRM_IOCTL_ASAHI_GEM_BIND_OBJECT failed: handle=%u\n",
              bind->handle);
   }

   return ret;
}

static int
win_bo_unbind_object(struct agx_device *dev, uint32_t object_handle)
{
   struct drm_asahi_gem_bind_object gem_bind = {
      .op = DRM_ASAHI_BIND_OBJECT_OP_UNBIND,
      .object_handle = object_handle,
   };

   int ret =
      ntagx_ioctl(dev->hdev, DRM_IOCTL_ASAHI_GEM_BIND_OBJECT, &gem_bind);
   if (ret) {
      fprintf(stderr,
              "DRM_IOCTL_ASAHI_GEM_BIND_OBJECT (unbind) failed: "
              "object_handle=%u\n",
              object_handle);
   }

   return ret;
}

/*
 * honeykrisp-h1-execution.md SS1/SS2.1: passthrough for the four
 * asahi_simple_ioctl() callers (VM_CREATE, QUEUE_CREATE, QUEUE_DESTROY,
 * GET_TIME) once they are promoted to `dev->ops.simple_ioctl(...)`.
 */
static int
win_simple_ioctl(struct agx_device *dev, unsigned cmd, void *req)
{
   return ntagx_ioctl(dev->hdev, cmd, req);
}

/*
 * OUR DESIGN (honeykrisp-h1-execution.md SS1 "bo_munmap"/"bo_free" rows):
 * no Linux body to clone -- munmap(2) and DRM_IOCTL_GEM_CLOSE are DRM-core/
 * OS primitives with no NT DeviceIoControl equivalent, so these route
 * through the two Windows-only ids (22, 20) instead.
 */
static void
win_bo_munmap(struct agx_device *dev, struct agx_bo *bo)
{
   struct ntasi_kmd_uapi_gem_munmap req = {.handle = bo->uapi_handle};

   ntagx_ioctl(dev->hdev, NTASI_KMD_UAPI_IOCTL_GEM_MUNMAP, &req);
}

static void
win_bo_free(struct agx_device *dev, uint32_t handle)
{
   struct ntasi_kmd_uapi_gem_close req = {.handle = handle};

   ntagx_ioctl(dev->hdev, NTASI_KMD_UAPI_IOCTL_GEM_CLOSE, &req);
}

static int
win_bo_sync_for_device(struct agx_device *dev, struct agx_bo *bo,
                       size_t offset, size_t size)
{
   (void)dev;
   (void)bo;
   (void)offset;
   (void)size;
   return 0;
}

const agx_device_ops_t agx_device_win_ops = {
   .bo_alloc = win_bo_alloc,
   .bo_bind = win_bo_bind,
   .bo_mmap = win_bo_mmap,
   .get_params = win_get_params,
   .submit = win_submit,
   .bo_bind_object = win_bo_bind_object,
   .bo_unbind_object = win_bo_unbind_object,
   .simple_ioctl = win_simple_ioctl,
   .bo_munmap = win_bo_munmap,
   .bo_sync_for_device = win_bo_sync_for_device,
   .bo_free = win_bo_free,
};

bool
agx_win_open_device(struct agx_device *dev, void *hdev)
{
   dev->hdev = hdev;
   dev->is_virtio = false;
   dev->ops = agx_device_win_ops;
   /* The origin for every delta printed afterwards, and the one sample that
    * proves the query works at all before any submission has happened. */
   ntagx_completion_counters_report(hdev, "device open");
   return true; /* caller then runs agx_open_device() */
}

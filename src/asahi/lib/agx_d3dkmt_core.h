/*
 * Copyright 2026 the Asahi Linux contributors
 * SPDX-License-Identifier: MIT
 */
#pragma once

/*
 * The portable half of the D3DKMT winsys: everything that is a decision
 * rather than a syscall.
 *
 * This file and agx_d3dkmt_core.c depend on nothing but <stdint.h>,
 * <stdbool.h>, <stddef.h>, the vendored D3DKMT ABI (agx_d3dkmt.h) and the
 * miniport's own contract header (drivers/AppleAgxWddm/core/agxw_contract.h).
 * No Mesa headers, no <windows.h>, no generated headers. That is what lets
 * gpu/honeykrisp-port/tests/run-host-tests.sh compile and run every rule below
 * natively on macOS, with no Windows, no GPU and no miniport; the Windows-only
 * half (agx_device_d3dkmt.c, agx_sync_d3dkmt.c) is then a shell whose only
 * untested content is the D3DKMT calls themselves.
 *
 * agxw_contract.h is INCLUDED, not transcribed. Every constant, struct and
 * validator the kernel side uses is the same object file's worth of source on
 * both sides of the boundary, so a contract bump is a compile error here
 * rather than a silent disagreement at runtime.
 *
 * Four things live here because all four are places an AGX concept and a WDDM
 * concept fail to line up, where a wrong answer is silent rather than a
 * compile error:
 *
 *   1. handle spaces      -- AGX handles are dense small integers used as
 *                            array indices; D3DKMT handles are not.
 *   2. VM_BIND translation -- flags split across three different WDDM fields.
 *   3. binary syncobjs    -- DRM binary syncobjs can be reset; WDDM monitored
 *                            fences are monotonic and cannot.
 *   4. wait timeouts      -- DRM passes an absolute CLOCK_MONOTONIC deadline;
 *                            Win32 waits take a relative millisecond count.
 *
 * plus one the contract surfaced: the user GPU VA window is NOT contiguous.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "agx_d3dkmt.h"
#include "agxw_contract.h"

#ifdef __cplusplus
extern "C" {
#endif

/* AGX UAT page size. From the contract, not from ail, so this file stays
 * Mesa-free; agx_device_d3dkmt.c static_asserts the two agree. */
#define AGX_D3DKMT_PAGE_SIZE AGXW_PAGE_SIZE

/* D3DDDI_RESERVEGPUVIRTUALADDRESS is a Windows UMD ABI rather than an AGX
 * page-table ABI: BaseAddress, MinimumAddress, MaximumAddress and Size are
 * all required to be 64 KiB aligned even though AGX maps native 16 KiB
 * pages. Keep that distinction at the D3DKMT boundary. */
#define AGX_D3DKMT_RESERVATION_GRANULARITY UINT64_C(0x10000)

/*
 * Honeykrisp reserves the first 64 GiB for robust zero-extended indexing and
 * the following 16 KiB for its fixed printf buffer.  The executable heap is
 * therefore the next 4 GiB-aligned window.  Keep these invariants in the
 * Windows-free core so a corrupted runtime policy can be rejected before it
 * becomes DriverProtection metadata consumed by VidMm.
 */
#define AGX_D3DKMT_USC_ALIGNMENT UINT64_C(0x100000000)
#define AGX_D3DKMT_USC_SIZE      UINT64_C(0x100000000)
#define AGX_D3DKMT_USC_MIN_BASE  UINT64_C(0x1100000000)

/*
 * Runtime policy: contract answers that may legitimately differ per adapter or
 * per contract revision, kept out of the call sites so reconciling them is a
 * data change.
 */
struct agx_d3dkmt_policy {
   /* CONTRACT.md SS7.2 item 3: residency is expressed by D3DKMTMakeResident.
    * SS4.8 notes the v0.1 aperture never evicts, so the paging fence is
    * normally already signalled -- but the call is not optional. */
   bool needs_residency;
   /* CONTRACT.md SS5.3: one node, both 3D and compute. */
   uint32_t node_ordinal;
   /*
    * The USC window (Mesa's AGX_BO_LOW_VA heap), reserved separately with
    * Minimum/MaximumAddress. Binds landing inside it get Protection.Execute
    * and DriverProtection AGXW_VAPROT_USC.
    *
    * CONTRACT.md SS3.3: LOW_VA and EXEC are NOT allocation flags -- they are a
    * VA-placement property and a mapping-protection property respectively, on
    * AGX exactly as on WDDM. This is where that lands in the UMD: the winsys
    * derives execute permission from the address, which is the only
    * information a bind op carries (drm_asahi_gem_bind_op has no exec bit,
    * asahi_drm.h:366-399).
    */
   uint64_t usc_base;
   uint64_t usc_size;
};

static inline struct agx_d3dkmt_policy
agx_d3dkmt_policy_default(void)
{
   return (struct agx_d3dkmt_policy){
      .needs_residency = true,
      .node_ordinal = AGXW_NODE_3D,
      .usc_base = 0,
      .usc_size = 0,
   };
}

/* -------------------------------------------------------------------------
 * 1. Handle spaces.
 *
 * agx_device.c uses BO handles as dense array indices in two places that are
 * linear in the handle VALUE, not the handle COUNT:
 *
 *   agx_device.c:835  for (uint32_t handle = 0; handle < dev->max_handle; ...)
 *   agx_bo.c:245      for (uint32_t handle = 0; handle < dev->max_handle; ...)
 *
 * and both bodies call agx_lookup_bo() -> util_sparse_array_get(), which
 * ALLOCATES the node for any index it is handed. The Linux/virtio backends are
 * safe because DRM GEM handles are dense from 1. D3DKMT handles are not:
 * dxgkrnl mints them from a handle table with type and instance bits in the
 * upper half, so a single allocation can set dev->max_handle near 2^31 and
 * turn either loop into an out-of-memory walk.
 *
 * So the winsys keeps AGX's dense space for bo->handle (the bo_map index) and
 * puts the D3DKMT handle in bo->uapi_handle -- which is what uapi_handle
 * already exists for ("Handle to refer to this BO in uAPI calls", agx_bo.h:94)
 * and is already what every kernel-facing call site passes (agx_device.c:139,
 * :157, hk_queue.c:473, hk_buffer.c:85). bo->handle is used only as a bo_map
 * index, by drmPrimeHandleToFD (Linux-only) and as the argument to
 * ops.bo_free -- which is why this pool must also answer "what D3DKMT handle
 * did dense handle N have" AFTER agx_bo_free() has zeroed the struct agx_bo.
 * ------------------------------------------------------------------------- */
enum agx_d3dkmt_external_memory_type {
   AGX_D3DKMT_EXTERNAL_MEMORY_NONE = 0,
   AGX_D3DKMT_EXTERNAL_MEMORY_OPAQUE_WIN32 = 1,
   AGX_D3DKMT_EXTERNAL_MEMORY_OPAQUE_WIN32_KMT = 2,
};

/* Private in-process bridge between Aurora's renamed DXVK UMD and
 * Honeykrisp. A normal KMT handle is a zero-extended 32-bit global-share
 * identifier. This tag instead carries a process-local D3DKMT allocation in
 * the low word, and is accepted only by hk_AllocateMemory for KMT imports. */
#define AGX_D3DKMT_LOCAL_ALLOCATION_TAG  UINT64_C(0x4157584c00000000)
#define AGX_D3DKMT_LOCAL_ALLOCATION_MASK UINT64_C(0xffffffff00000000)

static inline bool
agx_d3dkmt_is_local_allocation_handle(uintptr_t handle)
{
   return (uint64_t)handle != 0 &&
          ((uint64_t)handle & AGX_D3DKMT_LOCAL_ALLOCATION_MASK) ==
             AGX_D3DKMT_LOCAL_ALLOCATION_TAG &&
          ((uint32_t)handle != 0);
}

static inline agx_kmt_handle
agx_d3dkmt_local_allocation_handle(uintptr_t handle)
{
   return agx_d3dkmt_is_local_allocation_handle(handle)
             ? (agx_kmt_handle)(uint32_t)handle
             : 0;
}

/* Every local allocation handle has exactly one lifetime owner. A non-zero
 * resource owns the allocation and is destroyed as a resource; otherwise the
 * allocation itself is destroyed. global_share is only the non-owning KMT
 * identifier and must never be closed or treated as a reference. */
struct agx_d3dkmt_allocation_record {
   agx_kmt_handle allocation;
   agx_kmt_handle resource;
   agx_kmt_handle global_share;
   enum agx_d3dkmt_external_memory_type external_type;
   bool borrowed;
};

struct agx_d3dkmt_handle_pool {
   struct agx_d3dkmt_allocation_record *records;
   uint32_t *freelist;
   uint32_t capacity;
   uint32_t next;  /* lowest dense handle never yet handed out */
   uint32_t nfree; /* entries in freelist */
};

void agx_d3dkmt_pool_init(struct agx_d3dkmt_handle_pool *pool);
void agx_d3dkmt_pool_finish(struct agx_d3dkmt_handle_pool *pool);

/*
 * Bind `kmt` (non-zero) to a fresh dense handle. Returns the dense handle, or
 * 0 on allocation failure. Dense handles start at 1 because agx_bo_bind() uses
 * handle 0 as "no BO" (agx_device.c:139).
 */
uint32_t agx_d3dkmt_pool_alloc(struct agx_d3dkmt_handle_pool *pool,
                               agx_kmt_handle kmt);

uint32_t agx_d3dkmt_pool_alloc_record(
   struct agx_d3dkmt_handle_pool *pool,
   const struct agx_d3dkmt_allocation_record *record);

/* The D3DKMT handle for `dense`, or 0 if `dense` is not live. */
agx_kmt_handle agx_d3dkmt_pool_get(const struct agx_d3dkmt_handle_pool *pool,
                                   uint32_t dense);

bool agx_d3dkmt_pool_get_record(
   const struct agx_d3dkmt_handle_pool *pool, uint32_t dense,
   struct agx_d3dkmt_allocation_record *record);

/*
 * Release `dense` and return the D3DKMT handle it held (0 if not live). The
 * dense handle becomes reusable, which keeps dev->max_handle proportional to
 * live BOs rather than to BOs ever created.
 */
agx_kmt_handle agx_d3dkmt_pool_release(struct agx_d3dkmt_handle_pool *pool,
                                       uint32_t dense);

bool agx_d3dkmt_pool_release_record(
   struct agx_d3dkmt_handle_pool *pool, uint32_t dense,
   struct agx_d3dkmt_allocation_record *record);

/* -------------------------------------------------------------------------
 * 2. VM_BIND translation.
 * ------------------------------------------------------------------------- */

/* enum drm_asahi_bind_flags, mirrored so this file needs no uapi header.
 * agx_device_d3dkmt.c static_asserts these against the real values. */
#define AGX_D3DKMT_BIND_UNBIND      (1u << 0)
#define AGX_D3DKMT_BIND_READ        (1u << 1)
#define AGX_D3DKMT_BIND_WRITE       (1u << 2)
#define AGX_D3DKMT_BIND_SINGLE_PAGE (1u << 3)

/* enum agx_bo_flags (agx_bo.h:17-39), likewise mirrored and static_asserted. */
#define AGX_D3DKMT_BO_SHARED    (1u << 0)
#define AGX_D3DKMT_BO_LOW_VA    (1u << 1)
#define AGX_D3DKMT_BO_EXEC      (1u << 2)
#define AGX_D3DKMT_BO_WRITEBACK (1u << 3)
#define AGX_D3DKMT_BO_SHAREABLE (1u << 4)
#define AGX_D3DKMT_BO_READONLY  (1u << 5)
#define AGX_D3DKMT_BO_SCANOUT_DONOR (1u << 6)

/* A drm_asahi_gem_bind_op with the GEM handle already resolved to D3DKMT. */
struct agx_d3dkmt_bind {
   uint32_t flags; /* AGX_D3DKMT_BIND_* */
   agx_kmt_handle kmt_handle;
   uint64_t offset;
   uint64_t range;
   uint64_t addr;
};

#define AGX_D3DKMT_OK          0
#define AGX_D3DKMT_ERR_INVAL   (-1)
#define AGX_D3DKMT_ERR_NOSPACE (-2)

/*
 * Translate `count` binds into exactly `count` D3DKMT page-table operations
 * (CONTRACT.md SS4.4: D3DKMTUpdateGpuVirtualAddress' operation array is the
 * structural match for drm_asahi_vm_bind's num_binds x stride array, and
 * preserves VM_BIND's atomicity expectation).
 *
 * Every AGX bind flag has a home, and the contract put each in the right one:
 *
 *   BIND_UNBIND       -> UNMAP with Protection.NoAccess.
 *   BIND_READ only    -> MAP_PROTECT, Protection.Write = 0.
 *   BIND_READ|WRITE   -> MAP_PROTECT, Protection.Write = 1.
 *   BIND_WRITE only   -> Protection.Write = 1 plus DriverProtection
 *                        AGXW_VAPROT_WRITE_ONLY. WDDM has no write-only
 *                        encoding; AGX's AP bits do (CONTRACT.md SS4.7).
 *   BIND_SINGLE_PAGE  -> DriverProtection AGXW_VAPROT_SINGLE_PAGE, ONE
 *                        operation covering the whole range. This is the
 *                        contract's answer and it is much better than the
 *                        alternative: expanding a 1 GiB sparse scratch bind
 *                        (hk_bind_scratch(), hk_buffer.c:81-99) into 65536
 *                        per-page MAP operations, or into 17 COPY operations
 *                        with a page-table-doubling trick. The AGX UAT can
 *                        alias one physical page across a range natively, so
 *                        the honest expression is one op with a driver bit.
 *   USC range         -> Protection.Execute plus AGXW_VAPROT_USC. Derived from
 *                        the ADDRESS, because that is the only signal a bind
 *                        op carries; see policy.usc_base.
 *
 * Call with out == NULL to size (*out_count receives the requirement); passing
 * too small a buffer returns AGX_D3DKMT_ERR_NOSPACE with the requirement.
 * Ranges are validated against contract rule G1 (16 KiB alignment everywhere)
 * and against the user VA window including the kernel carve-out, using the
 * miniport's own agxw_va_range_is_bindable/agxw_va_range_is_user.
 */
int agx_d3dkmt_translate_binds(const struct agx_d3dkmt_bind *binds,
                               uint32_t count,
                               const struct agx_d3dkmt_policy *policy,
                               struct agx_kmt_updategpuva_op *out,
                               uint32_t out_capacity, uint32_t *out_count);

/*
 * Rebuild the bind-time executable policy from agx_device::shader_base and
 * require the long-lived winsys copy to agree exactly.  The returned object
 * is a caller-local snapshot: translation never trusts mutable USC bounds
 * directly out of the winsys.
 */
int agx_d3dkmt_bind_policy(uint64_t shader_base,
                           const struct agx_d3dkmt_policy *runtime,
                           struct agx_d3dkmt_policy *out);

/*
 * Last-boundary validation for the exact operation array handed to
 * D3DKMTUpdateGpuVirtualAddress.  It catches mutation, stale stack contents,
 * and any USC bit outside the immutable production window before dxgkrnl can
 * turn a malformed request into a paging callback.
 */
int agx_d3dkmt_validate_translated_binds(
   const struct agx_d3dkmt_bind *binds, uint32_t count,
   const struct agx_d3dkmt_policy *policy,
   const struct agx_kmt_updategpuva_op *ops, uint32_t op_count);

/* -------------------------------------------------------------------------
 * 2b. The user VA window is not contiguous.
 *
 * CONTRACT.md SS4.2: a single 16 KiB kernel/firmware page is carved out of the
 * MIDDLE of the user window at AGXW_VM_KERNEL_CARVEOUT_BASE. The miniport
 * reports it through DXGK_DRIVERCAPS::InternalGpuVirtualAddressRange so VidMm
 * never hands it out -- but a reservation spanning it would still be asking
 * for a range that includes it.
 *
 * Align [base, base+size) inward to the WDDM 64 KiB reservation ABI, then
 * split it around the 64 KiB reservation block containing the 16 KiB AGX
 * carve-out. Writes 1 or 2 sub-ranges into out[] and returns the count;
 * returns 0 when no complete WDDM reservation block remains. out must have
 * room for 2.
 */
struct agx_d3dkmt_va_range {
   uint64_t base;
   uint64_t size;
};

uint32_t agx_d3dkmt_va_split_carveout(uint64_t base, uint64_t size,
                                      struct agx_d3dkmt_va_range out[2]);

/* -------------------------------------------------------------------------
 * 3. Binary syncobjs over monotonic monitored fences.
 *
 * CONTRACT.md SS6.4: the miniport deliberately does NOT emulate DRM's reset in
 * the kernel, and points at a userspace epoch. This is that epoch.
 *
 *   signal()  -> signal base + 1, unless the fence is already there
 *                (signalling twice without an intervening reset is legal in
 *                DRM and must not become an illegal fence rewind here).
 *   reset()   -> base = the fence's CURRENT value, which makes base + 1
 *                unreached again without touching the fence.
 *   wait()    -> wait for base + 1.
 *
 * Timeline use never calls reset(), so base stays 0 and every timeline point
 * maps to the identical fence value; the two uses coexist in one object
 * exactly as they do in DRM.
 *
 * KNOWN LIMIT, reported back to the miniport: the epoch is process-local. A
 * syncobj shared with another process and then reset would leave the two
 * processes disagreeing about which value means "signalled". No Honeykrisp
 * path does that today -- the fd import/export slots are hard stubs on
 * Windows, so no syncobj can be shared at all -- but it is the constraint that
 * VK_KHR_external_semaphore_win32 would have to be designed around.
 * ------------------------------------------------------------------------- */
struct agx_d3dkmt_sync {
   agx_kmt_handle kmt;    /* monitored fence handle; 0 = free slot */
   uint64_t base;         /* binary epoch */
   uint64_t last_request; /* highest value we have asked anyone to signal */
   /* CONTRACT.md SS6.3: guaranteed non-NULL by the miniport. Kept optional
    * here so the degradation is explicit rather than a crash if it ever
    * isn't. */
   const volatile uint64_t *cpu_va;
};

struct agx_d3dkmt_sync_table {
   struct agx_d3dkmt_sync *slots;
   uint32_t *freelist;
   uint32_t capacity;
   uint32_t next;
   uint32_t nfree;
};

void agx_d3dkmt_sync_table_init(struct agx_d3dkmt_sync_table *t);
void agx_d3dkmt_sync_table_finish(struct agx_d3dkmt_sync_table *t);

/* Returns a dense syncobj handle (>= 1), or 0 on allocation failure. */
uint32_t agx_d3dkmt_sync_table_add(struct agx_d3dkmt_sync_table *t,
                                   agx_kmt_handle kmt,
                                   const volatile uint64_t *cpu_va,
                                   uint64_t initial_value);
struct agx_d3dkmt_sync *
agx_d3dkmt_sync_table_get(struct agx_d3dkmt_sync_table *t, uint32_t handle);
/* Removes `handle`, returning the D3DKMT fence handle (0 if not live). */
agx_kmt_handle agx_d3dkmt_sync_table_remove(struct agx_d3dkmt_sync_table *t,
                                            uint32_t handle);

/* Current fence value: the kernel's if we have a CPU mapping, else the last
 * value we requested. */
uint64_t agx_d3dkmt_sync_current(const struct agx_d3dkmt_sync *s);

/*
 * Value to signal for a binary signal(). Sets *skip when the fence has already
 * reached it, in which case no D3DKMT call must be made -- issuing one would
 * be a fence rewind.
 */
uint64_t agx_d3dkmt_binary_signal_value(const struct agx_d3dkmt_sync *s,
                                        bool *skip);

/* Value to wait for in a binary wait(). */
uint64_t agx_d3dkmt_binary_wait_value(const struct agx_d3dkmt_sync *s);

/* New epoch base for a binary reset(). */
uint64_t agx_d3dkmt_binary_reset_base(const struct agx_d3dkmt_sync *s);

/* Translate DRM syncobj-transfer points to monotonic D3DKMT fence values.
 * Point zero denotes the binary payload; non-zero points are timeline
 * values. */
void agx_d3dkmt_sync_transfer_values(const struct agx_d3dkmt_sync *src,
                                     const struct agx_d3dkmt_sync *dst,
                                     uint64_t src_point, uint64_t dst_point,
                                     uint64_t *src_value,
                                     uint64_t *dst_value);

/* -------------------------------------------------------------------------
 * 4. Timeouts and wait bookkeeping.
 * ------------------------------------------------------------------------- */
#define AGX_D3DKMT_INFINITE 0xFFFFFFFFu

/*
 * DRM syncobj waits take an ABSOLUTE CLOCK_MONOTONIC deadline in nanoseconds
 * (drm.h:930-931), with INT64_MAX meaning "forever". Win32 waits take a
 * RELATIVE millisecond count with 0xFFFFFFFF meaning INFINITE. Rounding is up
 * so a sub-millisecond deadline never becomes a zero-length poll, and the
 * result saturates one below INFINITE so a long-but-finite wait can never be
 * misread as an infinite one.
 */
uint32_t agx_d3dkmt_timeout_to_ms(int64_t abs_timeout_nsec, int64_t now_nsec);

/*
 * D3DKMTWaitForSynchronizationObjectFromCpu reports only that the wait was
 * satisfied, never which object satisfied it, while DRM's wait reports
 * `first_signaled`. Recompute it by re-reading the fence values -- free, given
 * CONTRACT.md SS6.3's CPU-visible fence storage. Returns the index of the
 * first object whose current value has reached its wait point, or UINT32_MAX.
 */
uint32_t agx_d3dkmt_first_signaled(const uint64_t *current,
                                   const uint64_t *wanted, uint32_t count);

enum agx_d3dkmt_cpu_wait_state {
   AGX_D3DKMT_CPU_WAIT_INVALID = -1,
   AGX_D3DKMT_CPU_WAIT_PENDING = 0,
   AGX_D3DKMT_CPU_WAIT_SATISFIED = 1,
};

/*
 * Probe the actual CPU-visible monitored-fence storage for a wait condition.
 * This is deliberately stateless: a D3DKMT async wait has no cancellation
 * API, so finite waits poll the mappings instead of sharing/reusing an event
 * that dxgkrnl may still signal after a timeout.  `first_signaled` is written
 * only for a satisfied wait and may be NULL.
 */
enum agx_d3dkmt_cpu_wait_state agx_d3dkmt_cpu_wait_probe(
   const volatile uint64_t *const *current, const uint64_t *wanted,
   uint32_t count, bool wait_any, uint32_t *first_signaled);

/* -------------------------------------------------------------------------
 * Contract blob helpers.
 * ------------------------------------------------------------------------- */

/*
 * Validate a KMTQAITYPE_UMDRIVERPRIVATE reply as a struct
 * agxw_adapter_identity this port understands, and seed `policy` from it.
 * Returns AGX_D3DKMT_OK, or AGX_D3DKMT_ERR_INVAL for a blob that is not ours
 * (a foreign vendor's adapter) or is a contract version we did not compile
 * against. Nothing is written on failure.
 *
 * This is the whole of adapter recognition: CONTRACT.md SS2.3 is explicit that
 * VendorId/DeviceId are not reliable on an ACPI-enumerated adapter with no PCI
 * config space, so the identity blob is the only supported discriminator.
 */
int agx_d3dkmt_parse_identity(const void *blob, uint32_t blob_size,
                              struct agx_d3dkmt_policy *policy,
                              struct agxw_adapter_identity *out);

/*
 * enum agx_bo_flags -> enum agxw_alloc_flags, for
 * D3DDDI_ALLOCATIONINFO2::pPrivateDriverData.
 *
 * Note what is NOT here: LOW_VA and EXEC. CONTRACT.md SS3.3 is right that they
 * are not allocation properties -- drm_asahi_gem_flags has exactly WRITEBACK
 * and VM_PRIVATE (kmd_uapi.h:258-261), and Mesa's LOW_VA/EXEC never reach the
 * Linux kernel either. They are handled at bind time; see policy.usc_base.
 */
uint32_t agx_d3dkmt_agxw_alloc_flags(uint32_t agx_bo_flags);

/* CPU-authored executable storage is persistent-mapped on Windows.  Give it
 * a cacheable mapping so D3DKMTInvalidateCache can establish the required
 * CPU-write -> GPU-read ownership transition. */
uint32_t agx_d3dkmt_cpu_visible_flags(uint32_t agx_bo_flags);

/* enum agx_bo_flags -> D3DKMT_CREATEALLOCATIONFLAGS. */
uint32_t agx_d3dkmt_create_flags(uint32_t agx_bo_flags);

uint32_t agx_d3dkmt_create_flags_external(
   uint32_t agx_bo_flags,
   enum agx_d3dkmt_external_memory_type external_type);

/* Insert one GPU range into a sorted, overlap-free vector, coalescing
 * overlap and adjacency.  This is shared by the Windows winsys and host ABI
 * tests so a range-count overflow fails before SubmitCommand. */
int agx_d3dkmt_read_range_add(struct agxw_submit_read_range *ranges,
                              uint32_t *count, uint32_t capacity,
                              uint64_t gpu_va, uint64_t length);

/* Reduce a canonical range census without dropping coverage. The smallest
 * legal gaps are bridged first; KMD advisory walkers skip unmapped pages, so
 * ordinary 64 KiB VA guards cost only their page walks. A gap which crosses
 * the protected kernel carveout is never eligible. */
int agx_d3dkmt_read_ranges_compact(struct agxw_submit_read_range *ranges,
                                   uint32_t *count, uint32_t capacity,
                                   uint64_t *bridged_bytes);

/* A BO normally has only one or two independently updated CPU regions. Keep
 * those intervals exact and bounded. If a pathological writer produces more,
 * the state falls back to the current BO lifetime's whole range. */
#define AGX_D3DKMT_DIRTY_RANGES_PER_BO 8u

struct agx_d3dkmt_dirty_range {
   uint64_t offset;
   uint64_t length;
};

struct agx_d3dkmt_dirty_state {
   uint64_t identity;
   uint64_t cpu_write_epoch;
   uint64_t clean_epoch;
   uint32_t range_count;
   bool whole_bo;
   struct agx_d3dkmt_dirty_range
      ranges[AGX_D3DKMT_DIRTY_RANGES_PER_BO];
};

struct agx_d3dkmt_dirty_snapshot {
   uint64_t identity;
   uint64_t cpu_write_epoch;
   uint32_t range_count;
   bool whole_bo;
   struct agx_d3dkmt_dirty_range
      ranges[AGX_D3DKMT_DIRTY_RANGES_PER_BO];
};

enum agx_d3dkmt_dirty_result {
   AGX_D3DKMT_DIRTY_OK = 0,
   AGX_D3DKMT_DIRTY_STALE = 1,
   AGX_D3DKMT_DIRTY_INVALID = 2,
};

void agx_d3dkmt_dirty_reset(struct agx_d3dkmt_dirty_state *state,
                            uint64_t identity);
void agx_d3dkmt_dirty_reset_mapped(struct agx_d3dkmt_dirty_state *state,
                                   uint64_t identity, uint64_t bo_size,
                                   bool mapped);

enum agx_d3dkmt_dirty_result
agx_d3dkmt_dirty_mark(struct agx_d3dkmt_dirty_state *state,
                      uint64_t bo_size, uint64_t offset, uint64_t length);
enum agx_d3dkmt_dirty_result
agx_d3dkmt_dirty_mark_whole(struct agx_d3dkmt_dirty_state *state,
                            uint64_t bo_size);

bool agx_d3dkmt_dirty_detach(struct agx_d3dkmt_dirty_state *state,
                             struct agx_d3dkmt_dirty_snapshot *snapshot);

enum agx_d3dkmt_dirty_result
agx_d3dkmt_dirty_restore(struct agx_d3dkmt_dirty_state *state,
                         uint64_t bo_size,
                         const struct agx_d3dkmt_dirty_snapshot *snapshot);

enum agx_d3dkmt_dirty_result
agx_d3dkmt_dirty_commit(struct agx_d3dkmt_dirty_state *state,
                        const struct agx_d3dkmt_dirty_snapshot *snapshot);

bool agx_d3dkmt_dirty_has_pending(
   const struct agx_d3dkmt_dirty_state *state);

#ifdef __cplusplus
}
#endif

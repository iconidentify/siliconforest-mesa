/*
 * Copyright 2026 the Asahi Linux contributors
 * SPDX-License-Identifier: MIT
 */

/*
 * Runtime-device bridge implementation (Windows half). See agx_d3dkmt_bridge.h
 * and agxw_bridge_core.h.
 *
 * The winsys funnels every kernel operation through ws->fns. In bridge mode
 * each device gets its own provider SLOT (agxw_bridge_core), and its ws->fns
 * device-scope members are set to that slot's THUNK SET. The thunk for slot N
 * is a distinct function that forwards to the shared implementation with the
 * constant N -- this is how per-device state reaches a plain
 * context-free agx_d3dkmt_fns function pointer without editing any winsys call
 * site. Adapter-scope members stay the gdi32 thunks agx_d3dkmt_load installed.
 *
 * Two translations are load-bearing, not cosmetic:
 *   - CONTEXT handles are 32-bit in the winsys but pointer-width in every
 *     callback; each provider's token table (core) keeps the two apart so a
 *     token minted for one device never resolves in another.
 *   - MakeResident returns E_PENDING (0x8000000A) on the WDDM2 async residency
 *     path with a valid PagingFenceValue -- VERIFIED on hardware by the KIF
 *     probe. The shim maps it to success-class STATUS_PENDING so the winsys
 *     waits the paging fence, exactly as the gdi32 thunk does; a genuine
 *     cross-device refusal is E_INVALIDARG and still fails.
 */

#include "agx_d3dkmt_bridge.h"
#include "agx_bridge_vma.h" /* console-free ProgramData diagnostic */
#include "agxw_bridge_ddi.h"
#include "agxw_contract.h" /* validate/read struct agxw_alloc_private */
#include "agx_device.h" /* full struct agx_device, referenced by the priv header */
#include "agx_d3dkmt_priv.h"

#include <windows.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * The exported consumer ABI of the Aurora UMD. MUST match, field for field,
 * umd/compat/aurora_gpu_bridge.h.
 * ------------------------------------------------------------------------- */
/* v2: generation-based revocation. v1's `callbacks` pointed into the
 * RUNTIME-OWNED TRITON_DEVICE block, which the runtime recycles across
 * Create/Destroy; a winsys outliving its D3D device called through freed
 * memory (measured: LogonUI 0xc0000005 in the allocation flow, 0.1.340). v2
 * hands over a pointer to STABLE publisher storage plus an alive cell checked
 * before every callback. Both sides ship together; a version mismatch makes
 * acquire return 0 and bridge mode reports "no runtime bridge" fail-closed. */
#define AURORA_GPU_BRIDGE_ABI_VERSION 3u
#define AURORA_GPU_BRIDGE_MODULE "AuroraD3D11Triton.dll"

typedef struct AuroraGpuBridge {
   unsigned int struct_size;
   unsigned int abi_version;
   const void *callbacks;
   void *hRTDevice;
   const volatile long *alive;
   unsigned long alive_value;
   /* v3: publisher-owned in-flight counter. Held across every callback so the
    * publisher can DRAIN stragglers before freeing the runtime device, which
    * `alive` alone cannot do -- testing it and then calling through hRTDevice
    * leaves a window the device can be freed in. Must mirror
    * umd/compat/aurora_gpu_bridge.h exactly; struct_size is checked. */
   volatile long *in_flight;
} AuroraGpuBridge;

typedef int(__cdecl *PFN_AuroraGpuBridgeAcquire)(AuroraGpuBridge *);

#define AURORA_GPU_RESOURCE_BRIDGE_ABI_VERSION 2u
typedef struct AuroraGpuResourceBridge {
   unsigned int struct_size;
   unsigned int abi_version;
   void *hRTDevice;
   void *hRTResource;
   unsigned int allocation_flags_set;
   unsigned int allocation_flags_clear;
   unsigned long long allocation_reserved;
   unsigned int allocation_info_flags;
   unsigned int vidpn_source_id;
   unsigned int secondary_enabled;
   unsigned int secondary_flags_set;
   unsigned int secondary_flags_clear;
   unsigned int secondary_info_flags;
   unsigned int secondary_vidpn_source_id;
} AuroraGpuResourceBridge;

typedef struct AuroraGpuResourceBridgeResult {
   unsigned int struct_size;
   unsigned int abi_version;
   void *hRTDevice;
   void *hRTResource;
   unsigned int status;
   unsigned int hKMResource;
   unsigned int hAllocation;
   unsigned int secondary_hAllocation;
   unsigned long long allocation_size;
} AuroraGpuResourceBridgeResult;

typedef int(__cdecl *PFN_AuroraGpuResourceBridgeAcquire)(
   AuroraGpuResourceBridge *);
typedef void(__cdecl *PFN_AuroraGpuResourceBridgeComplete)(
   const AuroraGpuResourceBridgeResult *);

static PFN_AuroraGpuResourceBridgeAcquire g_resource_acquire;
static PFN_AuroraGpuResourceBridgeComplete g_resource_complete;

/* -------------------------------------------------------------------------
 * NTSTATUS/HRESULT constants at the translation boundary.
 * ------------------------------------------------------------------------- */
#define AGXW_HRESULT_E_PENDING ((agx_kmt_status)0x8000000A)
#define AGXW_STATUS_DEVICE_REMOVED ((agx_kmt_status)0xC00002B6)
#define AGXW_STATUS_PENDING ((agx_kmt_status)0x00000103)
#define AGXW_STATUS_INVALID_PARAMETER ((agx_kmt_status)0xC000000D)
#define AGXW_STATUS_NOT_IMPLEMENTED ((agx_kmt_status)0xC0000002)
#define AGXW_STATUS_INSUFFICIENT_RESOURCES ((agx_kmt_status)0xC000009A)

/* -------------------------------------------------------------------------
 * Per-process registry + locking. The registry (provider slots + per-provider
 * context tables) is portable core; here we add one lock for slot alloc/free
 * and one lock per slot for that provider's context-token operations. Static
 * zero-initialization makes every SRWLOCK its valid initial value.
 * ------------------------------------------------------------------------- */
static SRWLOCK g_registry_lock = SRWLOCK_INIT;
static struct agxw_bridge_registry g_registry;
static SRWLOCK g_ctx_lock[AGXW_BRIDGE_MAX_DEVICES];
static SRWLOCK g_resource_lock[AGXW_BRIDGE_MAX_DEVICES];

/* -------------------------------------------------------------------------
 * Per-provider BO diagnostics.
 *
 * Only exact 4 MiB handles/fences are retained: that is the failing DXVK
 * chunk measured on hardware. This keeps the diagnostic bounded while still
 * correlating AllocateCb -> MakeResident -> UpdateGpuVA -> fence completion.
 * All callback failures are logged whether or not they concern that size.
 * ------------------------------------------------------------------------- */
#define AGXW_DIAG_TARGET_SIZE UINT64_C(4194304)
#define AGXW_DIAG_TARGET_HANDLES 256u
#define AGXW_DIAG_TARGET_FENCES 256u

struct agxw_bridge_diag_fence {
   uint64_t value;
   uint8_t stage;
};

struct agxw_bridge_diag {
   uint64_t alloc_calls;
   uint64_t alloc_failures;
   uint64_t alloc_bad_output;
   uint64_t alloc_valid_bytes;
   uint64_t alloc_target_calls;
   uint64_t alloc_target_failures;
   uint64_t alloc_refused_no_handoff;
   uint64_t alloc_degraded_no_handoff;
   uint64_t resident_calls;
   uint64_t resident_failures;
   uint64_t resident_pending;
   uint64_t resident_trim_events;
   uint64_t resident_trim_bytes;
   uint64_t resident_target_calls;
   uint64_t update_calls;
   uint64_t update_failures;
   uint64_t update_target_calls;
   uint64_t wait_calls;
   uint64_t wait_failures;
   uint64_t wait_target_calls;
   uint64_t context_create_calls;
   uint64_t context_create_callback_failures;
   uint64_t context_create_token_failures;
   uint64_t context_destroy_calls;
   uint64_t context_destroy_lookup_failures;
   uint64_t context_destroy_callback_failures;
   uint64_t sync_create_calls;
   uint64_t sync_create_failures;
   uint64_t sync_create_bad_output;
   uint64_t sync_destroy_calls;
   uint64_t sync_destroy_failures;
   agx_kmt_handle target_handles[AGXW_DIAG_TARGET_HANDLES];
   struct agxw_bridge_diag_fence target_fences[AGXW_DIAG_TARGET_FENCES];
};

static SRWLOCK g_diag_lock[AGXW_BRIDGE_MAX_DEVICES];
static struct agxw_bridge_diag g_diag[AGXW_BRIDGE_MAX_DEVICES];

static bool
diag_target_handle_locked(const struct agxw_bridge_diag *d,
                          agx_kmt_handle handle)
{
   if (handle == 0)
      return false;
   for (unsigned i = 0; i < AGXW_DIAG_TARGET_HANDLES; ++i)
      if (d->target_handles[i] == handle)
         return true;
   return false;
}

static void
diag_add_target_handle_locked(struct agxw_bridge_diag *d,
                              agx_kmt_handle handle)
{
   if (handle == 0 || diag_target_handle_locked(d, handle))
      return;
   for (unsigned i = 0; i < AGXW_DIAG_TARGET_HANDLES; ++i)
      if (d->target_handles[i] == 0) {
         d->target_handles[i] = handle;
         return;
      }
}

static void
diag_remove_target_handle_locked(struct agxw_bridge_diag *d,
                                 agx_kmt_handle handle)
{
   if (handle == 0)
      return;
   for (unsigned i = 0; i < AGXW_DIAG_TARGET_HANDLES; ++i)
      if (d->target_handles[i] == handle) {
         d->target_handles[i] = 0;
         return;
      }
}

static bool
diag_list_has_target_locked(const struct agxw_bridge_diag *d,
                            const agx_kmt_handle *handles, uint32_t count)
{
   if (handles == NULL)
      return false;
   for (uint32_t i = 0; i < count; ++i)
      if (diag_target_handle_locked(d, handles[i]))
         return true;
   return false;
}

static bool
diag_ops_have_target_locked(const struct agxw_bridge_diag *d,
                            const struct agx_kmt_updategpuva_op *ops,
                            uint32_t count)
{
   if (ops == NULL)
      return false;
   for (uint32_t i = 0; i < count; ++i) {
      if ((ops[i].OperationType == AGX_KMT_UPDATEGPUVA_MAP ||
           ops[i].OperationType == AGX_KMT_UPDATEGPUVA_MAP_PROTECT) &&
          diag_target_handle_locked(d, ops[i].Map.hAllocation))
         return true;
   }
   return false;
}

static void
diag_add_target_fence_locked(struct agxw_bridge_diag *d,
                             enum agx_d3dkmt_bridge_wait_stage stage,
                             uint64_t value)
{
   for (unsigned i = 0; i < AGXW_DIAG_TARGET_FENCES; ++i)
      if (d->target_fences[i].stage == 0) {
         d->target_fences[i].stage = (uint8_t)stage;
         d->target_fences[i].value = value;
         return;
      }
}

static bool
diag_take_target_fence_locked(struct agxw_bridge_diag *d,
                              enum agx_d3dkmt_bridge_wait_stage stage,
                              uint64_t value)
{
   for (unsigned i = 0; i < AGXW_DIAG_TARGET_FENCES; ++i)
      if (d->target_fences[i].stage == (uint8_t)stage &&
          d->target_fences[i].value == value) {
         d->target_fences[i].stage = 0;
         d->target_fences[i].value = 0;
         return true;
      }
   return false;
}

static bool
diag_allocation_private(const struct agx_kmt_allocationinfo2 *info,
                        uint64_t *size, uint32_t *flags)
{
   if (info == NULL || info->pPrivateDriverData == NULL)
      return false;
   const struct agxw_alloc_private *priv =
      (const struct agxw_alloc_private *)info->pPrivateDriverData;
   if (agxw_alloc_private_validate(priv, info->PrivateDriverDataSize) != 0)
      return false;
   *size = priv->size;
   *flags = priv->flags;
   return true;
}

static bool
diag_context_private(const struct agx_kmt_createcontextvirtual *a,
                     uint32_t *priority, uint32_t *flags,
                     uint64_t *usc_exec_base)
{
   if (a == NULL || a->pPrivateDriverData == NULL ||
       a->PrivateDriverDataSize < sizeof(struct agxw_context_private))
      return false;
   const struct agxw_context_private *priv =
      (const struct agxw_context_private *)a->pPrivateDriverData;
   if (priv->version != AGXW_CONTEXT_PRIVATE_VERSION ||
       priv->struct_size < sizeof(*priv) ||
       priv->struct_size > a->PrivateDriverDataSize)
      return false;
   *priority = priv->priority;
   *flags = priv->flags;
   *usc_exec_base = priv->usc_exec_base;
   return true;
}

/* Resolve slot -> provider + its callbacks + device handle, with a guard.
 * cb/hRTDevice are set once at install and constant until teardown, so reading
 * them lock-free is safe; a live device is never torn down while its own thunks
 * run. */
/* True when the publisher revoked this provider's runtime device (the D3D
 * device was destroyed, or the publisher slot was re-let). Logged once per
 * slot: a straggler burst after teardown is one line, not a flood. */
static bool
provider_revoked(unsigned s, const struct agxw_bridge_provider_core *p)
{
   if (p->alive == NULL ||
       *(const volatile LONG *)p->alive == (LONG)p->alive_value)
      return false;
   static LONG revoked_logged[AGXW_BRIDGE_MAX_DEVICES];
   if (InterlockedCompareExchange(&revoked_logged[s], 1, 0) == 0)
      agx_bridge_valog(
         "BRIDGE REVOKED slot=%u rt=%p gen_now=%ld gen_captured=%llu "
         "(runtime device destroyed; degrading calls to DEVICE_REMOVED)",
         s, p->hRTDevice, (long)*(const volatile LONG *)p->alive,
         (unsigned long long)p->alive_value);
   return true;
}

/* Hold the publisher's in-flight counter across a callback.
 *
 * provider_revoked() on its own is a time-of-check/time-of-use bug: it tests
 * the alive cell and the caller THEN calls through hRTDevice, and the runtime
 * device can be freed in that window. Measured as a dwm.exe read of freed
 * memory inside d3d11.dll, called from impl_destroy_allocation2, because DXVK
 * destroys resources on worker threads while the main thread tears the device
 * down.
 *
 * Enter BEFORE testing alive, so the publisher either observes this call and
 * waits for it, or bumps the generation first and the test below refuses. */
static bool
bridge_enter(const struct agxw_bridge_provider_core *p)
{
   if (p->in_flight == NULL)
      return true;
   (void)InterlockedIncrement((volatile LONG *)p->in_flight);
   return true;
}

static void
bridge_leave(const struct agxw_bridge_provider_core *p)
{
   if (p->in_flight != NULL)
      (void)InterlockedDecrement((volatile LONG *)p->in_flight);
}

/* Invoke a runtime callback under the in-flight guard, re-testing revocation
 * once the guard is held. Evaluates to the callback's status. */
#define BRIDGE_INVOKE(slot_, p_, call_)                                        \
   __extension__({                                                             \
      agx_kmt_status _bi_st = AGXW_STATUS_DEVICE_REMOVED;                       \
      bridge_enter((p_));                                                       \
      if (!provider_revoked((slot_), (p_)))                                     \
         _bi_st = (call_);                                                      \
      bridge_leave((p_));                                                       \
      _bi_st;                                                                   \
   })

#define PROVIDER(s)                                                            \
   struct agxw_bridge_provider_core *p = &g_registry.prov[(s)];                \
   const struct agxw_ddi_devicecallbacks *cb =                                 \
      (const struct agxw_ddi_devicecallbacks *)p->cb;                          \
   agxw_ddi_handle dev = (agxw_ddi_handle)p->hRTDevice;                        \
   if (!p->in_use || cb == NULL || dev == NULL)                               \
      return AGXW_STATUS_INVALID_PARAMETER;                                    \
   if (provider_revoked((s), p))                                              \
      return AGXW_STATUS_DEVICE_REMOVED

/* -------------------------------------------------------------------------
 * Device-scope shim implementations, parameterized by provider slot.
 * ------------------------------------------------------------------------- */

static agx_kmt_status
impl_create_paging_queue(unsigned s, struct agx_kmt_createpagingqueue *a)
{
   PROVIDER(s);
   struct agxw_ddi_createpagingqueue c = {
      .Priority = a->Priority,
      .PhysicalAdapterIndex = a->PhysicalAdapterIndex,
   };
   agx_kmt_status st = cb->pfnCreatePagingQueueCb(dev, &c);
   if (AGX_KMT_SUCCESS(st)) {
      a->hPagingQueue = c.hPagingQueue;
      a->hSyncObject = c.hSyncObject;
      a->FenceValueCPUVirtualAddress = c.FenceValueCPUVirtualAddress;
   }
   return st;
}

static agx_kmt_status
impl_destroy_paging_queue(unsigned s, struct agx_kmt_destroypagingqueue *a)
{
   PROVIDER(s);
   struct agxw_ddi_destroypagingqueue c = {.hPagingQueue = a->hPagingQueue};
   return cb->pfnDestroyPagingQueueCb(dev, &c);
}

static agx_kmt_status
impl_create_allocation2(unsigned s, struct agx_kmt_createallocation *a)
{
   PROVIDER(s);
   if (a == NULL)
      return AGXW_STATUS_INVALID_PARAMETER;
   const uint32_t create_flags_allowed =
      AGX_KMT_CREATEALLOC_CREATERESOURCE |
      AGX_KMT_CREATEALLOC_CREATESHARED |
      AGX_KMT_CREATEALLOC_NTSECURITYSHARING |
      AGX_KMT_CREATEALLOC_ALLOWNOTZEROED;
   if ((a->Flags & ~create_flags_allowed) != 0)
      return AGXW_STATUS_INVALID_PARAMETER;
   uint64_t valid_bytes = 0;
   uint32_t valid_count = 0;
   uint32_t target_count = 0;
   uint32_t first_flags = 0;
   if (a != NULL && a->pAllocationInfo2 != NULL) {
      for (uint32_t i = 0; i < a->NumAllocations; ++i) {
         uint64_t size = 0;
         uint32_t flags = 0;
         if (!diag_allocation_private(&a->pAllocationInfo2[i], &size,
                                      &flags))
            continue;
         if (valid_count == 0)
            first_flags = flags;
         valid_count++;
         if (UINT64_MAX - valid_bytes < size)
            valid_bytes = UINT64_MAX;
         else
            valid_bytes += size;
         if (size == AGXW_DIAG_TARGET_SIZE)
            target_count++;
      }
   }
   const bool resource_requested =
      (a->Flags & AGX_KMT_CREATEALLOC_CREATERESOURCE) != 0;
   AuroraGpuResourceBridge handoff = {
      .struct_size = sizeof(handoff),
      .abi_version = AURORA_GPU_RESOURCE_BRIDGE_ABI_VERSION,
      .hRTDevice = dev,
   };
   const bool resource_handoff =
      resource_requested && g_resource_acquire != NULL &&
      g_resource_complete != NULL && g_resource_acquire(&handoff) != 0;
   bool donor_handoff = false;
   uint64_t handoff_allocation_size = 0;

   /* A CreateResource request is legal on the callback path only while it is
    * synchronously nested inside Triton's matching CreateResource.  Without
    * that real hRTResource there is no lifetime owner; never downgrade it to
    * a bare allocation or synthesize a pointer from a KMT handle. */
   /* Export degrade, bridge edition (supersedes the hk-level degrade that
    * starved the handoff -- 0.1.341: 67/67 "native resource handoff failed
    * closed" because degrading in hk_AllocateMemory, ABOVE this function,
    * stripped the CreateResource flags before the pending Triton handoff
    * could ever be claimed). THIS is the one place that knows both facts:
    * the create wants a resource, and no handoff is pending. In bridge mode
    * every device shares one VM, so cross-device export is unnecessary by
    * construction; create the allocation PLAIN (blob rewritten
    * SHAREABLE -> VM_PRIVATE) and let an export that never comes fail at
    * bo_export (resourceless record). The winsys accepts the resourceless
    * shape (degraded-create tolerance in d3dkmt_create_allocation). A
    * PRIMARY without a handoff, or a blob the rewrite cannot make valid,
    * is still REFUSED: those are real contract violations. */
   struct agxw_alloc_private degraded_private;
   bool degraded = false;
   if (resource_requested && !resource_handoff) {
      uint64_t want_bytes = 0;
      uint32_t want_flags = 0;
      bool can_degrade =
         a->NumAllocations == 1 && a->pAllocationInfo2 != NULL &&
         diag_allocation_private(&a->pAllocationInfo2[0], &want_bytes,
                                 &want_flags) &&
         (want_flags & AGXW_ALLOC_PRIMARY) == 0;
      if (can_degrade) {
         memcpy(&degraded_private, a->pAllocationInfo2[0].pPrivateDriverData,
                sizeof(degraded_private));
         degraded_private.flags &=
            ~(uint32_t)(AGXW_ALLOC_SHAREABLE | AGXW_ALLOC_SCANOUT_DONOR |
                        AGXW_ALLOC_ADOPT_EXISTING |
                        AGXW_ALLOC_DIRECT_SCANOUT);
         degraded_private.flags |= AGXW_ALLOC_VM_PRIVATE;
         degraded_private.reserved = 0;
         can_degrade = agxw_alloc_private_validate(
                          &degraded_private, sizeof(degraded_private)) == 0;
      }
      if (!can_degrade) {
         AcquireSRWLockExclusive(&g_diag_lock[s]);
         struct agxw_bridge_diag *rd = &g_diag[s];
         rd->alloc_calls++;
         rd->alloc_failures++;
         rd->alloc_refused_no_handoff++;
         const uint64_t refused_call = rd->alloc_calls;
         ReleaseSRWLockExclusive(&g_diag_lock[s]);
         agx_bridge_valog(
            "BO CreateAllocation2 REFUSED no-handoff slot=%u call=%llu "
            "bytes=%llu blob_flags=0x%08x create_flags=0x%08x allocations=%u "
            "(primary or invalid blob cannot degrade)",
            s, (unsigned long long)refused_call,
            (unsigned long long)want_bytes, want_flags, (unsigned)a->Flags,
            a->NumAllocations);
         return AGXW_STATUS_INVALID_PARAMETER;
      }
      degraded = true;
      AcquireSRWLockExclusive(&g_diag_lock[s]);
      g_diag[s].alloc_degraded_no_handoff++;
      ReleaseSRWLockExclusive(&g_diag_lock[s]);
      static LONG degrade_logged;
      if (InterlockedIncrement(&degrade_logged) <= 8)
         agx_bridge_valog(
            "BO CreateAllocation2 EXPORT-DEGRADED slot=%u bytes=%llu "
            "blob_flags=0x%08x->0x%08x (no handoff pending; plain "
            "VM-private create, export will fail cleanly if redeemed)",
            s, (unsigned long long)want_bytes, want_flags,
            degraded_private.flags);
   }

   struct agxw_alloc_private bridged_private;
   struct agxw_alloc_private secondary_private;
   struct agxw_resource_private atomic_resource_private;
   struct agx_kmt_allocationinfo2 atomic_info[2];
   bool atomic_pair = false;
   void *original_private = NULL;
   uint32_t original_private_size = 0;
   uint32_t original_info_flags = 0;
   uint32_t original_vidpn = 0;
   if (resource_handoff) {
      if (handoff.hRTResource == NULL || a->NumAllocations != 1 ||
          a->pAllocationInfo2 == NULL || valid_count != 1) {
         AuroraGpuResourceBridgeResult result = {
            .struct_size = sizeof(result),
            .abi_version = AURORA_GPU_RESOURCE_BRIDGE_ABI_VERSION,
            .hRTDevice = dev,
            .hRTResource = handoff.hRTResource,
            .status = (unsigned int)AGXW_STATUS_INVALID_PARAMETER,
         };
         g_resource_complete(&result);
         return AGXW_STATUS_INVALID_PARAMETER;
      }
      struct agx_kmt_allocationinfo2 *info = &a->pAllocationInfo2[0];
      original_private = info->pPrivateDriverData;
      original_private_size = info->PrivateDriverDataSize;
      original_info_flags = info->Flags;
      original_vidpn = info->VidPnSourceId;
      memcpy(&bridged_private, original_private, sizeof(bridged_private));
      bridged_private.flags |= handoff.allocation_flags_set;
      bridged_private.flags &= ~handoff.allocation_flags_clear;
      if (handoff.allocation_reserved != 0)
         bridged_private.reserved = handoff.allocation_reserved;
      if (agxw_alloc_private_validate(&bridged_private,
                                      sizeof(bridged_private)) != 0) {
         /* This refusal used to be SILENT, and it hid the 0.1.603 flip-model
          * bug for a full debugging arc: Triton's primary handoff ORed
          * PRIMARY onto a SCANOUT_DONOR-tagged blob, the donor!=primary rule
          * in agxw_alloc_private_validate rejected it right here, the only
          * external evidence was NTSTATUS 0xc000000d in agx_d3dkmt_fail.log,
          * and the consumed handoff turned DXVK's retry into the degraded
          * unshareable surface (Terminal gray interior, no Start menu).
          * Name the exact blob so a contract violation is one grep away.
          * Diagnostic only -- the failure path itself is unchanged. */
         {
            static LONG blob_invalid_logged;
            if (InterlockedIncrement(&blob_invalid_logged) <= 16)
               agx_bridge_valog(
                  "BO CreateAllocation2 HANDOFF-BLOB-INVALID slot=%u "
                  "flags=0x%08x set=0x%08x clear=0x%08x result=0x%08x "
                  "reserved=0x%016llx size=%llu (handoff consumed; caller "
                  "sees INVALID_PARAMETER and any DXVK retry degrades)",
                  s, ((const struct agxw_alloc_private *)original_private)
                        ->flags,
                  handoff.allocation_flags_set, handoff.allocation_flags_clear,
                  bridged_private.flags,
                  (unsigned long long)bridged_private.reserved,
                  (unsigned long long)bridged_private.size);
         }
         AuroraGpuResourceBridgeResult result = {
            .struct_size = sizeof(result),
            .abi_version = AURORA_GPU_RESOURCE_BRIDGE_ABI_VERSION,
            .hRTDevice = dev,
            .hRTResource = handoff.hRTResource,
            .status = (unsigned int)AGXW_STATUS_INVALID_PARAMETER,
         };
         g_resource_complete(&result);
         return AGXW_STATUS_INVALID_PARAMETER;
      }
      donor_handoff =
         (bridged_private.flags & AGXW_ALLOC_SCANOUT_DONOR) != 0;
      if (valid_bytes > UINT64_MAX - AGXW_WDDM_LARGE_PAGE_MASK) {
         AuroraGpuResourceBridgeResult result = {
            .struct_size = sizeof(result),
            .abi_version = AURORA_GPU_RESOURCE_BRIDGE_ABI_VERSION,
            .hRTDevice = dev,
            .hRTResource = handoff.hRTResource,
            .status = (unsigned int)AGXW_STATUS_INVALID_PARAMETER,
         };
         g_resource_complete(&result);
         return AGXW_STATUS_INVALID_PARAMETER;
      }
      handoff_allocation_size =
         (valid_bytes + AGXW_WDDM_LARGE_PAGE_MASK) &
         ~(uint64_t)AGXW_WDDM_LARGE_PAGE_MASK;
      info->pPrivateDriverData = &bridged_private;
      info->PrivateDriverDataSize = sizeof(bridged_private);
      info->Flags |= handoff.allocation_info_flags;
      info->VidPnSourceId = handoff.vidpn_source_id;
      first_flags = bridged_private.flags;

      if (handoff.secondary_enabled != 0) {
         secondary_private = bridged_private;
         secondary_private.flags |= handoff.secondary_flags_set;
         secondary_private.flags &= ~handoff.secondary_flags_clear;
         if (agxw_alloc_private_validate(&secondary_private,
                                         sizeof(secondary_private)) != 0) {
            AuroraGpuResourceBridgeResult result = {
               .struct_size = sizeof(result),
               .abi_version = AURORA_GPU_RESOURCE_BRIDGE_ABI_VERSION,
               .hRTDevice = dev,
               .hRTResource = handoff.hRTResource,
               .status = (unsigned int)AGXW_STATUS_INVALID_PARAMETER,
            };
            g_resource_complete(&result);
            return AGXW_STATUS_INVALID_PARAMETER;
         }
         memset(&atomic_resource_private, 0, sizeof(atomic_resource_private));
         atomic_resource_private.version = AGXW_RESOURCE_PRIVATE_VERSION;
         atomic_resource_private.struct_size = sizeof(atomic_resource_private);
         atomic_resource_private.allocation_count = 2;
         memset(atomic_info, 0, sizeof(atomic_info));
         atomic_info[0] = *info;
         atomic_info[0].pPrivateDriverData = &bridged_private;
         atomic_info[0].PrivateDriverDataSize = sizeof(bridged_private);
         atomic_info[1].pPrivateDriverData = &secondary_private;
         atomic_info[1].PrivateDriverDataSize = sizeof(secondary_private);
         atomic_info[1].Flags = handoff.secondary_info_flags;
         atomic_info[1].VidPnSourceId = handoff.secondary_vidpn_source_id;
         atomic_pair = true;
      }
   }

   void *degraded_original_private = NULL;
   uint32_t degraded_original_size = 0;
   if (degraded) {
      struct agx_kmt_allocationinfo2 *info = &a->pAllocationInfo2[0];
      degraded_original_private = info->pPrivateDriverData;
      degraded_original_size = info->PrivateDriverDataSize;
      info->pPrivateDriverData = &degraded_private;
      info->PrivateDriverDataSize = sizeof(degraded_private);
   }
   struct agxw_ddi_allocate c = {
      /* Compatibility-only nested expansion. Production direct scanout must
       * never enter this branch: 0.1.622 proved dxgkrnl/KMD create both slots
       * but the nested caller receives only slot 0's UMD handle, yielding
       * bad_output=1. Triton now owns the genuine atomic callback and imports
       * its returned donor into Honeykrisp. Keep this ABI shape diagnosable
       * until the post-stability cleanup removes it. */
      .pPrivateDriverData = atomic_pair
                               ? &atomic_resource_private
                               : (resource_handoff && !donor_handoff
                                      ? a->pPrivateDriverData
                                      : NULL),
      .PrivateDriverDataSize = atomic_pair
                                  ? sizeof(atomic_resource_private)
                                  : (resource_handoff && !donor_handoff
                                         ? a->PrivateDriverDataSize
                                         : 0),
      .hResource = resource_handoff ? handoff.hRTResource : NULL,
      .NumAllocations = atomic_pair ? 2u : a->NumAllocations,
      .pAllocationInfo2 = atomic_pair ? atomic_info : a->pAllocationInfo2,
   };
   agx_kmt_status st = cb->pfnAllocateCb(dev, &c);
   if (atomic_pair && a->pAllocationInfo2 != NULL)
      a->pAllocationInfo2[0].hAllocation = atomic_info[0].hAllocation;
   if (degraded) {
      struct agx_kmt_allocationinfo2 *info = &a->pAllocationInfo2[0];
      info->pPrivateDriverData = degraded_original_private;
      info->PrivateDriverDataSize = degraded_original_size;
   }
   if (resource_handoff) {
      struct agx_kmt_allocationinfo2 *info = &a->pAllocationInfo2[0];
      info->pPrivateDriverData = original_private;
      info->PrivateDriverDataSize = original_private_size;
      info->Flags = original_info_flags;
      info->VidPnSourceId = original_vidpn;
   }
   a->hResource = 0;
   a->hGlobalShare = 0;

   bool bad_output = false;
   agx_kmt_handle first_handle = 0;
   agx_kmt_handle secondary_handle = 0;
   if (c.NumAllocations != 0 && c.pAllocationInfo2 == NULL) {
      bad_output = AGX_KMT_SUCCESS(st);
   } else if (c.pAllocationInfo2 != NULL) {
      first_handle = c.NumAllocations ? c.pAllocationInfo2[0].hAllocation : 0;
      secondary_handle = c.NumAllocations > 1
                            ? c.pAllocationInfo2[1].hAllocation
                            : 0;
      if (AGX_KMT_SUCCESS(st)) {
         for (uint32_t i = 0; i < c.NumAllocations; ++i)
            if (c.pAllocationInfo2[i].hAllocation == 0)
               bad_output = true;
      }
   }
   if (resource_handoff && AGX_KMT_SUCCESS(st) && !bad_output) {
      AcquireSRWLockExclusive(&g_resource_lock[s]);
      a->hResource = agxw_bridge_resource_alloc(
         p, handoff.hRTResource, c.hKMResource, first_handle);
      ReleaseSRWLockExclusive(&g_resource_lock[s]);
      if (a->hResource == 0) {
         agx_kmt_handle handles[2] = {first_handle, secondary_handle};
         struct agxw_ddi_deallocate2 deallocate = {
            .NumAllocations = atomic_pair ? 2u : 1u,
            .HandleList = handles,
         };
         (void)BRIDGE_INVOKE(s, p, cb->pfnDeallocate2Cb(dev, &deallocate));
         st = AGXW_STATUS_INSUFFICIENT_RESOURCES;
      }
   }
   if (resource_handoff) {
      AuroraGpuResourceBridgeResult result = {
         .struct_size = sizeof(result),
         .abi_version = AURORA_GPU_RESOURCE_BRIDGE_ABI_VERSION,
         .hRTDevice = dev,
         .hRTResource = handoff.hRTResource,
         .status = (unsigned int)st,
         .hKMResource = c.hKMResource,
         .hAllocation = first_handle,
         .secondary_hAllocation = secondary_handle,
         .allocation_size = handoff_allocation_size,
      };
      g_resource_complete(&result);
   }
   const bool failed = !AGX_KMT_SUCCESS(st) || bad_output;

   AcquireSRWLockExclusive(&g_diag_lock[s]);
   struct agxw_bridge_diag *d = &g_diag[s];
   const uint64_t call = ++d->alloc_calls;
   d->alloc_failures += !AGX_KMT_SUCCESS(st);
   d->alloc_bad_output += bad_output;
   d->alloc_valid_bytes += valid_bytes;
   d->alloc_target_calls += target_count;
   d->alloc_target_failures += failed ? target_count : 0;
   if (!failed && target_count != 0) {
      for (uint32_t i = 0; i < a->NumAllocations; ++i) {
         uint64_t size = 0;
         uint32_t flags = 0;
         if (diag_allocation_private(&a->pAllocationInfo2[i], &size, &flags) &&
             size == AGXW_DIAG_TARGET_SIZE)
            diag_add_target_handle_locked(d,
                                          a->pAllocationInfo2[i].hAllocation);
      }
   }
   ReleaseSRWLockExclusive(&g_diag_lock[s]);

   /* Also log every PRIMARY create, not only failures.  A primary that comes
    * back with hkmresource=0 is the whole desktop bug: with no KM resource the
    * winsys record is resourceless, agx_d3dkmt_bo_export_external refuses,
    * vkGetMemoryWin32HandleKHR fails ("Failed to get shared handle for
    * memory"), no BO is ever tagged AGXW_ALLOC_SCANOUT_DONOR, the KMD never
    * adopts (ADOPT latch=0) and the scanned-out primary stays blank. */
   if (failed || target_count != 0 ||
       (first_flags & AGXW_ALLOC_PRIMARY) != 0 || donor_handoff)
      agx_bridge_valog(
         "BO CreateAllocation2 slot=%u call=%llu allocations=%u valid=%u "
         "bytes=%llu target4m=%u cb_status=0x%08x return_status=0x%08x "
         "halloc0=0x%08x hresource=0x%08x hkmresource=0x%08x hglobal=0x%08x "
         "flags0=0x%08x create_flags=0x%08x resource_handoff=%u "
         "resource_requested=%u degraded=%u donor_handoff=%u "
         "handoff_size=%llu bad_output=%u",
         s, (unsigned long long)call, c.NumAllocations,
         valid_count + (atomic_pair ? 1u : 0u),
         (unsigned long long)valid_bytes, target_count, (unsigned)st,
         (unsigned)st, (unsigned)first_handle, (unsigned)a->hResource,
         (unsigned)c.hKMResource,
         (unsigned)a->hGlobalShare, first_flags, (unsigned)a->Flags,
         resource_handoff ? 1u : 0u, resource_requested ? 1u : 0u,
         degraded ? 1u : 0u, donor_handoff ? 1u : 0u,
         (unsigned long long)handoff_allocation_size,
         bad_output ? 1u : 0u);
   return st;
}

static agx_kmt_status
impl_destroy_allocation2(unsigned s, const struct agx_kmt_destroyallocation2 *a)
{
   PROVIDER(s);
   if (a == NULL)
      return AGXW_STATUS_INVALID_PARAMETER;
   if ((a->Flags & ~AGXW_DDI_DESTROY_UMD_FLAGS_ALL) != 0)
      return AGXW_STATUS_INVALID_PARAMETER;
   if (a->hResource != 0) {
      /* Every resource token returned by the bridge is a borrowed winsys view
       * of a callback-created runtime resource owned by Triton. Honeykrisp
       * releases its texture first; Triton then performs the one authoritative
       * DeallocateCb after all consumers are gone. Calling DeallocateCb here
       * would recursively destroy the same hRTResource while Triton is still
       * releasing it, which d3d11.dll rejects and which strands its allocation. */
      AcquireSRWLockExclusive(&g_resource_lock[s]);
      const struct agxw_bridge_resource_slot *resource =
         agxw_bridge_resource_lookup(p, a->hResource);
      const bool freed = resource != NULL &&
                         agxw_bridge_resource_free(p, a->hResource);
      ReleaseSRWLockExclusive(&g_resource_lock[s]);
      return freed ? 0 : AGXW_STATUS_INVALID_PARAMETER;
   }

   /* Only allocations created without a runtime resource are bridge-owned.
    * Retire those by their exact allocation handles. */
   struct agxw_ddi_deallocate2 c = {
      .hResource = NULL,
      .NumAllocations = a->AllocationCount,
      .HandleList = a->phAllocationList,
      .Flags = a->Flags,
   };
   agx_kmt_status st = BRIDGE_INVOKE(s, p, cb->pfnDeallocate2Cb(dev, &c));
   if (AGX_KMT_SUCCESS(st) && a->phAllocationList != NULL) {
      AcquireSRWLockExclusive(&g_diag_lock[s]);
      for (uint32_t i = 0; i < a->AllocationCount; ++i)
         diag_remove_target_handle_locked(&g_diag[s],
                                          a->phAllocationList[i]);
      ReleaseSRWLockExclusive(&g_diag_lock[s]);
   }
   if (!AGX_KMT_SUCCESS(st))
      agx_bridge_valog(
         "BO DestroyAllocation2 slot=%u allocations=%u status=0x%08x "
         "halloc0=0x%08x resource_token=0x%08x",
         s, a->AllocationCount, (unsigned)st,
         (unsigned)(a->AllocationCount && a->phAllocationList
                       ? a->phAllocationList[0]
                       : 0),
         (unsigned)a->hResource);
   return st;
}

static agx_kmt_status
impl_make_resident(unsigned s, struct agx_kmt_makeresident *a)
{
   PROVIDER(s);
   /* agx_kmt_makeresident IS D3DDDI_MAKERESIDENT: forward unchanged. */
   agx_kmt_status st = cb->pfnMakeResidentCb(dev, a);
   const agx_kmt_status returned =
      st == AGXW_HRESULT_E_PENDING ? AGXW_STATUS_PENDING : st;
   const bool failed = !AGX_KMT_SUCCESS(returned);

   AcquireSRWLockExclusive(&g_diag_lock[s]);
   struct agxw_bridge_diag *d = &g_diag[s];
   const bool target =
      diag_list_has_target_locked(d, a->AllocationList, a->NumAllocations);
   const uint64_t call = ++d->resident_calls;
   d->resident_failures += failed;
   d->resident_pending += st == AGXW_HRESULT_E_PENDING;
   d->resident_trim_events += a->NumBytesToTrim != 0;
   d->resident_trim_bytes += a->NumBytesToTrim;
   d->resident_target_calls += target;
   if (!failed && target)
      diag_add_target_fence_locked(
         d, AGX_D3DKMT_BRIDGE_WAIT_MAKE_RESIDENT, a->PagingFenceValue);
   ReleaseSRWLockExclusive(&g_diag_lock[s]);

   if (failed || target || a->NumBytesToTrim != 0)
      agx_bridge_valog(
         "BO MakeResident slot=%u call=%llu allocations=%u target4m=%u "
         "cb_hresult=0x%08x return_ntstatus=0x%08x fence=%llu trim=%llu "
         "flags=0x%08x halloc0=0x%08x",
         s, (unsigned long long)call, a->NumAllocations, target ? 1u : 0u,
         (unsigned)st, (unsigned)returned,
         (unsigned long long)a->PagingFenceValue,
         (unsigned long long)a->NumBytesToTrim, a->Flags,
         (unsigned)(a->NumAllocations && a->AllocationList
                       ? a->AllocationList[0]
                       : 0));
   return returned; /* E_PENDING remains success-class for the winsys wait */
}

static agx_kmt_status
impl_evict(unsigned s, struct agx_kmt_evict *a)
{
   PROVIDER(s);
   struct agxw_ddi_evict c = {
      .NumAllocations = a->NumAllocations,
      .AllocationList = a->AllocationList,
      .Flags = a->Flags,
   };
   agx_kmt_status st = cb->pfnEvictCb(dev, &c);
   a->NumBytesToTrim = c.NumBytesToTrim;
   return st;
}

static agx_kmt_status
impl_lock2(unsigned s, struct agx_kmt_lock2 *a)
{
   PROVIDER(s);
   struct agxw_ddi_lock2 c = {.hAllocation = a->hAllocation, .Flags = a->Flags};
   agx_kmt_status st = cb->pfnLock2Cb(dev, &c);
   if (AGX_KMT_SUCCESS(st))
      a->pData = c.pData;
   return st;
}

static agx_kmt_status
impl_unlock2(unsigned s, const struct agx_kmt_unlock2 *a)
{
   PROVIDER(s);
   struct agxw_ddi_unlock2 c = {.hAllocation = a->hAllocation};
   return cb->pfnUnlock2Cb(dev, &c);
}

static agx_kmt_status
impl_invalidate_cache(unsigned s, const struct agx_kmt_invalidatecache *a)
{
   PROVIDER(s);
   struct agxw_ddi_invalidatecache c = {
      .hAllocation = a->hAllocation,
      .Offset = (size_t)a->Offset,
      .Length = (size_t)a->Length,
   };
   return cb->pfnInvalidateCacheCb(dev, &c);
}

static agx_kmt_status
impl_reserve_gpuva(unsigned s, struct agx_kmt_reservegpuva *a)
{
   PROVIDER(s);
   /* agx_kmt_reservegpuva IS D3DDDI_RESERVEGPUVIRTUALADDRESS: forward
    * unchanged; its union carries the bridge paging queue and the callback
    * writes VirtualAddress/PagingFenceValue back.
    *
    * EXCEPT for one contract detail the kernel D3DKMT entry tolerates and the
    * runtime callback does not.  BaseAddress and MinimumAddress/MaximumAddress
    * are mutually exclusive: a non-zero BaseAddress means "reserve exactly
    * here", and the range pair must then be zero.  The winsys fills in all
    * three (agx_device_d3dkmt.c d3dkmt_reserve), because honeykrisp's VA
    * layout is not relocatable.  Through pfnReserveGpuVirtualAddressCb that
    * combination is rejected: measured 0.1.360, d3dkmt_reserve returned
    * 0x80004005 fourteen times -- once per failed D3D11CreateDevice -- which
    * is what stopped DWM from ever getting a device.  Normalise it here so the
    * fixed-address request survives the bridge. */
   if (a->BaseAddress != 0) {
      struct agx_kmt_reservegpuva fixed = *a;

      fixed.MinimumAddress = 0;
      fixed.MaximumAddress = 0;
      agx_kmt_status st = cb->pfnReserveGpuVirtualAddressCb(dev, &fixed);
      a->VirtualAddress = fixed.VirtualAddress;
      a->PagingFenceValue = fixed.PagingFenceValue;
      return st;
   }
   return cb->pfnReserveGpuVirtualAddressCb(dev, a);
}

static agx_kmt_status
impl_map_gpuva(unsigned s, struct agx_kmt_mapgpuva *a)
{
   /* The winsys binds via UpdateGpuVirtualAddress, never MapGpuVirtualAddress;
    * present so the table has no NULL slot, but never reached. */
   (void)s;
   (void)a;
   return AGXW_STATUS_NOT_IMPLEMENTED;
}

static agx_kmt_status
impl_free_gpuva(unsigned s, const struct agx_kmt_freegpuva *a)
{
   PROVIDER(s);
   /* Device-scoped callback; the thunk's hAdapter is dropped. */
   struct agxw_ddi_freegpuva c = {.BaseAddress = a->BaseAddress,
                                  .Size = a->Size};
   return cb->pfnFreeGpuVirtualAddressCb(dev, &c);
}

static agx_kmt_status
impl_update_gpuva(unsigned s, const struct agx_kmt_updategpuva *a)
{
   PROVIDER(s);
   AcquireSRWLockExclusive(&g_diag_lock[s]);
   struct agxw_bridge_diag *d = &g_diag[s];
   const bool target =
      diag_ops_have_target_locked(d, a->Operations, a->NumOperations);
   const uint64_t call = ++d->update_calls;
   d->update_target_calls += target;
   ReleaseSRWLockExclusive(&g_diag_lock[s]);

   AcquireSRWLockShared(&g_ctx_lock[s]);
   agxw_ddi_handle hctx =
      (agxw_ddi_handle)agxw_bridge_ctx_lookup(p, a->hContext);
   ReleaseSRWLockShared(&g_ctx_lock[s]);
   if (hctx == NULL) {
      AcquireSRWLockExclusive(&g_diag_lock[s]);
      g_diag[s].update_failures++;
      ReleaseSRWLockExclusive(&g_diag_lock[s]);
      agx_bridge_valog(
         "BO UpdateGpuVA slot=%u call=%llu ops=%u target4m=%u "
         "status=0x%08x fence=%llu flags=0x%08x context_token=0x%08x "
         "context=NULL reason=context-token-lookup",
         s, (unsigned long long)call, a->NumOperations, target ? 1u : 0u,
         (unsigned)AGXW_STATUS_INVALID_PARAMETER,
         (unsigned long long)a->FenceValue, a->Flags, (unsigned)a->hContext);
      return AGXW_STATUS_INVALID_PARAMETER;
   }

   struct agxw_ddi_updategpuva c = {
      .hContext = hctx,
      .hFenceObject = a->hFenceObject,
      .NumOperations = a->NumOperations,
      .Operations = a->Operations,
      .Reserved0 = a->Reserved0,
      .Reserved1 = a->Reserved1,
      .FenceValue = a->FenceValue,
      .Flags = a->Flags,
   };
   agx_kmt_status st = cb->pfnUpdateGpuVirtualAddressCb(dev, &c);
   const bool failed = !AGX_KMT_SUCCESS(st);

   AcquireSRWLockExclusive(&g_diag_lock[s]);
   d->update_failures += failed;
   /* The winsys waits the completion value immediately following the value
    * submitted to UpdateGpuVirtualAddress. */
   if (!failed && target && a->FenceValue != UINT64_MAX)
      diag_add_target_fence_locked(
         d, AGX_D3DKMT_BRIDGE_WAIT_UPDATE_GPUVA, a->FenceValue + 1);
   ReleaseSRWLockExclusive(&g_diag_lock[s]);

   const struct agx_kmt_updategpuva_op *op0 =
      a->NumOperations && a->Operations ? &a->Operations[0] : NULL;
   const bool op0_maps =
      op0 && (op0->OperationType == AGX_KMT_UPDATEGPUVA_MAP ||
              op0->OperationType == AGX_KMT_UPDATEGPUVA_MAP_PROTECT);
   if (failed || target)
      agx_bridge_valog(
         "BO UpdateGpuVA slot=%u call=%llu ops=%u target4m=%u "
         "status=0x%08x fence=%llu flags=0x%08x context_token=0x%08x "
         "context=%p op0_type=%d op0_alloc=0x%08x op0_base=0x%llx "
         "op0_size=0x%llx",
         s, (unsigned long long)call, a->NumOperations, target ? 1u : 0u,
         (unsigned)st, (unsigned long long)a->FenceValue, a->Flags,
         (unsigned)a->hContext, hctx, op0 ? op0->OperationType : -1,
         (unsigned)(op0_maps ? op0->Map.hAllocation : 0),
         (unsigned long long)(op0_maps ? op0->Map.BaseAddress : 0),
         (unsigned long long)(op0_maps ? op0->Map.SizeInBytes : 0));
   return st;
}

static agx_kmt_status
impl_create_context_virtual(unsigned s, struct agx_kmt_createcontextvirtual *a)
{
   PROVIDER(s);
   uint32_t priority = 0;
   uint32_t private_flags = 0;
   uint64_t usc_exec_base = 0;
   const bool private_valid = diag_context_private(
      a, &priority, &private_flags, &usc_exec_base);
   struct agxw_ddi_createcontextvirtual c = {
      .NodeOrdinal = a->NodeOrdinal,
      .EngineAffinity = a->EngineAffinity,
      .Flags = a->Flags,
      .pPrivateDriverData = a->pPrivateDriverData,
      .PrivateDriverDataSize = a->PrivateDriverDataSize,
   };
   agx_kmt_status st = cb->pfnCreateContextVirtualCb(dev, &c);
   AcquireSRWLockExclusive(&g_diag_lock[s]);
   struct agxw_bridge_diag *diag = &g_diag[s];
   const uint64_t call = ++diag->context_create_calls;
   diag->context_create_callback_failures += !AGX_KMT_SUCCESS(st);
   ReleaseSRWLockExclusive(&g_diag_lock[s]);
   if (!AGX_KMT_SUCCESS(st)) {
      agx_bridge_valog(
         "QUEUE CreateContextVirtual slot=%u call=%llu stage=callback "
         "status=0x%08x node=%u affinity=0x%08x ddi_flags=0x%08x "
         "private_valid=%u priority=%u private_flags=0x%08x usc=0x%llx "
         "context=%p token=0x%08x",
         s, (unsigned long long)call, (unsigned)st, a->NodeOrdinal,
         a->EngineAffinity, a->Flags, private_valid ? 1u : 0u, priority,
         private_flags, (unsigned long long)usc_exec_base, c.hContext, 0u);
      return st;
   }

   AcquireSRWLockExclusive(&g_ctx_lock[s]);
   uint32_t token = agxw_bridge_ctx_alloc(p, c.hContext);
   ReleaseSRWLockExclusive(&g_ctx_lock[s]);
   if (token == 0) {
      struct agxw_ddi_destroycontext d = {.hContext = c.hContext};
      agx_kmt_status cleanup_st = cb->pfnDestroyContextCb(dev, &d);
      AcquireSRWLockExclusive(&g_diag_lock[s]);
      g_diag[s].context_create_token_failures++;
      g_diag[s].context_destroy_calls++;
      g_diag[s].context_destroy_callback_failures +=
         !AGX_KMT_SUCCESS(cleanup_st);
      ReleaseSRWLockExclusive(&g_diag_lock[s]);
      agx_bridge_valog(
         "QUEUE CreateContextVirtual slot=%u call=%llu stage=token-mint "
         "status=0x%08x cleanup_status=0x%08x node=%u affinity=0x%08x "
         "ddi_flags=0x%08x private_valid=%u priority=%u "
         "private_flags=0x%08x usc=0x%llx context=%p token=0x%08x",
         s, (unsigned long long)call,
         (unsigned)AGXW_STATUS_INSUFFICIENT_RESOURCES, (unsigned)cleanup_st,
         a->NodeOrdinal, a->EngineAffinity, a->Flags,
         private_valid ? 1u : 0u, priority, private_flags,
         (unsigned long long)usc_exec_base, c.hContext, 0u);
      return AGXW_STATUS_INSUFFICIENT_RESOURCES;
   }
   a->hContext = token;
   agx_bridge_valog(
      "QUEUE CreateContextVirtual slot=%u call=%llu stage=complete "
      "status=0x%08x node=%u affinity=0x%08x ddi_flags=0x%08x "
      "private_valid=%u priority=%u private_flags=0x%08x usc=0x%llx "
      "context=%p token=0x%08x",
      s, (unsigned long long)call, (unsigned)st, a->NodeOrdinal,
      a->EngineAffinity, a->Flags, private_valid ? 1u : 0u, priority,
      private_flags, (unsigned long long)usc_exec_base, c.hContext, token);
   return st;
}

static agx_kmt_status
impl_destroy_context(unsigned s, const struct agx_kmt_destroycontext *a)
{
   PROVIDER(s);
   AcquireSRWLockExclusive(&g_diag_lock[s]);
   const uint64_t call = ++g_diag[s].context_destroy_calls;
   ReleaseSRWLockExclusive(&g_diag_lock[s]);
   AcquireSRWLockShared(&g_ctx_lock[s]);
   agxw_ddi_handle hctx =
      (agxw_ddi_handle)agxw_bridge_ctx_lookup(p, a->hContext);
   ReleaseSRWLockShared(&g_ctx_lock[s]);
   if (hctx == NULL) {
      AcquireSRWLockExclusive(&g_diag_lock[s]);
      g_diag[s].context_destroy_lookup_failures++;
      ReleaseSRWLockExclusive(&g_diag_lock[s]);
      agx_bridge_valog(
         "QUEUE DestroyContext slot=%u call=%llu stage=token-lookup "
         "status=0x%08x token=0x%08x context=NULL",
         s, (unsigned long long)call, (unsigned)AGXW_STATUS_INVALID_PARAMETER,
         (unsigned)a->hContext);
      return AGXW_STATUS_INVALID_PARAMETER;
   }

   struct agxw_ddi_destroycontext c = {.hContext = hctx};
   agx_kmt_status st = cb->pfnDestroyContextCb(dev, &c);
   AcquireSRWLockExclusive(&g_diag_lock[s]);
   g_diag[s].context_destroy_callback_failures += !AGX_KMT_SUCCESS(st);
   ReleaseSRWLockExclusive(&g_diag_lock[s]);
   AcquireSRWLockExclusive(&g_ctx_lock[s]);
   agxw_bridge_ctx_free(p, a->hContext);
   ReleaseSRWLockExclusive(&g_ctx_lock[s]);
   agx_bridge_valog(
      "QUEUE DestroyContext slot=%u call=%llu stage=callback "
      "status=0x%08x token=0x%08x context=%p",
      s, (unsigned long long)call, (unsigned)st, (unsigned)a->hContext, hctx);
   return st;
}

static agx_kmt_status
impl_submit_command(unsigned s, const struct agx_kmt_submitcommand *a)
{
   PROVIDER(s);
   if (a->BroadcastContextCount == 0 ||
       a->BroadcastContextCount > AGX_KMT_MAX_BROADCAST_CONTEXT)
      return AGXW_STATUS_INVALID_PARAMETER;

   struct agxw_ddi_submitcommand c = {
      .Commands = a->Commands,
      .CommandLength = a->CommandLength,
      .Flags = a->Flags,
      .BroadcastContextCount = a->BroadcastContextCount,
      .pPrivateDriverData = a->pPrivateDriverData,
      .PrivateDriverDataSize = a->PrivateDriverDataSize,
      .NumPrimaries = a->NumPrimaries,
      .NumHistoryBuffers = a->NumHistoryBuffers,
      .HistoryBufferArray = a->HistoryBufferArray,
   };
   AcquireSRWLockShared(&g_ctx_lock[s]);
   for (uint32_t i = 0; i < a->BroadcastContextCount; ++i) {
      agxw_ddi_handle hctx =
         (agxw_ddi_handle)agxw_bridge_ctx_lookup(p, a->BroadcastContext[i]);
      if (hctx == NULL) {
         ReleaseSRWLockShared(&g_ctx_lock[s]);
         return AGXW_STATUS_INVALID_PARAMETER;
      }
      c.BroadcastContext[i] = hctx;
   }
   ReleaseSRWLockShared(&g_ctx_lock[s]);

   for (uint32_t i = 0;
        i < a->NumPrimaries && i < AGX_KMT_MAX_WRITTEN_PRIMARIES; ++i)
      c.WrittenPrimaries[i] = a->WrittenPrimaries[i];

   return cb->pfnSubmitCommandCb(dev, &c);
}

static agx_kmt_status
impl_create_syncobj2(unsigned s, struct agx_kmt_createsyncobj2 *a)
{
   PROVIDER(s);
   struct agxw_ddi_createsyncobj2 c = {.Info = a->Info};
   agx_kmt_status st = cb->pfnCreateSynchronizationObject2Cb(dev, &c);
   if (AGX_KMT_SUCCESS(st)) {
      a->Info = c.Info; /* carries FenceValueCPU/GPUVirtualAddress out */
      a->hSyncObject = c.hSyncObject;
   }
   const bool bad_output = AGX_KMT_SUCCESS(st) && c.hSyncObject == 0;
   AcquireSRWLockExclusive(&g_diag_lock[s]);
   struct agxw_bridge_diag *diag = &g_diag[s];
   const uint64_t call = ++diag->sync_create_calls;
   diag->sync_create_failures += !AGX_KMT_SUCCESS(st);
   diag->sync_create_bad_output += bad_output;
   ReleaseSRWLockExclusive(&g_diag_lock[s]);
   agx_bridge_valog(
      "QUEUE CreateSyncObject slot=%u call=%llu status=0x%08x type=%d "
      "flags=0x%08x sync=0x%08x cpu=%p gpu=0x%llx initial=%llu "
      "affinity=0x%08x bad_output=%u",
      s, (unsigned long long)call, (unsigned)st, a->Info.Type, a->Info.Flags,
      (unsigned)c.hSyncObject,
      c.Info.Type == AGX_KMT_SYNC_MONITORED_FENCE
         ? c.Info.MonitoredFence.FenceValueCPUVirtualAddress
         : NULL,
      (unsigned long long)(c.Info.Type == AGX_KMT_SYNC_MONITORED_FENCE
                              ? c.Info.MonitoredFence.FenceValueGPUVirtualAddress
                              : 0),
      (unsigned long long)(c.Info.Type == AGX_KMT_SYNC_MONITORED_FENCE
                              ? c.Info.MonitoredFence.InitialFenceValue
                              : 0),
      c.Info.Type == AGX_KMT_SYNC_MONITORED_FENCE
         ? c.Info.MonitoredFence.EngineAffinity
         : 0,
      bad_output ? 1u : 0u);
   return st;
}

static agx_kmt_status
impl_destroy_syncobj(unsigned s, const struct agx_kmt_destroysyncobj *a)
{
   PROVIDER(s);
   struct agxw_ddi_destroysyncobj c = {.hSyncObject = a->hSyncObject};
   agx_kmt_status st = cb->pfnDestroySynchronizationObjectCb(dev, &c);
   AcquireSRWLockExclusive(&g_diag_lock[s]);
   struct agxw_bridge_diag *diag = &g_diag[s];
   const uint64_t call = ++diag->sync_destroy_calls;
   diag->sync_destroy_failures += !AGX_KMT_SUCCESS(st);
   ReleaseSRWLockExclusive(&g_diag_lock[s]);
   agx_bridge_valog(
      "QUEUE DestroySyncObject slot=%u call=%llu status=0x%08x sync=0x%08x",
      s, (unsigned long long)call, (unsigned)st, (unsigned)a->hSyncObject);
   return st;
}

static agx_kmt_status
impl_signal_from_cpu(unsigned s, const struct agx_kmt_signalsyncfromcpu *a)
{
   PROVIDER(s);
   const agx_kmt_handle first_handle =
      a->ObjectCount && a->ObjectHandleArray ? a->ObjectHandleArray[0] : 0u;
   const uint64_t first_value =
      a->ObjectCount && a->FenceValueArray ? a->FenceValueArray[0] : 0u;
   agx_bridge_valog(
      "QUEUE SignalSyncCpu slot=%u stage=entry count=%u first_sync=0x%08x first_value=%llu",
      s, a->ObjectCount, first_handle, (unsigned long long)first_value);
   struct agxw_ddi_signalsyncfromcpu c = {
      .ObjectCount = a->ObjectCount,
      .ObjectHandleArray = a->ObjectHandleArray,
      .FenceValueArray = a->FenceValueArray,
   };
   agx_kmt_status st = cb->pfnSignalSynchronizationObjectFromCpuCb(dev, &c);
   agx_bridge_valog(
      "QUEUE SignalSyncCpu slot=%u stage=return status=0x%08x count=%u first_sync=0x%08x first_value=%llu",
      s, (unsigned)st, a->ObjectCount, first_handle,
      (unsigned long long)first_value);
   return st;
}

static agx_kmt_status
impl_wait_from_cpu(unsigned s, const struct agx_kmt_waitsyncfromcpu *a)
{
   PROVIDER(s);
   const agx_kmt_handle first_handle =
      a->ObjectCount && a->ObjectHandleArray ? a->ObjectHandleArray[0] : 0u;
   const uint64_t first_value =
      a->ObjectCount && a->FenceValueArray ? a->FenceValueArray[0] : 0u;
   agx_bridge_valog(
      "QUEUE WaitSyncCpu slot=%u stage=entry count=%u first_sync=0x%08x first_value=%llu event=%p flags=0x%08x",
      s, a->ObjectCount, first_handle, (unsigned long long)first_value,
      a->hAsyncEvent, a->Flags);
   struct agxw_ddi_waitsyncfromcpu c = {
      .ObjectCount = a->ObjectCount,
      .ObjectHandleArray = a->ObjectHandleArray,
      .FenceValueArray = a->FenceValueArray,
      .hAsyncEvent = a->hAsyncEvent,
      .Flags = a->Flags,
   };
   agx_kmt_status st = cb->pfnWaitForSynchronizationObjectFromCpuCb(dev, &c);
   agx_bridge_valog(
      "QUEUE WaitSyncCpu slot=%u stage=return status=0x%08x count=%u first_sync=0x%08x first_value=%llu",
      s, (unsigned)st, a->ObjectCount, first_handle,
      (unsigned long long)first_value);
   return st;
}

static agx_kmt_status
impl_signal_from_gpu(unsigned s, const struct agx_kmt_signalsyncfromgpu *a)
{
   PROVIDER(s);
   AcquireSRWLockShared(&g_ctx_lock[s]);
   agxw_ddi_handle hctx =
      (agxw_ddi_handle)agxw_bridge_ctx_lookup(p, a->hContext);
   ReleaseSRWLockShared(&g_ctx_lock[s]);
   if (hctx == NULL)
      return AGXW_STATUS_INVALID_PARAMETER;

   struct agxw_ddi_signalsyncfromgpu c = {
      .hContext = hctx,
      .ObjectCount = a->ObjectCount,
      .ObjectHandleArray = a->ObjectHandleArray,
      .MonitoredFenceValueArray = a->MonitoredFenceValueArray,
   };
   const agx_kmt_handle first_handle =
      a->ObjectCount && a->ObjectHandleArray ? a->ObjectHandleArray[0] : 0u;
   const uint64_t first_value =
      a->ObjectCount && a->MonitoredFenceValueArray
         ? a->MonitoredFenceValueArray[0] : 0u;
   agx_bridge_valog(
      "QUEUE SignalSyncGpu slot=%u stage=entry context=0x%08x count=%u first_sync=0x%08x first_value=%llu",
      s, (unsigned)(uintptr_t)hctx, a->ObjectCount, first_handle,
      (unsigned long long)first_value);
   agx_kmt_status st = cb->pfnSignalSynchronizationObjectFromGpuCb(dev, &c);
   agx_bridge_valog(
      "QUEUE SignalSyncGpu slot=%u stage=return status=0x%08x context=0x%08x count=%u first_sync=0x%08x first_value=%llu",
      s, (unsigned)st, (unsigned)(uintptr_t)hctx, a->ObjectCount,
      first_handle, (unsigned long long)first_value);
   return st;
}

static agx_kmt_status
impl_wait_from_gpu(unsigned s, const struct agx_kmt_waitsyncfromgpu *a)
{
   PROVIDER(s);
   AcquireSRWLockShared(&g_ctx_lock[s]);
   agxw_ddi_handle hctx =
      (agxw_ddi_handle)agxw_bridge_ctx_lookup(p, a->hContext);
   ReleaseSRWLockShared(&g_ctx_lock[s]);
   if (hctx == NULL)
      return AGXW_STATUS_INVALID_PARAMETER;

   struct agxw_ddi_waitsyncfromgpu c = {
      .hContext = hctx,
      .ObjectCount = a->ObjectCount,
      .ObjectHandleArray = a->ObjectHandleArray,
      .MonitoredFenceValueArray = a->MonitoredFenceValueArray,
   };
   return cb->pfnWaitForSynchronizationObjectFromGpuCb(dev, &c);
}

/* -------------------------------------------------------------------------
 * Per-slot thunk pool. GEN(N) defines one distinct forwarding function per
 * device-scope op, each binding the constant slot N; INST(N) installs slot N's
 * thunks into an agx_d3dkmt_fns. BRIDGE_APPLY lists every slot 0..MAX-1.
 * ------------------------------------------------------------------------- */
#define GEN(N)                                                                 \
   static agx_kmt_status AGX_KMT_API bt_cpq_##N(                               \
      struct agx_kmt_createpagingqueue *a)                                     \
   {                                                                           \
      return impl_create_paging_queue((N), a);                                 \
   }                                                                           \
   static agx_kmt_status AGX_KMT_API bt_dpq_##N(                               \
      struct agx_kmt_destroypagingqueue *a)                                    \
   {                                                                           \
      return impl_destroy_paging_queue((N), a);                                \
   }                                                                           \
   static agx_kmt_status AGX_KMT_API bt_ca2_##N(                               \
      struct agx_kmt_createallocation *a)                                      \
   {                                                                           \
      return impl_create_allocation2((N), a);                                  \
   }                                                                           \
   static agx_kmt_status AGX_KMT_API bt_da2_##N(                               \
      const struct agx_kmt_destroyallocation2 *a)                              \
   {                                                                           \
      return impl_destroy_allocation2((N), a);                                 \
   }                                                                           \
   static agx_kmt_status AGX_KMT_API bt_mr_##N(struct agx_kmt_makeresident *a) \
   {                                                                           \
      return impl_make_resident((N), a);                                       \
   }                                                                           \
   static agx_kmt_status AGX_KMT_API bt_ev_##N(struct agx_kmt_evict *a)        \
   {                                                                           \
      return impl_evict((N), a);                                               \
   }                                                                           \
   static agx_kmt_status AGX_KMT_API bt_l2_##N(struct agx_kmt_lock2 *a)        \
   {                                                                           \
      return impl_lock2((N), a);                                               \
   }                                                                           \
   static agx_kmt_status AGX_KMT_API bt_u2_##N(                               \
      const struct agx_kmt_unlock2 *a)                                         \
   {                                                                           \
      return impl_unlock2((N), a);                                             \
   }                                                                           \
   static agx_kmt_status AGX_KMT_API bt_ic_##N(                               \
      const struct agx_kmt_invalidatecache *a)                                 \
   {                                                                           \
      return impl_invalidate_cache((N), a);                                    \
   }                                                                           \
   static agx_kmt_status AGX_KMT_API bt_rva_##N(                              \
      struct agx_kmt_reservegpuva *a)                                          \
   {                                                                           \
      return impl_reserve_gpuva((N), a);                                       \
   }                                                                           \
   static agx_kmt_status AGX_KMT_API bt_mva_##N(struct agx_kmt_mapgpuva *a)    \
   {                                                                           \
      return impl_map_gpuva((N), a);                                           \
   }                                                                           \
   static agx_kmt_status AGX_KMT_API bt_fva_##N(                              \
      const struct agx_kmt_freegpuva *a)                                       \
   {                                                                           \
      return impl_free_gpuva((N), a);                                          \
   }                                                                           \
   static agx_kmt_status AGX_KMT_API bt_uva_##N(                              \
      const struct agx_kmt_updategpuva *a)                                     \
   {                                                                           \
      return impl_update_gpuva((N), a);                                        \
   }                                                                           \
   static agx_kmt_status AGX_KMT_API bt_ccv_##N(                              \
      struct agx_kmt_createcontextvirtual *a)                                  \
   {                                                                           \
      return impl_create_context_virtual((N), a);                             \
   }                                                                           \
   static agx_kmt_status AGX_KMT_API bt_dc_##N(                               \
      const struct agx_kmt_destroycontext *a)                                  \
   {                                                                           \
      return impl_destroy_context((N), a);                                     \
   }                                                                           \
   static agx_kmt_status AGX_KMT_API bt_sc_##N(                               \
      const struct agx_kmt_submitcommand *a)                                   \
   {                                                                           \
      return impl_submit_command((N), a);                                      \
   }                                                                           \
   static agx_kmt_status AGX_KMT_API bt_cso2_##N(                             \
      struct agx_kmt_createsyncobj2 *a)                                        \
   {                                                                           \
      return impl_create_syncobj2((N), a);                                     \
   }                                                                           \
   static agx_kmt_status AGX_KMT_API bt_dso_##N(                              \
      const struct agx_kmt_destroysyncobj *a)                                  \
   {                                                                           \
      return impl_destroy_syncobj((N), a);                                     \
   }                                                                           \
   static agx_kmt_status AGX_KMT_API bt_scpu_##N(                             \
      const struct agx_kmt_signalsyncfromcpu *a)                              \
   {                                                                           \
      return impl_signal_from_cpu((N), a);                                     \
   }                                                                           \
   static agx_kmt_status AGX_KMT_API bt_wcpu_##N(                             \
      const struct agx_kmt_waitsyncfromcpu *a)                                \
   {                                                                           \
      return impl_wait_from_cpu((N), a);                                       \
   }                                                                           \
   static agx_kmt_status AGX_KMT_API bt_sgpu_##N(                             \
      const struct agx_kmt_signalsyncfromgpu *a)                              \
   {                                                                           \
      return impl_signal_from_gpu((N), a);                                     \
   }                                                                           \
   static agx_kmt_status AGX_KMT_API bt_wgpu_##N(                             \
      const struct agx_kmt_waitsyncfromgpu *a)                                \
   {                                                                           \
      return impl_wait_from_gpu((N), a);                                       \
   }

#define INST(N)                                                                \
   case (N):                                                                   \
      f->CreatePagingQueue = bt_cpq_##N;                                       \
      f->DestroyPagingQueue = bt_dpq_##N;                                      \
      f->CreateAllocation2 = bt_ca2_##N;                                       \
      f->DestroyAllocation2 = bt_da2_##N;                                      \
      f->MakeResident = bt_mr_##N;                                             \
      f->Evict = bt_ev_##N;                                                    \
      f->Lock2 = bt_l2_##N;                                                    \
      f->Unlock2 = bt_u2_##N;                                                  \
      f->InvalidateCache = bt_ic_##N;                                          \
      f->ReserveGpuVirtualAddress = bt_rva_##N;                                \
      f->MapGpuVirtualAddress = bt_mva_##N;                                    \
      f->FreeGpuVirtualAddress = bt_fva_##N;                                   \
      f->UpdateGpuVirtualAddress = bt_uva_##N;                                 \
      f->CreateContextVirtual = bt_ccv_##N;                                    \
      f->DestroyContext = bt_dc_##N;                                           \
      f->SubmitCommand = bt_sc_##N;                                            \
      f->CreateSynchronizationObject2 = bt_cso2_##N;                           \
      f->DestroySynchronizationObject = bt_dso_##N;                           \
      f->SignalSynchronizationObjectFromCpu = bt_scpu_##N;                     \
      f->WaitForSynchronizationObjectFromCpu = bt_wcpu_##N;                    \
      f->SignalSynchronizationObjectFromGpu = bt_sgpu_##N;                     \
      f->WaitForSynchronizationObjectFromGpu = bt_wgpu_##N;                    \
      break;

/* Every slot 0..AGXW_BRIDGE_MAX_DEVICES-1. Keep this list's length equal to
 * AGXW_BRIDGE_MAX_DEVICES (asserted below). */
#define BRIDGE_APPLY(M)                                                        \
   M(0) M(1) M(2) M(3) M(4) M(5) M(6) M(7)                                     \
   M(8) M(9) M(10) M(11) M(12) M(13) M(14) M(15)                              \
   M(16) M(17) M(18) M(19) M(20) M(21) M(22) M(23)                            \
   M(24) M(25) M(26) M(27) M(28) M(29) M(30) M(31)                            \
   M(32) M(33) M(34) M(35) M(36) M(37) M(38) M(39)                            \
   M(40) M(41) M(42) M(43) M(44) M(45) M(46) M(47)                            \
   M(48) M(49) M(50) M(51) M(52) M(53) M(54) M(55)                            \
   M(56) M(57) M(58) M(59) M(60) M(61) M(62) M(63)

_Static_assert(AGXW_BRIDGE_MAX_DEVICES == 64,
               "BRIDGE_APPLY lists exactly AGXW_BRIDGE_MAX_DEVICES slots");

BRIDGE_APPLY(GEN)

static void
install_slot(struct agx_d3dkmt_fns *f, unsigned slot)
{
   switch (slot) {
      BRIDGE_APPLY(INST)
   default:
      break;
   }
}

/* -------------------------------------------------------------------------
 * Enable / install / uninstall.
 * ------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------
 * Backing-pool availability, for an HONEST Vulkan heap budget.
 *
 * Without this the ICD reports a synthetic per-process number and every value
 * is wrong: too low and the compositor's scanout primary is refused outright;
 * too high and DXVK's chunk cache grows into the shared carveout until the
 * SECOND full-screen primary cannot be backed -- measured on hardware as
 * leased 59839/65536 with DWM's 2nd 3024x1964 create failing E_INVALIDARG,
 * after which DWM destroys all 19 resources and its device and never presents.
 *
 * AGXW_ESC_GET_POOL is a pure device-property read (it allocates, binds and
 * synchronises nothing), issued adapter-scoped with hDevice = 0 so it works in
 * bridge mode where ws->device is a sentinel.  Throttled, because DXVK
 * re-queries the budget frequently and this crosses into the kernel.
 * ------------------------------------------------------------------------- */
uint64_t
agx_d3dkmt_pool_free_bytes(struct agx_device *dev)
{
   static ULONGLONG cache_deadline;
   static uint64_t cache_value;
   struct agx_d3dkmt_winsys *ws;
   struct {
      struct agxw_escape_header header;
      struct agxw_escape_pool pool;
   } blob;
   struct agx_kmt_escape escape;
   ULONGLONG now;

   if (dev == NULL)
      return 0;
   now = GetTickCount64();
   if (cache_deadline != 0 && now < cache_deadline)
      return cache_value;

   ws = agx_d3dkmt_ws(dev);
   if (ws == NULL || ws->fns.Escape == NULL || ws->adapter == 0)
      return 0;

   memset(&blob, 0, sizeof(blob));
   blob.header.magic = AGXW_ESCAPE_MAGIC;
   blob.header.version = AGXW_ESCAPE_VERSION;
   blob.header.op = AGXW_ESC_GET_POOL;
   blob.header.payload_size = (uint32_t)sizeof(blob.pool);

   memset(&escape, 0, sizeof(escape));
   escape.hAdapter = ws->adapter;
   escape.hDevice = 0; /* adapter-scoped: ws->device is a bridge sentinel */
   escape.pPrivateDriverData = &blob;
   escape.PrivateDriverDataSize = (uint32_t)sizeof(blob);

   if (!AGX_KMT_SUCCESS(ws->fns.Escape(&escape)) || blob.header.status != 0) {
      /* Fail soft: caller keeps its previous (synthetic) budget. Re-probe
       * after the throttle so a late BackingPoolReady is picked up. */
      cache_deadline = now + 500ull;
      cache_value = 0;
      return 0;
   }

   cache_value = ((uint64_t)blob.pool.cached_free_pages +
                  (uint64_t)blob.pool.noncached_free_pages) *
                 (uint64_t)blob.pool.page_size;
   cache_deadline = now + 200ull;
   return cache_value;
}

bool
agx_d3dkmt_bridge_enabled(void)
{
   const char *v = getenv("HONEYKRISP_RT_DEVICE");
   return v != NULL && v[0] != '\0' && v[0] != '0';
}

bool
agx_d3dkmt_bridge_install(struct agx_d3dkmt_winsys *ws)
{
   HMODULE umd = GetModuleHandleA(AURORA_GPU_BRIDGE_MODULE);
   if (umd == NULL) {
      fprintf(stderr, "agx_d3dkmt_bridge: %s not loaded in this process\n",
              AURORA_GPU_BRIDGE_MODULE);
      return false;
   }

   PFN_AuroraGpuBridgeAcquire acquire_current =
      (PFN_AuroraGpuBridgeAcquire)(void *)GetProcAddress(
         umd, "AuroraGpuBridgeAcquireCurrentThread");
   PFN_AuroraGpuBridgeAcquire acquire_single =
      (PFN_AuroraGpuBridgeAcquire)(void *)GetProcAddress(
         umd, "AuroraGpuBridgeAcquireSingleActive");
   PFN_AuroraGpuResourceBridgeAcquire resource_acquire =
      (PFN_AuroraGpuResourceBridgeAcquire)(void *)GetProcAddress(
         umd, "AuroraGpuResourceBridgeAcquireCurrentThread");
   PFN_AuroraGpuResourceBridgeComplete resource_complete =
      (PFN_AuroraGpuResourceBridgeComplete)(void *)GetProcAddress(
         umd, "AuroraGpuResourceBridgeCompleteCurrentThread");

   AuroraGpuBridge b = {
      .struct_size = sizeof(b),
      .abi_version = AURORA_GPU_BRIDGE_ABI_VERSION,
   };
   /* Current-thread FIRST: with several devices live, this binds to the D3D
    * device whose vkCreateDevice is running on this thread. Single-active is a
    * last resort for the lone-device case and safely returns 0 when ambiguous. */
   int ok = (acquire_current != NULL && acquire_current(&b)) ||
            (acquire_single != NULL && acquire_single(&b));
   if (!ok || b.callbacks == NULL || b.hRTDevice == NULL ||
       b.alive == NULL) {
      fprintf(stderr, "agx_d3dkmt_bridge: no runtime bridge published for this "
                      "thread (ABI/version mismatch or device not creating)\n");
      return false;
   }
   const struct agxw_ddi_devicecallbacks *callbacks =
      (const struct agxw_ddi_devicecallbacks *)b.callbacks;
   if (resource_acquire == NULL || resource_complete == NULL ||
       callbacks->pfnAllocateCb == NULL ||
       callbacks->pfnDeallocate2Cb == NULL) {
      fprintf(stderr,
              "agx_d3dkmt_bridge: runtime-resource handoff ABI/callbacks "
              "missing; refusing partial bridge\n");
      return false;
   }
   g_resource_acquire = resource_acquire;
   g_resource_complete = resource_complete;

   AcquireSRWLockExclusive(&g_registry_lock);
   int slot = agxw_bridge_provider_alloc(&g_registry, b.callbacks, b.hRTDevice);
   if (slot >= 0) {
      g_registry.prov[slot].alive = (const void *)b.alive;
      g_registry.prov[slot].alive_value = (uint64_t)b.alive_value;
      g_registry.prov[slot].in_flight = (const void *)b.in_flight;
   }
   ReleaseSRWLockExclusive(&g_registry_lock);
   if (slot < 0) {
      fprintf(stderr,
              "agx_d3dkmt_bridge: all %u provider slots in use; refusing device\n",
              AGXW_BRIDGE_MAX_DEVICES);
      return false;
   }

   AcquireSRWLockExclusive(&g_diag_lock[slot]);
   memset(&g_diag[slot], 0, sizeof(g_diag[slot]));
   ReleaseSRWLockExclusive(&g_diag_lock[slot]);

   install_slot(&ws->fns, (unsigned)slot);
   ws->device = agxw_bridge_sentinel_for_slot((unsigned)slot);
   ws->device_lease_held = false;

   fprintf(stderr,
           "agx_d3dkmt_bridge: installed on runtime device %p slot=%d live=%u "
           "(callbacks %p)\n",
           b.hRTDevice, slot, agxw_bridge_provider_count(&g_registry),
           b.callbacks);
   return true;
}

void
agx_d3dkmt_bridge_uninstall(struct agx_d3dkmt_winsys *ws)
{
   if (ws == NULL || !agxw_bridge_is_sentinel(ws->device))
      return;
   unsigned slot = agxw_bridge_slot_of_sentinel(ws->device);

   AcquireSRWLockShared(&g_registry_lock);
   const struct agxw_bridge_provider_core *provider =
      agxw_bridge_provider_get(&g_registry, slot);
   const unsigned live_resources = agxw_bridge_resource_count(provider);
   ReleaseSRWLockShared(&g_registry_lock);
   if (live_resources != 0) {
      fprintf(stderr,
              "agx_d3dkmt_bridge: SEVERE provider teardown with %u live "
              "runtime resources in slot %u; destroy-before-uninstall "
              "contract violated\n",
              live_resources, slot);
      agx_bridge_valog(
         "SEVERE RESOURCE LEAK slot=%u live=%u provider teardown would "
         "strand hRTResource owners",
         slot, live_resources);
   }
   assert(live_resources == 0 &&
          "runtime resources must be destroyed before bridge uninstall");

   AcquireSRWLockShared(&g_diag_lock[slot]);
   const struct agxw_bridge_diag *summary = &g_diag[slot];
   agx_bridge_valog(
      "BO SUMMARY slot=%u alloc_calls=%llu alloc_fail=%llu bad_output=%llu "
      "valid_bytes=%llu alloc4m=%llu alloc4m_fail=%llu "
      "refused_no_handoff=%llu degraded_no_handoff=%llu resident_calls=%llu "
      "resident_fail=%llu pending=%llu trim_events=%llu trim_bytes=%llu "
      "resident4m=%llu update_calls=%llu update_fail=%llu update4m=%llu "
      "wait_calls=%llu wait_fail=%llu wait4m=%llu",
      slot, (unsigned long long)summary->alloc_calls,
      (unsigned long long)summary->alloc_failures,
      (unsigned long long)summary->alloc_bad_output,
      (unsigned long long)summary->alloc_valid_bytes,
      (unsigned long long)summary->alloc_target_calls,
      (unsigned long long)summary->alloc_target_failures,
      (unsigned long long)summary->alloc_refused_no_handoff,
      (unsigned long long)summary->alloc_degraded_no_handoff,
      (unsigned long long)summary->resident_calls,
      (unsigned long long)summary->resident_failures,
      (unsigned long long)summary->resident_pending,
      (unsigned long long)summary->resident_trim_events,
      (unsigned long long)summary->resident_trim_bytes,
      (unsigned long long)summary->resident_target_calls,
      (unsigned long long)summary->update_calls,
      (unsigned long long)summary->update_failures,
      (unsigned long long)summary->update_target_calls,
      (unsigned long long)summary->wait_calls,
      (unsigned long long)summary->wait_failures,
      (unsigned long long)summary->wait_target_calls);
   agx_bridge_valog(
      "QUEUE SUMMARY slot=%u ctx_create=%llu ctx_create_cb_fail=%llu "
      "ctx_create_token_fail=%llu ctx_destroy=%llu "
      "ctx_destroy_lookup_fail=%llu ctx_destroy_cb_fail=%llu "
      "sync_create=%llu sync_create_fail=%llu sync_create_bad=%llu "
      "sync_destroy=%llu sync_destroy_fail=%llu",
      slot,
      (unsigned long long)summary->context_create_calls,
      (unsigned long long)summary->context_create_callback_failures,
      (unsigned long long)summary->context_create_token_failures,
      (unsigned long long)summary->context_destroy_calls,
      (unsigned long long)summary->context_destroy_lookup_failures,
      (unsigned long long)summary->context_destroy_callback_failures,
      (unsigned long long)summary->sync_create_calls,
      (unsigned long long)summary->sync_create_failures,
      (unsigned long long)summary->sync_create_bad_output,
      (unsigned long long)summary->sync_destroy_calls,
      (unsigned long long)summary->sync_destroy_failures);
   ReleaseSRWLockShared(&g_diag_lock[slot]);

   AcquireSRWLockExclusive(&g_registry_lock);
   agxw_bridge_provider_free(&g_registry, slot);
   ReleaseSRWLockExclusive(&g_registry_lock);
}

void
agx_d3dkmt_bridge_log_paging_wait(
   const struct agx_d3dkmt_winsys *ws,
   enum agx_d3dkmt_bridge_wait_stage stage, uint64_t fence_value, int result)
{
   if (ws == NULL || !agxw_bridge_is_sentinel(ws->device))
      return;
   const unsigned slot = agxw_bridge_slot_of_sentinel(ws->device);
   if (slot >= AGXW_BRIDGE_MAX_DEVICES)
      return;

   AcquireSRWLockExclusive(&g_diag_lock[slot]);
   struct agxw_bridge_diag *d = &g_diag[slot];
   const bool target = diag_take_target_fence_locked(d, stage, fence_value);
   const uint64_t call = ++d->wait_calls;
   d->wait_failures += result != 0;
   d->wait_target_calls += target;
   ReleaseSRWLockExclusive(&g_diag_lock[slot]);

   if (result != 0 || target)
      agx_bridge_valog(
         "BO PagingWait slot=%u call=%llu stage=%s target4m=%u fence=%llu "
         "result=%d",
         slot, (unsigned long long)call,
         stage == AGX_D3DKMT_BRIDGE_WAIT_MAKE_RESIDENT ? "MakeResident"
                                                       : "UpdateGpuVA",
         target ? 1u : 0u, (unsigned long long)fence_value, result);
}

const void *
agx_d3dkmt_bridge_rtdevice(const struct agx_d3dkmt_winsys *ws)
{
   if (ws == NULL || !agxw_bridge_is_sentinel(ws->device))
      return NULL;
   unsigned slot = agxw_bridge_slot_of_sentinel(ws->device);

   const void *rt = NULL;
   AcquireSRWLockShared(&g_registry_lock);
   struct agxw_bridge_provider_core *p =
      agxw_bridge_provider_get(&g_registry, slot);
   if (p != NULL)
      rt = p->hRTDevice;
   ReleaseSRWLockShared(&g_registry_lock);
   return rt;
}

uint64_t
agx_d3dkmt_bridge_vm_key(const struct agx_d3dkmt_winsys *ws)
{
   if (ws == NULL || !agxw_bridge_is_sentinel(ws->device))
      return 0;

   return (uint64_t)(uint32_t)ws->luid.LowPart |
          ((uint64_t)(uint32_t)ws->luid.HighPart << 32);
}

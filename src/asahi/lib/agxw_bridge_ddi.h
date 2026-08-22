/*
 * Copyright 2026 the Asahi Linux contributors
 * SPDX-License-Identifier: MIT
 */
#pragma once

/*
 * Vendored, ABI-pinned subset of the WDDM user-mode DDI *callback* interface
 * (<d3dumddi.h>), for the runtime-device bridge (agx_d3dkmt_bridge.c).
 *
 * WHY VENDOR, exactly as agx_d3dkmt.h vendors the D3DKMT *thunk* subset:
 * llvm-mingw ships no <d3dumddi.h>, and the SDK copy is not redistributable in
 * a Mesa patch. This header declares only the ~20 D3DDDICB_* structs and the
 * device-callback function-pointer table members this bridge actually reads.
 *
 * REUSE: every callback whose argument is a shared D3DDDI_* struct (already
 * vendored and conformance-tested in agx_d3dkmt.h -- MakeResident,
 * ReserveGpuVirtualAddress, the UpdateGpuVirtualAddress operation, and the
 * synchronization-object info) takes the agx_kmt_* type directly, so those
 * layouts are not re-vendored here and cannot drift independently.
 *
 * LAYOUT PROOF: agxw_bridge_conformance.cpp includes the real <d3dumddi.h>
 * (and <d3dkmthk.h>) at global scope and this header inside `namespace vend`,
 * then static_asserts every sizeof and offsetof pairwise, and asserts the
 * device-callback table member OFFSETS so a version-gated slot shift is a hard
 * compile error. It is compiled by the same aarch64-w64-mingw32 cross compiler
 * the real build uses.
 *
 * PINNED VERSION: Windows SDK/WDK 10.0.26100.6584, um/d3dumddi.h +
 * shared/d3dukmdt.h, D3D_UMD_INTERFACE_VERSION latest.
 *
 * CONTEXT HANDLE WIDTH (the one non-obvious hazard): in the *thunk* world a
 * context is a 32-bit D3DKMT_HANDLE, but every *callback* names a context as a
 * pointer-width HANDLE (CreateContextVirtualCb returns HANDLE; Submit/
 * Update/Signal/Wait take HANDLE). Honeykrisp's winsys stores contexts as
 * agx_kmt_handle (32-bit). The bridge therefore keeps a small token<->HANDLE
 * table and never lets a 64-bit context handle reach a 32-bit field. This
 * header exposes both widths so that translation is explicit, never implicit.
 */

#include "agx_d3dkmt.h" /* base types + the reused D3DDDI_* (agx_kmt_*) structs */

#ifdef __cplusplus
extern "C" {
#endif

/* Pointer-width handle, as the callbacks name a context/event/device. */
typedef void *agxw_ddi_handle;

/* -------------------------------------------------------------------------
 * Allocation.  agx_kmt_allocationinfo2 (agx_d3dkmt.h) IS D3DDDI_ALLOCATIONINFO2
 * -- same struct on both the thunk and callback paths -- so it is reused
 * rather than re-vendored, and its conformance assertions already cover it.
 * ------------------------------------------------------------------------- */
struct agxw_ddi_allocate { /* D3DDDICB_ALLOCATE */
   const void *pPrivateDriverData; /* resource-level blob (in) */
   uint32_t PrivateDriverDataSize;
   agxw_ddi_handle hResource; /* in: runtime resource, or NULL (UMD-private) */
   agx_kmt_handle hKMResource; /* out */
   uint32_t NumAllocations;
   struct agx_kmt_allocationinfo2 *pAllocationInfo2;
};

struct agxw_ddi_deallocate2 { /* D3DDDICB_DEALLOCATE2 */
   agxw_ddi_handle hResource;
   uint32_t NumAllocations;
   const agx_kmt_handle *HandleList;
   uint32_t Flags;
};

/* D3DDDICB_DESTROYALLOCATION2FLAGS. SystemUseOnly (bit 31) is runtime-only
 * and must never be emitted by either Honeykrisp or this UMD bridge. */
#define AGXW_DDI_DESTROY_ASSUME_NOT_IN_USE (1u << 0)
#define AGXW_DDI_DESTROY_SYNCHRONOUS (1u << 1)
#define AGXW_DDI_DESTROY_UMD_FLAGS_ALL                                    \
   (AGXW_DDI_DESTROY_ASSUME_NOT_IN_USE | AGXW_DDI_DESTROY_SYNCHRONOUS)

/* -------------------------------------------------------------------------
 * Residency / CPU access.
 * ------------------------------------------------------------------------- */
struct agxw_ddi_evict { /* D3DDDICB_EVICT */
   uint32_t NumAllocations;
   const agx_kmt_handle *AllocationList;
   uint32_t Flags;
   uint64_t NumBytesToTrim; /* out */
};

struct agxw_ddi_lock2 { /* D3DDDICB_LOCK2 */
   agx_kmt_handle hAllocation;
   uint32_t Flags;
   void *pData; /* out */
};

struct agxw_ddi_unlock2 { /* D3DDDICB_UNLOCK2 */
   agx_kmt_handle hAllocation;
};

struct agxw_ddi_invalidatecache { /* D3DDDICB_INVALIDATECACHE */
   agx_kmt_handle hAllocation;
   size_t Offset;
   size_t Length;
};

/* -------------------------------------------------------------------------
 * Paging queue.
 * ------------------------------------------------------------------------- */
struct agxw_ddi_createpagingqueue { /* D3DDDICB_CREATEPAGINGQUEUE */
   int32_t Priority;               /* D3DDDI_PAGINGQUEUE_PRIORITY */
   agx_kmt_handle hPagingQueue;    /* out */
   agx_kmt_handle hSyncObject;     /* out */
   void *FenceValueCPUVirtualAddress; /* out */
   uint32_t PhysicalAdapterIndex;
};

struct agxw_ddi_destroypagingqueue { /* D3DDDI_DESTROYPAGINGQUEUE */
   agx_kmt_handle hPagingQueue;
};

/* -------------------------------------------------------------------------
 * GPU virtual address.
 * ------------------------------------------------------------------------- */
struct agxw_ddi_freegpuva { /* D3DDDICB_FREEGPUVIRTUALADDRESS */
   agx_kmt_gpuva BaseAddress;
   agx_kmt_gpusize Size;
};

struct agxw_ddi_updategpuva { /* D3DDDICB_UPDATEGPUVIRTUALADDRESS */
   agxw_ddi_handle hContext;
   agx_kmt_handle hFenceObject;
   uint32_t NumOperations;
   struct agx_kmt_updategpuva_op *Operations; /* reused vendored op */
   uint32_t Reserved0;
   uint64_t Reserved1;
   uint64_t FenceValue;
   uint32_t Flags; /* {DoNotWait:1} */
};

/* -------------------------------------------------------------------------
 * Contexts and submission.
 * ------------------------------------------------------------------------- */
struct agxw_ddi_createcontextvirtual { /* D3DDDICB_CREATECONTEXTVIRTUAL */
   uint32_t NodeOrdinal;
   uint32_t EngineAffinity;
   uint32_t Flags; /* D3DDDI_CREATECONTEXTFLAGS */
   void *pPrivateDriverData;
   uint32_t PrivateDriverDataSize;
   agxw_ddi_handle hContext; /* out: POINTER-WIDTH */
};

struct agxw_ddi_destroycontext { /* D3DDDICB_DESTROYCONTEXT */
   agxw_ddi_handle hContext;
};

struct agxw_ddi_submitcommand { /* D3DDDICB_SUBMITCOMMAND */
   agx_kmt_gpuva Commands;
   uint32_t CommandLength;
   uint32_t Flags; /* D3DDDICB_SUBMITCOMMANDFLAGS */
   uint32_t BroadcastContextCount;
   agxw_ddi_handle BroadcastContext[AGX_KMT_MAX_BROADCAST_CONTEXT]; /* HANDLE */
   void *pPrivateDriverData;
   uint32_t PrivateDriverDataSize;
   uint32_t NumPrimaries;
   agx_kmt_handle WrittenPrimaries[AGX_KMT_MAX_WRITTEN_PRIMARIES];
   int32_t MarkerLogType; /* D3DDDI_MARKERLOGTYPE */
   uint32_t RenderCBSequence;
   union {
      struct {
         uint32_t FirstAPISequenceNumberHigh;
         uint32_t CompletedAPISequenceNumberLow0Size;
         uint32_t CompletedAPISequenceNumberLow1Size;
         uint32_t BegunAPISequenceNumberLow0Size;
         uint32_t BegunAPISequenceNumberLow1Size;
      };
      uint32_t BatchedMarkerDataCount;
   };
   union {
      struct {
         const uint32_t *pCompletedAPISequenceNumberLow0;
         const uint32_t *pCompletedAPISequenceNumberLow1;
         const uint32_t *pBegunAPISequenceNumberLow0;
         const uint32_t *pBegunAPISequenceNumberLow1;
      };
      const void *pBatchedMarkerData;
   };
   uint32_t Reserved;
   uint32_t NumHistoryBuffers;
   agx_kmt_handle *HistoryBufferArray;
   void *pReserved;
};

/* -------------------------------------------------------------------------
 * Synchronization objects (monitored fences).
 * ------------------------------------------------------------------------- */
struct agxw_ddi_createsyncobj2 { /* D3DDDICB_CREATESYNCHRONIZATIONOBJECT2 */
   struct agx_kmt_syncobjinfo2 Info; /* reused vendored info */
   agx_kmt_handle hSyncObject;       /* out */
};

struct agxw_ddi_destroysyncobj { /* D3DDDICB_DESTROYSYNCHRONIZATIONOBJECT */
   agx_kmt_handle hSyncObject;
};

struct agxw_ddi_signalsyncfromcpu { /* D3DDDICB_SIGNAL...FROMCPU */
   uint32_t ObjectCount;
   const agx_kmt_handle *ObjectHandleArray;
   const uint64_t *FenceValueArray;
};

struct agxw_ddi_waitsyncfromcpu { /* D3DDDICB_WAIT...FROMCPU */
   uint32_t ObjectCount;
   const agx_kmt_handle *ObjectHandleArray;
   const uint64_t *FenceValueArray;
   agxw_ddi_handle hAsyncEvent;
   uint32_t Flags;
};

struct agxw_ddi_signalsyncfromgpu { /* D3DDDICB_SIGNAL...FROMGPU */
   agxw_ddi_handle hContext;
   uint32_t ObjectCount;
   const agx_kmt_handle *ObjectHandleArray;
   union {
      const uint64_t *MonitoredFenceValueArray;
      uint64_t Reserved[8];
   };
};

struct agxw_ddi_waitsyncfromgpu { /* D3DDDICB_WAIT...FROMGPU */
   agxw_ddi_handle hContext;
   uint32_t ObjectCount;
   const agx_kmt_handle *ObjectHandleArray;
   union {
      const uint64_t *MonitoredFenceValueArray;
      uint64_t FenceValue;
      uint64_t Reserved[8];
   };
};

/* -------------------------------------------------------------------------
 * The device callback table.  Members are declared in the EXACT order of the
 * real D3DDDI_DEVICECALLBACKS through the last one this bridge reads
 * (pfnDeallocate2Cb); unused earlier members are typed void* so the offsets
 * of the members we DO read match the runtime's table.  The conformance test
 * asserts every used member's offset against the real header.
 * ------------------------------------------------------------------------- */
#if defined(_WIN32)
#define AGXW_DDI_API __stdcall
#else
#define AGXW_DDI_API
#endif

typedef agx_kmt_status(AGXW_DDI_API *pfn_agxw_allocate)(
   agxw_ddi_handle, struct agxw_ddi_allocate *);
typedef agx_kmt_status(AGXW_DDI_API *pfn_agxw_escape)(agxw_ddi_handle,
                                                      const void *);
typedef agx_kmt_status(AGXW_DDI_API *pfn_agxw_destroycontext)(
   agxw_ddi_handle, struct agxw_ddi_destroycontext *);
typedef agx_kmt_status(AGXW_DDI_API *pfn_agxw_destroysyncobj)(
   agxw_ddi_handle, struct agxw_ddi_destroysyncobj *);
typedef agx_kmt_status(AGXW_DDI_API *pfn_agxw_createsyncobj2)(
   agxw_ddi_handle, struct agxw_ddi_createsyncobj2 *);
typedef agx_kmt_status(AGXW_DDI_API *pfn_agxw_makeresident)(
   agxw_ddi_handle, struct agx_kmt_makeresident *);
typedef agx_kmt_status(AGXW_DDI_API *pfn_agxw_evict)(agxw_ddi_handle,
                                                     struct agxw_ddi_evict *);
typedef agx_kmt_status(AGXW_DDI_API *pfn_agxw_waitcpu)(
   agxw_ddi_handle, const struct agxw_ddi_waitsyncfromcpu *);
typedef agx_kmt_status(AGXW_DDI_API *pfn_agxw_signalcpu)(
   agxw_ddi_handle, const struct agxw_ddi_signalsyncfromcpu *);
typedef agx_kmt_status(AGXW_DDI_API *pfn_agxw_waitgpu)(
   agxw_ddi_handle, const struct agxw_ddi_waitsyncfromgpu *);
typedef agx_kmt_status(AGXW_DDI_API *pfn_agxw_signalgpu)(
   agxw_ddi_handle, const struct agxw_ddi_signalsyncfromgpu *);
typedef agx_kmt_status(AGXW_DDI_API *pfn_agxw_createpagingqueue)(
   agxw_ddi_handle, struct agxw_ddi_createpagingqueue *);
typedef agx_kmt_status(AGXW_DDI_API *pfn_agxw_destroypagingqueue)(
   agxw_ddi_handle, struct agxw_ddi_destroypagingqueue *);
typedef agx_kmt_status(AGXW_DDI_API *pfn_agxw_lock2)(agxw_ddi_handle,
                                                     struct agxw_ddi_lock2 *);
typedef agx_kmt_status(AGXW_DDI_API *pfn_agxw_unlock2)(
   agxw_ddi_handle, const struct agxw_ddi_unlock2 *);
typedef agx_kmt_status(AGXW_DDI_API *pfn_agxw_invalidatecache)(
   agxw_ddi_handle, const struct agxw_ddi_invalidatecache *);
typedef agx_kmt_status(AGXW_DDI_API *pfn_agxw_reservegpuva)(
   agxw_ddi_handle, struct agx_kmt_reservegpuva *);
typedef agx_kmt_status(AGXW_DDI_API *pfn_agxw_freegpuva)(
   agxw_ddi_handle, const struct agxw_ddi_freegpuva *);
typedef agx_kmt_status(AGXW_DDI_API *pfn_agxw_updategpuva)(
   agxw_ddi_handle, const struct agxw_ddi_updategpuva *);
typedef agx_kmt_status(AGXW_DDI_API *pfn_agxw_createcontextvirtual)(
   agxw_ddi_handle, struct agxw_ddi_createcontextvirtual *);
typedef agx_kmt_status(AGXW_DDI_API *pfn_agxw_submitcommand)(
   agxw_ddi_handle, const struct agxw_ddi_submitcommand *);
typedef agx_kmt_status(AGXW_DDI_API *pfn_agxw_deallocate2)(
   agxw_ddi_handle, struct agxw_ddi_deallocate2 *);

struct agxw_ddi_devicecallbacks { /* D3DDDI_DEVICECALLBACKS (prefix) */
   pfn_agxw_allocate pfnAllocateCb;
   void *pfnDeallocateCb;
   void *pfnSetPriorityCb;
   void *pfnQueryResidencyCb;
   void *pfnSetDisplayModeCb;
   void *pfnPresentCb;
   void *pfnRenderCb;
   void *pfnLockCb;
   void *pfnUnlockCb;
   pfn_agxw_escape pfnEscapeCb;
   void *pfnCreateOverlayCb;
   void *pfnUpdateOverlayCb;
   void *pfnFlipOverlayCb;
   void *pfnDestroyOverlayCb;
   void *pfnCreateContextCb;
   pfn_agxw_destroycontext pfnDestroyContextCb;
   void *pfnCreateSynchronizationObjectCb;
   pfn_agxw_destroysyncobj pfnDestroySynchronizationObjectCb;
   void *pfnWaitForSynchronizationObjectCb;
   void *pfnSignalSynchronizationObjectCb;
   void *pfnSetAsyncCallbacksCb;
   void *pfnSetDisplayPrivateDriverFormatCb;
   void *pfnOfferAllocationsCb;
   void *pfnReclaimAllocationsCb;
   pfn_agxw_createsyncobj2 pfnCreateSynchronizationObject2Cb;
   void *pfnWaitForSynchronizationObject2Cb;
   void *pfnSignalSynchronizationObject2Cb;
   void *pfnPresentMultiPlaneOverlayCb;
   void *pfnLogUMDMarkerCb;
   pfn_agxw_makeresident pfnMakeResidentCb;
   pfn_agxw_evict pfnEvictCb;
   pfn_agxw_waitcpu pfnWaitForSynchronizationObjectFromCpuCb;
   pfn_agxw_signalcpu pfnSignalSynchronizationObjectFromCpuCb;
   pfn_agxw_waitgpu pfnWaitForSynchronizationObjectFromGpuCb;
   pfn_agxw_signalgpu pfnSignalSynchronizationObjectFromGpuCb;
   pfn_agxw_createpagingqueue pfnCreatePagingQueueCb;
   pfn_agxw_destroypagingqueue pfnDestroyPagingQueueCb;
   pfn_agxw_lock2 pfnLock2Cb;
   pfn_agxw_unlock2 pfnUnlock2Cb;
   pfn_agxw_invalidatecache pfnInvalidateCacheCb;
   pfn_agxw_reservegpuva pfnReserveGpuVirtualAddressCb;
   void *pfnMapGpuVirtualAddressCb;
   pfn_agxw_freegpuva pfnFreeGpuVirtualAddressCb;
   pfn_agxw_updategpuva pfnUpdateGpuVirtualAddressCb;
   pfn_agxw_createcontextvirtual pfnCreateContextVirtualCb;
   pfn_agxw_submitcommand pfnSubmitCommandCb;
   pfn_agxw_deallocate2 pfnDeallocate2Cb;
   /* Members past pfnDeallocate2Cb exist in the real table but are never read
    * by this bridge, so they are intentionally omitted. */
};

#ifdef __cplusplus
}
#endif

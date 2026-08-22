/*
 * Copyright 2026 the Asahi Linux contributors
 * SPDX-License-Identifier: MIT
 */
#pragma once

/*
 * Vendored, ABI-pinned subset of the Windows D3DKMT thunk interface
 * (<d3dkmthk.h> / <d3dkmdt.h> / <d3dukmdt.h>, exported from gdi32.dll).
 *
 * WHY VENDOR RATHER THAN #include <d3dkmthk.h>:
 * llvm-mingw -- the only Windows/arm64 toolchain this port cross-builds with
 * (honeykrisp-port/cross-aarch64-windows.txt) -- ships no d3dkmthk.h at all
 * (VERIFIED: no such file anywhere under $TOOLCHAINS/llvm-mingw), while the
 * Windows SDK copy is not redistributable inside a Mesa patch. Vendoring the
 * ~30 structs this winsys actually touches keeps the cross-build dependency-
 * free, exactly the way kmd-uapi-core/kmd_uapi.h vendors the Asahi DRM uapi.
 *
 * PINNED VERSION: Windows SDK 10.0.26100.6584, shared/d3dkmthk.h +
 * shared/d3dkmdt.h + shared/d3dukmdt.h, DXGKDDI_INTERFACE_VERSION_WDDM3_2.
 * Layout is guaranteed the same way kmd_uapi.h guarantees its own: a
 * conformance TU (agx_d3dkmt_conformance.cpp, run by
 * tests/run-host-tests.sh) includes the REAL SDK headers at global scope and
 * this header inside `namespace vend`, then static_asserts every sizeof and
 * every offsetof pairwise. If Microsoft ever changes a layout, that test
 * fails; it is never "checked by hand".
 *
 * SCOPE: only 64-bit Windows (aarch64 and x86_64). On _WIN32 the SDK's own
 * D3DKMT_PTR(Type,Name) macro expands to a plain `Type Name;` and
 * D3DKMT_ALIGN64 expands to nothing (VERIFIED d3dukmdt.h:168-178), so the
 * plain field declarations below ARE the Microsoft layout -- there is no
 * 32-bit thunking shape to reproduce.
 *
 * NAMING: Microsoft field names are kept verbatim so the conformance test's
 * offsetof pairs can be written mechanically, and so a reader can diff this
 * against MSDN. Type names are prefixed `agx_kmt_` because the real ones are
 * typedef'd in the SDK and would collide when both headers are visible.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ---------------------------------------------------------------------------
 * CONTRACT STATUS
 *
 * The kernel<->userspace contract this winsys is written against is
 * drivers/AppleAgxWddm/CONTRACT.md, machine-readable as
 * drivers/AppleAgxWddm/core/agxw_contract.h -- which this port INCLUDES
 * rather than transcribes, so a contract bump is a compile error rather than
 * a silent disagreement. agx_d3dkmt_core.h/.c and agx_device_d3dkmt.c cite
 * CONTRACT.md sections at every point the two models differ.
 *
 * This header is only the Microsoft ABI. It contains no assumption about the
 * miniport: everything of that kind now lives in agxw_contract.h. The two
 * items still open against the contract are documented where they bite --
 * grep for "contract gap":
 *
 *   - d3dkmt_bo_bind_object() in agx_device_d3dkmt.c: SS3.5's
 *     AGXW_ALLOC_TIMESTAMP output has nowhere to land in
 *     struct agxw_alloc_private, and Honeykrisp binds timestamps after the
 *     BO already exists.
 *   - d3dkmt_reserve() in agx_device_d3dkmt.c: SS4.3 decides BaseAddress is
 *     mandatory but flags it as unproven against VidMm. That assumption is
 *     isolated to that one function so it is swappable, not diffused.
 * ---------------------------------------------------------------------------
 */

/* -------------------------------------------------------------------------
 * Base types (d3dukmdt.h:175-209, 367).
 * ------------------------------------------------------------------------- */
typedef uint32_t agx_kmt_handle;  /* D3DKMT_HANDLE == UINT */
typedef uint64_t agx_kmt_gpuva;   /* D3DGPU_VIRTUAL_ADDRESS == ULONGLONG */
typedef uint64_t agx_kmt_gpusize; /* D3DGPU_SIZE_T == ULONGLONG */
typedef int32_t agx_kmt_status;   /* NTSTATUS == LONG */

#define AGX_KMT_SUCCESS(s) ((agx_kmt_status)(s) >= 0)

struct agx_kmt_luid {
   uint32_t LowPart;
   int32_t HighPart;
};

/* d3dukmdt.h:343, 2072 */
#define AGX_KMT_MAX_WRITTEN_PRIMARIES 16
#define AGX_KMT_MAX_BROADCAST_CONTEXT 64

/* -------------------------------------------------------------------------
 * Adapter enumeration and query.
 * ------------------------------------------------------------------------- */
struct agx_kmt_adapterinfo { /* D3DKMT_ADAPTERINFO */
   agx_kmt_handle hAdapter;
   struct agx_kmt_luid AdapterLuid;
   uint32_t NumOfSources;
   int32_t bPrecisePresentRegionsPreferred; /* BOOL */
};

struct agx_kmt_enumadapters2 { /* D3DKMT_ENUMADAPTERS2 */
   uint32_t NumAdapters;      /* ULONG */
   struct agx_kmt_adapterinfo *pAdapters;
};

struct agx_kmt_openadapterfromluid { /* D3DKMT_OPENADAPTERFROMLUID */
   struct agx_kmt_luid AdapterLuid;
   agx_kmt_handle hAdapter;
};

struct agx_kmt_closeadapter { /* D3DKMT_CLOSEADAPTER */
   agx_kmt_handle hAdapter;
};

/* KMTQUERYADAPTERINFOTYPE, d3dkmthk.h. Only the values this port uses. */
#define AGX_KMT_QAITYPE_UMDRIVERPRIVATE 0
#define AGX_KMT_QAITYPE_ADAPTERTYPE     15

struct agx_kmt_queryadapterinfo { /* D3DKMT_QUERYADAPTERINFO */
   agx_kmt_handle hAdapter;
   int32_t Type; /* KMTQUERYADAPTERINFOTYPE (enum, int-sized) */
   void *pPrivateDriverData;
   uint32_t PrivateDriverDataSize;
};

struct agx_kmt_adaptertype { /* D3DKMT_ADAPTERTYPE, bitfield block */
   uint32_t Value;
};
#define AGX_KMT_ADAPTERTYPE_RENDERSUPPORTED (1u << 0)
#define AGX_KMT_ADAPTERTYPE_DISPLAYSUPPORTED (1u << 4)
#define AGX_KMT_ADAPTERTYPE_SOFTWAREDEVICE (1u << 3)

/* -------------------------------------------------------------------------
 * Device.
 * ------------------------------------------------------------------------- */
struct agx_kmt_createdevice { /* D3DKMT_CREATEDEVICE */
   union {
      agx_kmt_handle hAdapter;
      void *pAdapter;
   };
   uint32_t Flags; /* D3DKMT_CREATEDEVICEFLAGS bitfields */
   agx_kmt_handle hDevice;
   void *pCommandBuffer;
   uint32_t CommandBufferSize;
   void *pAllocationList;
   uint32_t AllocationListSize;
   void *pPatchLocationList;
   uint32_t PatchLocationListSize;
};

struct agx_kmt_destroydevice { /* D3DKMT_DESTROYDEVICE */
   agx_kmt_handle hDevice;
};

/* -------------------------------------------------------------------------
 * Allocations.
 * ------------------------------------------------------------------------- */
struct agx_kmt_allocationinfo2 { /* D3DDDI_ALLOCATIONINFO2 */
   agx_kmt_handle hAllocation;
   union {
      void *hSection;
      const void *pSystemMem;
   };
   void *pPrivateDriverData;
   uint32_t PrivateDriverDataSize;
   uint32_t VidPnSourceId;
   uint32_t Flags;
   agx_kmt_gpuva GpuVirtualAddress;
   union {
      uint32_t Priority;
      uint64_t Unused;
   };
   uint64_t Reserved[5];
};

/* D3DKMT_CREATEALLOCATIONFLAGS, d3dkmthk.h. Bit positions, not bitfields. */
#define AGX_KMT_CREATEALLOC_CREATERESOURCE (1u << 0)
#define AGX_KMT_CREATEALLOC_CREATESHARED   (1u << 1)
#define AGX_KMT_CREATEALLOC_NONSECURE      (1u << 2)
#define AGX_KMT_CREATEALLOC_EXISTINGSYSMEM (1u << 5)
#define AGX_KMT_CREATEALLOC_NTSECURITYSHARING (1u << 6)
#define AGX_KMT_CREATEALLOC_READONLY       (1u << 7)
#define AGX_KMT_CREATEALLOC_ALLOWNOTZEROED (1u << 18)

struct agx_kmt_createallocation { /* D3DKMT_CREATEALLOCATION */
   agx_kmt_handle hDevice;
   agx_kmt_handle hResource;
   agx_kmt_handle hGlobalShare;
   const void *pPrivateRuntimeData;
   uint32_t PrivateRuntimeDataSize;
   union {
      void *pStandardAllocation;
      const void *pPrivateDriverData;
   };
   uint32_t PrivateDriverDataSize;
   uint32_t NumAllocations;
   union {
      void *pAllocationInfo;
      struct agx_kmt_allocationinfo2 *pAllocationInfo2;
   };
   uint32_t Flags;
   void *hPrivateRuntimeResourceHandle;
};

/* D3DDDI_OPENALLOCATIONINFO2. The pointer-sized Reserved words are 64-bit in
 * both supported builds (Windows/arm64 and Windows/x64). */
struct agx_kmt_openallocationinfo2 {
   agx_kmt_handle hAllocation;
   const void *pPrivateDriverData;
   uint32_t PrivateDriverDataSize;
   agx_kmt_gpuva GpuVirtualAddress;
   uintptr_t Reserved[6];
};

struct agx_kmt_queryresourceinfo { /* D3DKMT_QUERYRESOURCEINFO */
   agx_kmt_handle hDevice;
   agx_kmt_handle hGlobalShare;
   void *pPrivateRuntimeData;
   uint32_t PrivateRuntimeDataSize;
   uint32_t TotalPrivateDriverDataSize;
   uint32_t ResourcePrivateDriverDataSize;
   uint32_t NumAllocations;
};

struct agx_kmt_queryresourceinfofromnthandle {
   agx_kmt_handle hDevice;
   void *hNtHandle;
   void *pPrivateRuntimeData;
   uint32_t PrivateRuntimeDataSize;
   uint32_t TotalPrivateDriverDataSize;
   uint32_t ResourcePrivateDriverDataSize;
   uint32_t NumAllocations;
};

struct agx_kmt_openresource { /* D3DKMT_OPENRESOURCE */
   agx_kmt_handle hDevice;
   agx_kmt_handle hGlobalShare;
   uint32_t NumAllocations;
   union {
      void *pOpenAllocationInfo;
      struct agx_kmt_openallocationinfo2 *pOpenAllocationInfo2;
   };
   void *pPrivateRuntimeData;
   uint32_t PrivateRuntimeDataSize;
   void *pResourcePrivateDriverData;
   uint32_t ResourcePrivateDriverDataSize;
   void *pTotalPrivateDriverDataBuffer;
   uint32_t TotalPrivateDriverDataBufferSize;
   agx_kmt_handle hResource;
};

struct agx_kmt_openresourcefromnthandle {
   agx_kmt_handle hDevice;
   void *hNtHandle;
   uint32_t NumAllocations;
   struct agx_kmt_openallocationinfo2 *pOpenAllocationInfo2;
   uint32_t PrivateRuntimeDataSize;
   void *pPrivateRuntimeData;
   uint32_t ResourcePrivateDriverDataSize;
   void *pResourcePrivateDriverData;
   uint32_t TotalPrivateDriverDataBufferSize;
   void *pTotalPrivateDriverDataBuffer;
   agx_kmt_handle hResource;
   agx_kmt_handle hKeyedMutex;
   void *pKeyedMutexPrivateRuntimeData;
   uint32_t KeyedMutexPrivateRuntimeDataSize;
   agx_kmt_handle hSyncObject;
};

/* d3dkmthk.h: SHARED_ALLOCATION_ALL_ACCESS. */
#define AGX_KMT_SHARED_ALLOCATION_ALL_ACCESS 0x000f0001u

struct agx_kmt_destroyallocation2 { /* D3DKMT_DESTROYALLOCATION2 */
   agx_kmt_handle hDevice;
   agx_kmt_handle hResource;
   const agx_kmt_handle *phAllocationList;
   uint32_t AllocationCount;
   uint32_t Flags; /* D3DDDICB_DESTROYALLOCATION2FLAGS */
};

struct agx_kmt_lock2 { /* D3DKMT_LOCK2 */
   agx_kmt_handle hDevice;
   agx_kmt_handle hAllocation;
   uint32_t Flags;
   void *pData;
};

struct agx_kmt_unlock2 { /* D3DKMT_UNLOCK2 */
   agx_kmt_handle hDevice;
   agx_kmt_handle hAllocation;
};

struct agx_kmt_invalidatecache { /* D3DKMT_INVALIDATECACHE */
   agx_kmt_handle hDevice;
   agx_kmt_handle hAllocation;
   uint64_t Offset;
   uint64_t Length;
};

/* -------------------------------------------------------------------------
 * Residency.
 * ------------------------------------------------------------------------- */
struct agx_kmt_makeresident { /* D3DDDI_MAKERESIDENT */
   agx_kmt_handle hPagingQueue;
   uint32_t NumAllocations;
   const agx_kmt_handle *AllocationList;
   const uint32_t *PriorityList;
   uint32_t Flags;
   uint64_t PagingFenceValue;
   uint64_t NumBytesToTrim;
};

struct agx_kmt_evict { /* D3DKMT_EVICT */
   agx_kmt_handle hDevice;
   uint32_t NumAllocations;
   const agx_kmt_handle *AllocationList;
   uint32_t Flags;
   uint64_t NumBytesToTrim;
};

/* -------------------------------------------------------------------------
 * Paging queue and GPU virtual address space.
 * ------------------------------------------------------------------------- */
struct agx_kmt_createpagingqueue { /* D3DKMT_CREATEPAGINGQUEUE */
   agx_kmt_handle hDevice;
   int32_t Priority; /* D3DDDI_PAGINGQUEUE_PRIORITY */
   agx_kmt_handle hPagingQueue;
   agx_kmt_handle hSyncObject;
   void *FenceValueCPUVirtualAddress;
   uint32_t PhysicalAdapterIndex;
};

struct agx_kmt_destroypagingqueue { /* D3DDDI_DESTROYPAGINGQUEUE */
   agx_kmt_handle hPagingQueue;
};

/* D3DDDIGPUVIRTUALADDRESS_PROTECTION_TYPE, d3dukmdt.h:1525. */
struct agx_kmt_gpuva_protection {
   uint64_t Value;
};
#define AGX_KMT_GPUVA_PROT_WRITE    (1ull << 0)
#define AGX_KMT_GPUVA_PROT_EXECUTE  (1ull << 1)
#define AGX_KMT_GPUVA_PROT_ZERO     (1ull << 2)
#define AGX_KMT_GPUVA_PROT_NOACCESS (1ull << 3)

/* D3DDDIGPUVIRTUALADDRESS_RESERVATION_TYPE, d3dukmdt.h. */
#define AGX_KMT_GPUVA_RESERVE_NO_ACCESS 0
#define AGX_KMT_GPUVA_RESERVE_ZERO      1

/* D3DDDI_UPDATEGPUVIRTUALADDRESS_OPERATION_TYPE, d3dukmdt.h. */
#define AGX_KMT_UPDATEGPUVA_MAP         0
#define AGX_KMT_UPDATEGPUVA_UNMAP       1
#define AGX_KMT_UPDATEGPUVA_COPY        2
#define AGX_KMT_UPDATEGPUVA_MAP_PROTECT 3

struct agx_kmt_updategpuva_op { /* D3DDDI_UPDATEGPUVIRTUALADDRESS_OPERATION */
   int32_t OperationType;
   union {
      struct {
         agx_kmt_gpuva BaseAddress;
         agx_kmt_gpusize SizeInBytes;
         agx_kmt_handle hAllocation;
         agx_kmt_gpusize AllocationOffsetInBytes;
         agx_kmt_gpusize AllocationSizeInBytes;
      } Map;
      struct {
         agx_kmt_gpuva BaseAddress;
         agx_kmt_gpusize SizeInBytes;
         agx_kmt_handle hAllocation;
         agx_kmt_gpusize AllocationOffsetInBytes;
         agx_kmt_gpusize AllocationSizeInBytes;
         struct agx_kmt_gpuva_protection Protection;
         uint64_t DriverProtection;
      } MapProtect;
      struct {
         agx_kmt_gpuva BaseAddress;
         agx_kmt_gpusize SizeInBytes;
         struct agx_kmt_gpuva_protection Protection;
      } Unmap;
      struct {
         agx_kmt_gpuva SourceAddress;
         agx_kmt_gpusize SizeInBytes;
         agx_kmt_gpuva DestAddress;
      } Copy;
   };
};

struct agx_kmt_reservegpuva { /* D3DDDI_RESERVEGPUVIRTUALADDRESS */
   union {
      agx_kmt_handle hPagingQueue;
      agx_kmt_handle hAdapter;
   };
   agx_kmt_gpuva BaseAddress;
   agx_kmt_gpuva MinimumAddress;
   agx_kmt_gpuva MaximumAddress;
   agx_kmt_gpusize Size;
   union {
      int32_t ReservationType;
      uint32_t Reserved0;
   };
   union {
      uint64_t DriverProtection;
      uint64_t Reserved1;
   };
   agx_kmt_gpuva VirtualAddress;
   union {
      uint64_t PagingFenceValue;
      uint64_t Reserved2;
   };
};

struct agx_kmt_mapgpuva { /* D3DDDI_MAPGPUVIRTUALADDRESS */
   agx_kmt_handle hPagingQueue;
   agx_kmt_gpuva BaseAddress;
   agx_kmt_gpuva MinimumAddress;
   agx_kmt_gpuva MaximumAddress;
   agx_kmt_handle hAllocation;
   agx_kmt_gpusize OffsetInPages;
   agx_kmt_gpusize SizeInPages;
   struct agx_kmt_gpuva_protection Protection;
   uint64_t DriverProtection;
   uint32_t Reserved0;
   uint64_t Reserved1;
   agx_kmt_gpuva VirtualAddress;
   uint64_t PagingFenceValue;
};

struct agx_kmt_updategpuva { /* D3DKMT_UPDATEGPUVIRTUALADDRESS */
   agx_kmt_handle hDevice;
   agx_kmt_handle hContext;
   agx_kmt_handle hFenceObject;
   uint32_t NumOperations;
   struct agx_kmt_updategpuva_op *Operations;
   uint64_t Reserved0;
   uint64_t Reserved1;
   uint64_t FenceValue;
   uint32_t Flags;
};

struct agx_kmt_freegpuva { /* D3DKMT_FREEGPUVIRTUALADDRESS */
   agx_kmt_handle hAdapter;
   agx_kmt_gpuva BaseAddress;
   agx_kmt_gpusize Size;
};

/* -------------------------------------------------------------------------
 * Contexts and submission.
 * ------------------------------------------------------------------------- */
/* D3DDDI_CREATECONTEXTFLAGS. SynchronizationOnly marks a context that only
 * ever issues waits and signals and never renders -- exactly what DRM syncobj
 * transfer needs (see agx_sync_d3dkmt.c). */
#define AGX_KMT_CREATECONTEXT_NULLRENDERING       (1u << 0)
#define AGX_KMT_CREATECONTEXT_DISABLEGPUTIMEOUT   (1u << 2)
#define AGX_KMT_CREATECONTEXT_SYNCHRONIZATIONONLY (1u << 3)

struct agx_kmt_createcontextvirtual { /* D3DKMT_CREATECONTEXTVIRTUAL */
   agx_kmt_handle hDevice;
   uint32_t NodeOrdinal;
   uint32_t EngineAffinity;
   uint32_t Flags; /* D3DDDI_CREATECONTEXTFLAGS */
   void *pPrivateDriverData;
   uint32_t PrivateDriverDataSize;
   int32_t ClientHint; /* D3DKMT_CLIENTHINT */
   agx_kmt_handle hContext;
};

struct agx_kmt_destroycontext { /* D3DKMT_DESTROYCONTEXT */
   agx_kmt_handle hContext;
};

struct agx_kmt_submitcommand { /* D3DKMT_SUBMITCOMMAND */
   agx_kmt_gpuva Commands;
   uint32_t CommandLength;
   uint32_t Flags; /* D3DKMT_SUBMITCOMMANDFLAGS */
   uint64_t PresentHistoryToken;
   uint32_t BroadcastContextCount;
   agx_kmt_handle BroadcastContext[AGX_KMT_MAX_BROADCAST_CONTEXT];
   void *pPrivateDriverData;
   uint32_t PrivateDriverDataSize;
   uint32_t NumPrimaries;
   agx_kmt_handle WrittenPrimaries[AGX_KMT_MAX_WRITTEN_PRIMARIES];
   uint32_t NumHistoryBuffers;
   agx_kmt_handle *HistoryBufferArray;
};

/* -------------------------------------------------------------------------
 * Synchronization objects (monitored fences).
 * ------------------------------------------------------------------------- */
/* D3DDDI_SYNCHRONIZATIONOBJECT_TYPE */
#define AGX_KMT_SYNC_MONITORED_FENCE 5

struct agx_kmt_syncobjinfo2 { /* D3DDDI_SYNCHRONIZATIONOBJECTINFO2 */
   int32_t Type;
   uint32_t Flags;
   union {
      struct {
         uint64_t InitialFenceValue;
         void *FenceValueCPUVirtualAddress;
         agx_kmt_gpuva FenceValueGPUVirtualAddress;
         uint32_t EngineAffinity;
         uint32_t Padding;
      } MonitoredFence;
      struct {
         uint64_t Reserved[8];
      } Reserved;
   };
   agx_kmt_handle SharedHandle;
};

struct agx_kmt_createsyncobj2 { /* D3DKMT_CREATESYNCHRONIZATIONOBJECT2 */
   agx_kmt_handle hDevice;
   struct agx_kmt_syncobjinfo2 Info;
   agx_kmt_handle hSyncObject;
};

struct agx_kmt_destroysyncobj { /* D3DKMT_DESTROYSYNCHRONIZATIONOBJECT */
   agx_kmt_handle hSyncObject;
};

struct agx_kmt_signalsyncfromcpu { /* D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMCPU */
   agx_kmt_handle hDevice;
   uint32_t ObjectCount;
   const agx_kmt_handle *ObjectHandleArray;
   const uint64_t *FenceValueArray;
   uint32_t Flags; /* D3DDDICB_SIGNALFLAGS */
};

/* D3DDDI_WAITFORSYNCHRONIZATIONOBJECTFROMCPU_FLAGS */
#define AGX_KMT_WAITCPU_WAITANY (1u << 0)

struct agx_kmt_waitsyncfromcpu { /* D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU */
   agx_kmt_handle hDevice;
   uint32_t ObjectCount;
   const agx_kmt_handle *ObjectHandleArray;
   const uint64_t *FenceValueArray;
   void *hAsyncEvent;
   uint32_t Flags;
};

struct agx_kmt_signalsyncfromgpu { /* D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU */
   agx_kmt_handle hContext;
   uint32_t ObjectCount;
   const agx_kmt_handle *ObjectHandleArray;
   union {
      const uint64_t *MonitoredFenceValueArray;
      uint64_t Reserved[8];
   };
};

struct agx_kmt_waitsyncfromgpu { /* D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMGPU */
   agx_kmt_handle hContext;
   uint32_t ObjectCount;
   const agx_kmt_handle *ObjectHandleArray;
   union {
      const uint64_t *MonitoredFenceValueArray;
      uint64_t FenceValue;
      uint64_t Reserved[8];
   };
};

/* -------------------------------------------------------------------------
 * Clock calibration (the GET_TIME replacement) and escape (unused; declared
 * so the fn table is complete and a future need does not have to re-derive
 * the ABI).
 * ------------------------------------------------------------------------- */
/*
 * DXGK_GPUCLOCKDATA lives inside d3dkmdt.h's `#pragma pack(push, 1)` region
 * (d3dkmdt.h:1493-2500, VERIFIED), so it is byte-packed: 28 bytes, alignment
 * 1, and D3DKMT_QUERYCLOCKCALIBRATION therefore places it at offset 12 with
 * no padding. Reproduce that exactly -- this is the one struct in this header
 * whose layout is not the natural one, and getting it wrong would silently
 * misread the GPU timestamp.
 */
#pragma pack(push, 1)
struct agx_kmt_gpuclockdata { /* DXGK_GPUCLOCKDATA */
   uint64_t GpuFrequency;
   uint64_t GpuClockCounter;
   uint64_t CpuClockCounter;
   uint32_t Flags;
};
#pragma pack(pop)

struct agx_kmt_queryclockcalibration { /* D3DKMT_QUERYCLOCKCALIBRATION */
   agx_kmt_handle hAdapter;
   uint32_t NodeOrdinal;
   uint32_t PhysicalAdapterIndex;
   struct agx_kmt_gpuclockdata ClockData;
};

struct agx_kmt_escape { /* D3DKMT_ESCAPE */
   agx_kmt_handle hAdapter;
   agx_kmt_handle hDevice;
   int32_t Type;
   uint32_t Flags;
   void *pPrivateDriverData;
   uint32_t PrivateDriverDataSize;
   agx_kmt_handle hContext;
};

/* -------------------------------------------------------------------------
 * The gdi32.dll entry-point table.
 *
 * Resolved with GetProcAddress rather than a static import so that (a) a
 * Windows build never fails to load merely because one thunk is absent on an
 * older OS, and (b) host tests on macOS can inject a mock table and exercise
 * every code path in this winsys that is not literally a syscall. There is
 * exactly one place that fills this in: agx_d3dkmt_load() in agx_d3dkmt.c.
 * ------------------------------------------------------------------------- */
#if defined(_WIN32)
#define AGX_KMT_API __stdcall
#else
#define AGX_KMT_API
#endif

struct agx_d3dkmt_fns {
   agx_kmt_status(AGX_KMT_API *EnumAdapters2)(struct agx_kmt_enumadapters2 *);
   agx_kmt_status(AGX_KMT_API *OpenAdapterFromLuid)(
      struct agx_kmt_openadapterfromluid *);
   agx_kmt_status(AGX_KMT_API *CloseAdapter)(const struct agx_kmt_closeadapter *);
   agx_kmt_status(AGX_KMT_API *QueryAdapterInfo)(
      struct agx_kmt_queryadapterinfo *);

   agx_kmt_status(AGX_KMT_API *CreateDevice)(struct agx_kmt_createdevice *);
   agx_kmt_status(AGX_KMT_API *DestroyDevice)(
      const struct agx_kmt_destroydevice *);

   agx_kmt_status(AGX_KMT_API *CreateAllocation2)(
      struct agx_kmt_createallocation *);
   agx_kmt_status(AGX_KMT_API *QueryResourceInfo)(
      struct agx_kmt_queryresourceinfo *);
   agx_kmt_status(AGX_KMT_API *QueryResourceInfoFromNtHandle)(
      struct agx_kmt_queryresourceinfofromnthandle *);
   agx_kmt_status(AGX_KMT_API *ShareObjects)(uint32_t,
                                             const agx_kmt_handle *, void *,
                                             uint32_t, void **);
   agx_kmt_status(AGX_KMT_API *OpenResource)(struct agx_kmt_openresource *);
   agx_kmt_status(AGX_KMT_API *OpenResourceFromNtHandle)(
      struct agx_kmt_openresourcefromnthandle *);
   agx_kmt_status(AGX_KMT_API *DestroyAllocation2)(
      const struct agx_kmt_destroyallocation2 *);
   agx_kmt_status(AGX_KMT_API *Lock2)(struct agx_kmt_lock2 *);
   agx_kmt_status(AGX_KMT_API *Unlock2)(const struct agx_kmt_unlock2 *);
   agx_kmt_status(AGX_KMT_API *InvalidateCache)(
      const struct agx_kmt_invalidatecache *);
   agx_kmt_status(AGX_KMT_API *MakeResident)(struct agx_kmt_makeresident *);
   agx_kmt_status(AGX_KMT_API *Evict)(struct agx_kmt_evict *);

   agx_kmt_status(AGX_KMT_API *CreatePagingQueue)(
      struct agx_kmt_createpagingqueue *);
   agx_kmt_status(AGX_KMT_API *DestroyPagingQueue)(
      struct agx_kmt_destroypagingqueue *);
   agx_kmt_status(AGX_KMT_API *ReserveGpuVirtualAddress)(
      struct agx_kmt_reservegpuva *);
   agx_kmt_status(AGX_KMT_API *MapGpuVirtualAddress)(struct agx_kmt_mapgpuva *);
   agx_kmt_status(AGX_KMT_API *UpdateGpuVirtualAddress)(
      const struct agx_kmt_updategpuva *);
   agx_kmt_status(AGX_KMT_API *FreeGpuVirtualAddress)(
      const struct agx_kmt_freegpuva *);

   agx_kmt_status(AGX_KMT_API *CreateContextVirtual)(
      struct agx_kmt_createcontextvirtual *);
   agx_kmt_status(AGX_KMT_API *DestroyContext)(
      const struct agx_kmt_destroycontext *);
   agx_kmt_status(AGX_KMT_API *SubmitCommand)(
      const struct agx_kmt_submitcommand *);

   agx_kmt_status(AGX_KMT_API *CreateSynchronizationObject2)(
      struct agx_kmt_createsyncobj2 *);
   agx_kmt_status(AGX_KMT_API *DestroySynchronizationObject)(
      const struct agx_kmt_destroysyncobj *);
   agx_kmt_status(AGX_KMT_API *SignalSynchronizationObjectFromCpu)(
      const struct agx_kmt_signalsyncfromcpu *);
   agx_kmt_status(AGX_KMT_API *WaitForSynchronizationObjectFromCpu)(
      const struct agx_kmt_waitsyncfromcpu *);
   agx_kmt_status(AGX_KMT_API *SignalSynchronizationObjectFromGpu)(
      const struct agx_kmt_signalsyncfromgpu *);
   agx_kmt_status(AGX_KMT_API *WaitForSynchronizationObjectFromGpu)(
      const struct agx_kmt_waitsyncfromgpu *);

   agx_kmt_status(AGX_KMT_API *QueryClockCalibration)(
      struct agx_kmt_queryclockcalibration *);
   agx_kmt_status(AGX_KMT_API *Escape)(const struct agx_kmt_escape *);
};

/*
 * Fill `fns` from gdi32.dll. Returns false if any entry point this winsys
 * requires is missing (i.e. the OS predates WDDM 2.0), leaving `fns` in an
 * unspecified state. Windows-only; on other hosts this is not compiled and
 * tests supply their own table.
 */
bool agx_d3dkmt_load(struct agx_d3dkmt_fns *fns);

#ifdef __cplusplus
}
#endif

/*
 * Copyright 2026 the Asahi Linux contributors
 * SPDX-License-Identifier: MIT
 */

/*
 * ABI conformance gate for agx_d3dkmt.h's vendored D3DKMT declarations.
 *
 * This TU is NEVER part of the Mesa build (no meson target references it).
 * It is compiled -- syntax-only, by the same aarch64-w64-mingw32 cross
 * compiler the real build uses -- by
 * gpu/honeykrisp-port/tests/run-host-tests.sh, which points -I at an
 * isolated copy of the three real Windows SDK headers
 * (shared/d3dkmthk.h, shared/d3dkmdt.h, shared/d3dukmdt.h). Those three
 * compile cleanly under llvm-mingw when isolated from the rest of the SDK,
 * which is what makes this check possible on a macOS host with no Windows.
 *
 * The real headers are included at global scope; agx_d3dkmt.h is included
 * inside `namespace vend`, so both sets of types coexist and every size and
 * field offset can be compared pairwise at compile time. A layout drift in
 * either direction is a hard compile error naming the exact field.
 *
 * If the SDK is not installed the script skips this gate loudly; the
 * portable-core tests (agx_d3dkmt_core_tests.c) still run.
 */

#include <windows.h>

#include <d3dkmthk.h>

#include <cstddef>
#include <cstdbool>
#include <cstdint>

namespace vend {
#include "agx_d3dkmt.h"
}

#define CHECK_SIZE(REAL, VEND)                                                 \
   static_assert(sizeof(REAL) == sizeof(vend::VEND),                           \
                 "sizeof mismatch: " #REAL " vs " #VEND)

#define CHECK_OFF(REAL, VEND, F)                                               \
   static_assert(offsetof(REAL, F) == offsetof(vend::VEND, F),                 \
                 "offset mismatch: " #REAL "." #F)

/* Base types. */
static_assert(sizeof(D3DKMT_HANDLE) == sizeof(vend::agx_kmt_handle), "handle");
static_assert(sizeof(D3DGPU_VIRTUAL_ADDRESS) == sizeof(vend::agx_kmt_gpuva),
              "gpuva");
static_assert(sizeof(D3DGPU_SIZE_T) == sizeof(vend::agx_kmt_gpusize), "gpusize");
static_assert(sizeof(NTSTATUS) == sizeof(vend::agx_kmt_status), "status");
CHECK_SIZE(LUID, agx_kmt_luid);
CHECK_OFF(LUID, agx_kmt_luid, LowPart);
CHECK_OFF(LUID, agx_kmt_luid, HighPart);
static_assert(D3DDDI_MAX_WRITTEN_PRIMARIES == AGX_KMT_MAX_WRITTEN_PRIMARIES,
              "max written primaries");
static_assert(D3DDDI_MAX_BROADCAST_CONTEXT == AGX_KMT_MAX_BROADCAST_CONTEXT,
              "max broadcast context");

/* Adapter. */
CHECK_SIZE(D3DKMT_ADAPTERINFO, agx_kmt_adapterinfo);
CHECK_OFF(D3DKMT_ADAPTERINFO, agx_kmt_adapterinfo, hAdapter);
CHECK_OFF(D3DKMT_ADAPTERINFO, agx_kmt_adapterinfo, AdapterLuid);
CHECK_OFF(D3DKMT_ADAPTERINFO, agx_kmt_adapterinfo, NumOfSources);
CHECK_OFF(D3DKMT_ADAPTERINFO, agx_kmt_adapterinfo,
          bPrecisePresentRegionsPreferred);

CHECK_SIZE(D3DKMT_ENUMADAPTERS2, agx_kmt_enumadapters2);
CHECK_OFF(D3DKMT_ENUMADAPTERS2, agx_kmt_enumadapters2, NumAdapters);
CHECK_OFF(D3DKMT_ENUMADAPTERS2, agx_kmt_enumadapters2, pAdapters);

CHECK_SIZE(D3DKMT_OPENADAPTERFROMLUID, agx_kmt_openadapterfromluid);
CHECK_OFF(D3DKMT_OPENADAPTERFROMLUID, agx_kmt_openadapterfromluid, AdapterLuid);
CHECK_OFF(D3DKMT_OPENADAPTERFROMLUID, agx_kmt_openadapterfromluid, hAdapter);

CHECK_SIZE(D3DKMT_CLOSEADAPTER, agx_kmt_closeadapter);

CHECK_SIZE(D3DKMT_QUERYADAPTERINFO, agx_kmt_queryadapterinfo);
CHECK_OFF(D3DKMT_QUERYADAPTERINFO, agx_kmt_queryadapterinfo, hAdapter);
CHECK_OFF(D3DKMT_QUERYADAPTERINFO, agx_kmt_queryadapterinfo, Type);
CHECK_OFF(D3DKMT_QUERYADAPTERINFO, agx_kmt_queryadapterinfo,
          pPrivateDriverData);
CHECK_OFF(D3DKMT_QUERYADAPTERINFO, agx_kmt_queryadapterinfo,
          PrivateDriverDataSize);
static_assert(KMTQAITYPE_UMDRIVERPRIVATE == AGX_KMT_QAITYPE_UMDRIVERPRIVATE,
              "KMTQAITYPE_UMDRIVERPRIVATE");
static_assert(KMTQAITYPE_ADAPTERTYPE == AGX_KMT_QAITYPE_ADAPTERTYPE,
              "KMTQAITYPE_ADAPTERTYPE");
CHECK_SIZE(D3DKMT_ADAPTERTYPE, agx_kmt_adaptertype);

/* Device. */
CHECK_SIZE(D3DKMT_CREATEDEVICE, agx_kmt_createdevice);
CHECK_OFF(D3DKMT_CREATEDEVICE, agx_kmt_createdevice, hAdapter);
CHECK_OFF(D3DKMT_CREATEDEVICE, agx_kmt_createdevice, Flags);
CHECK_OFF(D3DKMT_CREATEDEVICE, agx_kmt_createdevice, hDevice);
CHECK_OFF(D3DKMT_CREATEDEVICE, agx_kmt_createdevice, pCommandBuffer);
CHECK_OFF(D3DKMT_CREATEDEVICE, agx_kmt_createdevice, CommandBufferSize);
CHECK_OFF(D3DKMT_CREATEDEVICE, agx_kmt_createdevice, pAllocationList);
CHECK_OFF(D3DKMT_CREATEDEVICE, agx_kmt_createdevice, AllocationListSize);
CHECK_OFF(D3DKMT_CREATEDEVICE, agx_kmt_createdevice, pPatchLocationList);
CHECK_OFF(D3DKMT_CREATEDEVICE, agx_kmt_createdevice, PatchLocationListSize);
CHECK_SIZE(D3DKMT_DESTROYDEVICE, agx_kmt_destroydevice);

/* Allocations. */
CHECK_SIZE(D3DDDI_ALLOCATIONINFO2, agx_kmt_allocationinfo2);
CHECK_OFF(D3DDDI_ALLOCATIONINFO2, agx_kmt_allocationinfo2, hAllocation);
CHECK_OFF(D3DDDI_ALLOCATIONINFO2, agx_kmt_allocationinfo2, pSystemMem);
CHECK_OFF(D3DDDI_ALLOCATIONINFO2, agx_kmt_allocationinfo2, pPrivateDriverData);
CHECK_OFF(D3DDDI_ALLOCATIONINFO2, agx_kmt_allocationinfo2,
          PrivateDriverDataSize);
CHECK_OFF(D3DDDI_ALLOCATIONINFO2, agx_kmt_allocationinfo2, VidPnSourceId);
CHECK_OFF(D3DDDI_ALLOCATIONINFO2, agx_kmt_allocationinfo2, Flags);
CHECK_OFF(D3DDDI_ALLOCATIONINFO2, agx_kmt_allocationinfo2, GpuVirtualAddress);
CHECK_OFF(D3DDDI_ALLOCATIONINFO2, agx_kmt_allocationinfo2, Priority);
CHECK_OFF(D3DDDI_ALLOCATIONINFO2, agx_kmt_allocationinfo2, Reserved);

CHECK_SIZE(D3DKMT_CREATEALLOCATION, agx_kmt_createallocation);
CHECK_OFF(D3DKMT_CREATEALLOCATION, agx_kmt_createallocation, hDevice);
CHECK_OFF(D3DKMT_CREATEALLOCATION, agx_kmt_createallocation, hResource);
CHECK_OFF(D3DKMT_CREATEALLOCATION, agx_kmt_createallocation, hGlobalShare);
CHECK_OFF(D3DKMT_CREATEALLOCATION, agx_kmt_createallocation,
          pPrivateRuntimeData);
CHECK_OFF(D3DKMT_CREATEALLOCATION, agx_kmt_createallocation,
          PrivateRuntimeDataSize);
CHECK_OFF(D3DKMT_CREATEALLOCATION, agx_kmt_createallocation,
          pPrivateDriverData);
CHECK_OFF(D3DKMT_CREATEALLOCATION, agx_kmt_createallocation,
          PrivateDriverDataSize);
CHECK_OFF(D3DKMT_CREATEALLOCATION, agx_kmt_createallocation, NumAllocations);
CHECK_OFF(D3DKMT_CREATEALLOCATION, agx_kmt_createallocation, pAllocationInfo2);
CHECK_OFF(D3DKMT_CREATEALLOCATION, agx_kmt_createallocation, Flags);
CHECK_OFF(D3DKMT_CREATEALLOCATION, agx_kmt_createallocation,
          hPrivateRuntimeResourceHandle);

CHECK_SIZE(D3DDDI_OPENALLOCATIONINFO2, agx_kmt_openallocationinfo2);
CHECK_OFF(D3DDDI_OPENALLOCATIONINFO2, agx_kmt_openallocationinfo2,
          hAllocation);
CHECK_OFF(D3DDDI_OPENALLOCATIONINFO2, agx_kmt_openallocationinfo2,
          pPrivateDriverData);
CHECK_OFF(D3DDDI_OPENALLOCATIONINFO2, agx_kmt_openallocationinfo2,
          PrivateDriverDataSize);
CHECK_OFF(D3DDDI_OPENALLOCATIONINFO2, agx_kmt_openallocationinfo2,
          GpuVirtualAddress);
CHECK_OFF(D3DDDI_OPENALLOCATIONINFO2, agx_kmt_openallocationinfo2, Reserved);

CHECK_SIZE(D3DKMT_QUERYRESOURCEINFO, agx_kmt_queryresourceinfo);
CHECK_OFF(D3DKMT_QUERYRESOURCEINFO, agx_kmt_queryresourceinfo, hDevice);
CHECK_OFF(D3DKMT_QUERYRESOURCEINFO, agx_kmt_queryresourceinfo, hGlobalShare);
CHECK_OFF(D3DKMT_QUERYRESOURCEINFO, agx_kmt_queryresourceinfo,
          pPrivateRuntimeData);
CHECK_OFF(D3DKMT_QUERYRESOURCEINFO, agx_kmt_queryresourceinfo,
          PrivateRuntimeDataSize);
CHECK_OFF(D3DKMT_QUERYRESOURCEINFO, agx_kmt_queryresourceinfo,
          TotalPrivateDriverDataSize);
CHECK_OFF(D3DKMT_QUERYRESOURCEINFO, agx_kmt_queryresourceinfo,
          ResourcePrivateDriverDataSize);
CHECK_OFF(D3DKMT_QUERYRESOURCEINFO, agx_kmt_queryresourceinfo, NumAllocations);

CHECK_SIZE(D3DKMT_QUERYRESOURCEINFOFROMNTHANDLE,
           agx_kmt_queryresourceinfofromnthandle);
CHECK_OFF(D3DKMT_QUERYRESOURCEINFOFROMNTHANDLE,
          agx_kmt_queryresourceinfofromnthandle, hNtHandle);
CHECK_OFF(D3DKMT_QUERYRESOURCEINFOFROMNTHANDLE,
          agx_kmt_queryresourceinfofromnthandle, NumAllocations);

CHECK_SIZE(D3DKMT_OPENRESOURCE, agx_kmt_openresource);
CHECK_OFF(D3DKMT_OPENRESOURCE, agx_kmt_openresource, hDevice);
CHECK_OFF(D3DKMT_OPENRESOURCE, agx_kmt_openresource, hGlobalShare);
CHECK_OFF(D3DKMT_OPENRESOURCE, agx_kmt_openresource, NumAllocations);
CHECK_OFF(D3DKMT_OPENRESOURCE, agx_kmt_openresource, pOpenAllocationInfo2);
CHECK_OFF(D3DKMT_OPENRESOURCE, agx_kmt_openresource, pPrivateRuntimeData);
CHECK_OFF(D3DKMT_OPENRESOURCE, agx_kmt_openresource,
          pResourcePrivateDriverData);
CHECK_OFF(D3DKMT_OPENRESOURCE, agx_kmt_openresource,
          pTotalPrivateDriverDataBuffer);
CHECK_OFF(D3DKMT_OPENRESOURCE, agx_kmt_openresource, hResource);

CHECK_SIZE(D3DKMT_OPENRESOURCEFROMNTHANDLE,
           agx_kmt_openresourcefromnthandle);
CHECK_OFF(D3DKMT_OPENRESOURCEFROMNTHANDLE,
          agx_kmt_openresourcefromnthandle, hNtHandle);
CHECK_OFF(D3DKMT_OPENRESOURCEFROMNTHANDLE,
          agx_kmt_openresourcefromnthandle, pOpenAllocationInfo2);
CHECK_OFF(D3DKMT_OPENRESOURCEFROMNTHANDLE,
          agx_kmt_openresourcefromnthandle, pTotalPrivateDriverDataBuffer);
CHECK_OFF(D3DKMT_OPENRESOURCEFROMNTHANDLE,
          agx_kmt_openresourcefromnthandle, hResource);
CHECK_OFF(D3DKMT_OPENRESOURCEFROMNTHANDLE,
          agx_kmt_openresourcefromnthandle, hKeyedMutex);
CHECK_OFF(D3DKMT_OPENRESOURCEFROMNTHANDLE,
          agx_kmt_openresourcefromnthandle, hSyncObject);
static_assert(SHARED_ALLOCATION_ALL_ACCESS ==
                 AGX_KMT_SHARED_ALLOCATION_ALL_ACCESS,
              "SHARED_ALLOCATION_ALL_ACCESS");

CHECK_SIZE(D3DKMT_DESTROYALLOCATION2, agx_kmt_destroyallocation2);
CHECK_OFF(D3DKMT_DESTROYALLOCATION2, agx_kmt_destroyallocation2, hDevice);
CHECK_OFF(D3DKMT_DESTROYALLOCATION2, agx_kmt_destroyallocation2, hResource);
CHECK_OFF(D3DKMT_DESTROYALLOCATION2, agx_kmt_destroyallocation2,
          phAllocationList);
CHECK_OFF(D3DKMT_DESTROYALLOCATION2, agx_kmt_destroyallocation2,
          AllocationCount);
CHECK_OFF(D3DKMT_DESTROYALLOCATION2, agx_kmt_destroyallocation2, Flags);

CHECK_SIZE(D3DKMT_LOCK2, agx_kmt_lock2);
CHECK_OFF(D3DKMT_LOCK2, agx_kmt_lock2, hDevice);
CHECK_OFF(D3DKMT_LOCK2, agx_kmt_lock2, hAllocation);
CHECK_OFF(D3DKMT_LOCK2, agx_kmt_lock2, Flags);
CHECK_OFF(D3DKMT_LOCK2, agx_kmt_lock2, pData);
CHECK_SIZE(D3DKMT_UNLOCK2, agx_kmt_unlock2);
CHECK_SIZE(D3DKMT_INVALIDATECACHE, agx_kmt_invalidatecache);
CHECK_OFF(D3DKMT_INVALIDATECACHE, agx_kmt_invalidatecache, hDevice);
CHECK_OFF(D3DKMT_INVALIDATECACHE, agx_kmt_invalidatecache, hAllocation);
CHECK_OFF(D3DKMT_INVALIDATECACHE, agx_kmt_invalidatecache, Offset);
CHECK_OFF(D3DKMT_INVALIDATECACHE, agx_kmt_invalidatecache, Length);
CHECK_OFF(D3DKMT_UNLOCK2, agx_kmt_unlock2, hDevice);
CHECK_OFF(D3DKMT_UNLOCK2, agx_kmt_unlock2, hAllocation);

/* Residency. */
CHECK_SIZE(D3DDDI_MAKERESIDENT, agx_kmt_makeresident);
CHECK_OFF(D3DDDI_MAKERESIDENT, agx_kmt_makeresident, hPagingQueue);
CHECK_OFF(D3DDDI_MAKERESIDENT, agx_kmt_makeresident, NumAllocations);
CHECK_OFF(D3DDDI_MAKERESIDENT, agx_kmt_makeresident, AllocationList);
CHECK_OFF(D3DDDI_MAKERESIDENT, agx_kmt_makeresident, PriorityList);
CHECK_OFF(D3DDDI_MAKERESIDENT, agx_kmt_makeresident, Flags);
CHECK_OFF(D3DDDI_MAKERESIDENT, agx_kmt_makeresident, PagingFenceValue);
CHECK_OFF(D3DDDI_MAKERESIDENT, agx_kmt_makeresident, NumBytesToTrim);
CHECK_SIZE(D3DKMT_EVICT, agx_kmt_evict);
CHECK_OFF(D3DKMT_EVICT, agx_kmt_evict, NumBytesToTrim);

/* Paging queue / GPU VA. */
CHECK_SIZE(D3DKMT_CREATEPAGINGQUEUE, agx_kmt_createpagingqueue);
CHECK_OFF(D3DKMT_CREATEPAGINGQUEUE, agx_kmt_createpagingqueue, hDevice);
CHECK_OFF(D3DKMT_CREATEPAGINGQUEUE, agx_kmt_createpagingqueue, Priority);
CHECK_OFF(D3DKMT_CREATEPAGINGQUEUE, agx_kmt_createpagingqueue, hPagingQueue);
CHECK_OFF(D3DKMT_CREATEPAGINGQUEUE, agx_kmt_createpagingqueue, hSyncObject);
CHECK_OFF(D3DKMT_CREATEPAGINGQUEUE, agx_kmt_createpagingqueue,
          FenceValueCPUVirtualAddress);
CHECK_OFF(D3DKMT_CREATEPAGINGQUEUE, agx_kmt_createpagingqueue,
          PhysicalAdapterIndex);
CHECK_SIZE(D3DDDI_DESTROYPAGINGQUEUE, agx_kmt_destroypagingqueue);

CHECK_SIZE(D3DDDIGPUVIRTUALADDRESS_PROTECTION_TYPE, agx_kmt_gpuva_protection);
static_assert(D3DDDIGPUVIRTUALADDRESS_RESERVE_NO_ACCESS ==
                 AGX_KMT_GPUVA_RESERVE_NO_ACCESS,
              "RESERVE_NO_ACCESS");
static_assert(D3DDDIGPUVIRTUALADDRESS_RESERVE_ZERO == AGX_KMT_GPUVA_RESERVE_ZERO,
              "RESERVE_ZERO");
static_assert(D3DDDI_UPDATEGPUVIRTUALADDRESS_MAP == AGX_KMT_UPDATEGPUVA_MAP,
              "UPDATEGPUVA_MAP");
static_assert(D3DDDI_UPDATEGPUVIRTUALADDRESS_UNMAP == AGX_KMT_UPDATEGPUVA_UNMAP,
              "UPDATEGPUVA_UNMAP");
static_assert(D3DDDI_UPDATEGPUVIRTUALADDRESS_COPY == AGX_KMT_UPDATEGPUVA_COPY,
              "UPDATEGPUVA_COPY");
static_assert(D3DDDI_UPDATEGPUVIRTUALADDRESS_MAP_PROTECT ==
                 AGX_KMT_UPDATEGPUVA_MAP_PROTECT,
              "UPDATEGPUVA_MAP_PROTECT");

CHECK_SIZE(D3DDDI_UPDATEGPUVIRTUALADDRESS_OPERATION, agx_kmt_updategpuva_op);
CHECK_OFF(D3DDDI_UPDATEGPUVIRTUALADDRESS_OPERATION, agx_kmt_updategpuva_op,
          OperationType);
CHECK_OFF(D3DDDI_UPDATEGPUVIRTUALADDRESS_OPERATION, agx_kmt_updategpuva_op,
          MapProtect);
CHECK_OFF(D3DDDI_UPDATEGPUVIRTUALADDRESS_OPERATION, agx_kmt_updategpuva_op,
          Unmap);
CHECK_OFF(D3DDDI_UPDATEGPUVIRTUALADDRESS_OPERATION, agx_kmt_updategpuva_op,
          Copy);
static_assert(sizeof(((D3DDDI_UPDATEGPUVIRTUALADDRESS_OPERATION *)0)
                        ->MapProtect) ==
                 sizeof(((vend::agx_kmt_updategpuva_op *)0)->MapProtect),
              "MapProtect body");

CHECK_SIZE(D3DDDI_RESERVEGPUVIRTUALADDRESS, agx_kmt_reservegpuva);
CHECK_OFF(D3DDDI_RESERVEGPUVIRTUALADDRESS, agx_kmt_reservegpuva, hPagingQueue);
CHECK_OFF(D3DDDI_RESERVEGPUVIRTUALADDRESS, agx_kmt_reservegpuva, BaseAddress);
CHECK_OFF(D3DDDI_RESERVEGPUVIRTUALADDRESS, agx_kmt_reservegpuva, MinimumAddress);
CHECK_OFF(D3DDDI_RESERVEGPUVIRTUALADDRESS, agx_kmt_reservegpuva, MaximumAddress);
CHECK_OFF(D3DDDI_RESERVEGPUVIRTUALADDRESS, agx_kmt_reservegpuva, Size);
CHECK_OFF(D3DDDI_RESERVEGPUVIRTUALADDRESS, agx_kmt_reservegpuva,
          ReservationType);
CHECK_OFF(D3DDDI_RESERVEGPUVIRTUALADDRESS, agx_kmt_reservegpuva, VirtualAddress);
CHECK_OFF(D3DDDI_RESERVEGPUVIRTUALADDRESS, agx_kmt_reservegpuva,
          PagingFenceValue);

CHECK_SIZE(D3DDDI_MAPGPUVIRTUALADDRESS, agx_kmt_mapgpuva);
CHECK_OFF(D3DDDI_MAPGPUVIRTUALADDRESS, agx_kmt_mapgpuva, hPagingQueue);
CHECK_OFF(D3DDDI_MAPGPUVIRTUALADDRESS, agx_kmt_mapgpuva, BaseAddress);
CHECK_OFF(D3DDDI_MAPGPUVIRTUALADDRESS, agx_kmt_mapgpuva, hAllocation);
CHECK_OFF(D3DDDI_MAPGPUVIRTUALADDRESS, agx_kmt_mapgpuva, OffsetInPages);
CHECK_OFF(D3DDDI_MAPGPUVIRTUALADDRESS, agx_kmt_mapgpuva, SizeInPages);
CHECK_OFF(D3DDDI_MAPGPUVIRTUALADDRESS, agx_kmt_mapgpuva, Protection);
CHECK_OFF(D3DDDI_MAPGPUVIRTUALADDRESS, agx_kmt_mapgpuva, DriverProtection);
CHECK_OFF(D3DDDI_MAPGPUVIRTUALADDRESS, agx_kmt_mapgpuva, VirtualAddress);
CHECK_OFF(D3DDDI_MAPGPUVIRTUALADDRESS, agx_kmt_mapgpuva, PagingFenceValue);

CHECK_SIZE(D3DKMT_UPDATEGPUVIRTUALADDRESS, agx_kmt_updategpuva);
CHECK_OFF(D3DKMT_UPDATEGPUVIRTUALADDRESS, agx_kmt_updategpuva, hDevice);
CHECK_OFF(D3DKMT_UPDATEGPUVIRTUALADDRESS, agx_kmt_updategpuva, hContext);
CHECK_OFF(D3DKMT_UPDATEGPUVIRTUALADDRESS, agx_kmt_updategpuva, hFenceObject);
CHECK_OFF(D3DKMT_UPDATEGPUVIRTUALADDRESS, agx_kmt_updategpuva, NumOperations);
CHECK_OFF(D3DKMT_UPDATEGPUVIRTUALADDRESS, agx_kmt_updategpuva, Operations);
CHECK_OFF(D3DKMT_UPDATEGPUVIRTUALADDRESS, agx_kmt_updategpuva, FenceValue);
CHECK_OFF(D3DKMT_UPDATEGPUVIRTUALADDRESS, agx_kmt_updategpuva, Flags);

CHECK_SIZE(D3DKMT_FREEGPUVIRTUALADDRESS, agx_kmt_freegpuva);
CHECK_OFF(D3DKMT_FREEGPUVIRTUALADDRESS, agx_kmt_freegpuva, BaseAddress);
CHECK_OFF(D3DKMT_FREEGPUVIRTUALADDRESS, agx_kmt_freegpuva, Size);

/* Context / submission. */
CHECK_SIZE(D3DKMT_CREATECONTEXTVIRTUAL, agx_kmt_createcontextvirtual);
CHECK_OFF(D3DKMT_CREATECONTEXTVIRTUAL, agx_kmt_createcontextvirtual, hDevice);
CHECK_OFF(D3DKMT_CREATECONTEXTVIRTUAL, agx_kmt_createcontextvirtual,
          NodeOrdinal);
CHECK_OFF(D3DKMT_CREATECONTEXTVIRTUAL, agx_kmt_createcontextvirtual,
          EngineAffinity);
CHECK_OFF(D3DKMT_CREATECONTEXTVIRTUAL, agx_kmt_createcontextvirtual, Flags);
CHECK_OFF(D3DKMT_CREATECONTEXTVIRTUAL, agx_kmt_createcontextvirtual,
          pPrivateDriverData);
CHECK_OFF(D3DKMT_CREATECONTEXTVIRTUAL, agx_kmt_createcontextvirtual,
          PrivateDriverDataSize);
CHECK_OFF(D3DKMT_CREATECONTEXTVIRTUAL, agx_kmt_createcontextvirtual, ClientHint);
CHECK_OFF(D3DKMT_CREATECONTEXTVIRTUAL, agx_kmt_createcontextvirtual, hContext);
CHECK_SIZE(D3DKMT_DESTROYCONTEXT, agx_kmt_destroycontext);

CHECK_SIZE(D3DKMT_SUBMITCOMMAND, agx_kmt_submitcommand);
CHECK_OFF(D3DKMT_SUBMITCOMMAND, agx_kmt_submitcommand, Commands);
CHECK_OFF(D3DKMT_SUBMITCOMMAND, agx_kmt_submitcommand, CommandLength);
CHECK_OFF(D3DKMT_SUBMITCOMMAND, agx_kmt_submitcommand, Flags);
CHECK_OFF(D3DKMT_SUBMITCOMMAND, agx_kmt_submitcommand, PresentHistoryToken);
CHECK_OFF(D3DKMT_SUBMITCOMMAND, agx_kmt_submitcommand, BroadcastContextCount);
CHECK_OFF(D3DKMT_SUBMITCOMMAND, agx_kmt_submitcommand, BroadcastContext);
CHECK_OFF(D3DKMT_SUBMITCOMMAND, agx_kmt_submitcommand, pPrivateDriverData);
CHECK_OFF(D3DKMT_SUBMITCOMMAND, agx_kmt_submitcommand, PrivateDriverDataSize);
CHECK_OFF(D3DKMT_SUBMITCOMMAND, agx_kmt_submitcommand, NumPrimaries);
CHECK_OFF(D3DKMT_SUBMITCOMMAND, agx_kmt_submitcommand, WrittenPrimaries);
CHECK_OFF(D3DKMT_SUBMITCOMMAND, agx_kmt_submitcommand, NumHistoryBuffers);
CHECK_OFF(D3DKMT_SUBMITCOMMAND, agx_kmt_submitcommand, HistoryBufferArray);

/* Sync objects. */
static_assert(D3DDDI_MONITORED_FENCE == AGX_KMT_SYNC_MONITORED_FENCE,
              "MONITORED_FENCE");
CHECK_SIZE(D3DDDI_SYNCHRONIZATIONOBJECTINFO2, agx_kmt_syncobjinfo2);
CHECK_OFF(D3DDDI_SYNCHRONIZATIONOBJECTINFO2, agx_kmt_syncobjinfo2, Type);
CHECK_OFF(D3DDDI_SYNCHRONIZATIONOBJECTINFO2, agx_kmt_syncobjinfo2, Flags);
CHECK_OFF(D3DDDI_SYNCHRONIZATIONOBJECTINFO2, agx_kmt_syncobjinfo2,
          MonitoredFence);
CHECK_OFF(D3DDDI_SYNCHRONIZATIONOBJECTINFO2, agx_kmt_syncobjinfo2, SharedHandle);
static_assert(offsetof(D3DDDI_SYNCHRONIZATIONOBJECTINFO2,
                       MonitoredFence.FenceValueCPUVirtualAddress) ==
                 offsetof(vend::agx_kmt_syncobjinfo2,
                          MonitoredFence.FenceValueCPUVirtualAddress),
              "MonitoredFence.FenceValueCPUVirtualAddress");
static_assert(offsetof(D3DDDI_SYNCHRONIZATIONOBJECTINFO2,
                       MonitoredFence.InitialFenceValue) ==
                 offsetof(vend::agx_kmt_syncobjinfo2,
                          MonitoredFence.InitialFenceValue),
              "MonitoredFence.InitialFenceValue");

CHECK_SIZE(D3DKMT_CREATESYNCHRONIZATIONOBJECT2, agx_kmt_createsyncobj2);
CHECK_OFF(D3DKMT_CREATESYNCHRONIZATIONOBJECT2, agx_kmt_createsyncobj2, hDevice);
CHECK_OFF(D3DKMT_CREATESYNCHRONIZATIONOBJECT2, agx_kmt_createsyncobj2, Info);
CHECK_OFF(D3DKMT_CREATESYNCHRONIZATIONOBJECT2, agx_kmt_createsyncobj2,
          hSyncObject);
CHECK_SIZE(D3DKMT_DESTROYSYNCHRONIZATIONOBJECT, agx_kmt_destroysyncobj);

CHECK_SIZE(D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMCPU, agx_kmt_signalsyncfromcpu);
CHECK_OFF(D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMCPU, agx_kmt_signalsyncfromcpu,
          hDevice);
CHECK_OFF(D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMCPU, agx_kmt_signalsyncfromcpu,
          ObjectCount);
CHECK_OFF(D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMCPU, agx_kmt_signalsyncfromcpu,
          ObjectHandleArray);
CHECK_OFF(D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMCPU, agx_kmt_signalsyncfromcpu,
          FenceValueArray);
CHECK_OFF(D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMCPU, agx_kmt_signalsyncfromcpu,
          Flags);

CHECK_SIZE(D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU,
           agx_kmt_waitsyncfromcpu);
CHECK_OFF(D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU, agx_kmt_waitsyncfromcpu,
          hDevice);
CHECK_OFF(D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU, agx_kmt_waitsyncfromcpu,
          ObjectCount);
CHECK_OFF(D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU, agx_kmt_waitsyncfromcpu,
          ObjectHandleArray);
CHECK_OFF(D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU, agx_kmt_waitsyncfromcpu,
          FenceValueArray);
CHECK_OFF(D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU, agx_kmt_waitsyncfromcpu,
          hAsyncEvent);
CHECK_OFF(D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU, agx_kmt_waitsyncfromcpu,
          Flags);

CHECK_SIZE(D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU, agx_kmt_signalsyncfromgpu);
CHECK_OFF(D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU, agx_kmt_signalsyncfromgpu,
          hContext);
CHECK_OFF(D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU, agx_kmt_signalsyncfromgpu,
          ObjectHandleArray);
CHECK_OFF(D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU, agx_kmt_signalsyncfromgpu,
          MonitoredFenceValueArray);

CHECK_SIZE(D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMGPU, agx_kmt_waitsyncfromgpu);
CHECK_OFF(D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMGPU, agx_kmt_waitsyncfromgpu,
          hContext);
CHECK_OFF(D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMGPU, agx_kmt_waitsyncfromgpu,
          ObjectHandleArray);
CHECK_OFF(D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMGPU, agx_kmt_waitsyncfromgpu,
          MonitoredFenceValueArray);

/* Clock calibration / escape. */
CHECK_SIZE(DXGK_GPUCLOCKDATA, agx_kmt_gpuclockdata);
CHECK_OFF(DXGK_GPUCLOCKDATA, agx_kmt_gpuclockdata, GpuFrequency);
CHECK_OFF(DXGK_GPUCLOCKDATA, agx_kmt_gpuclockdata, GpuClockCounter);
CHECK_OFF(DXGK_GPUCLOCKDATA, agx_kmt_gpuclockdata, CpuClockCounter);
CHECK_OFF(DXGK_GPUCLOCKDATA, agx_kmt_gpuclockdata, Flags);
CHECK_SIZE(D3DKMT_QUERYCLOCKCALIBRATION, agx_kmt_queryclockcalibration);
CHECK_OFF(D3DKMT_QUERYCLOCKCALIBRATION, agx_kmt_queryclockcalibration,
          NodeOrdinal);
CHECK_OFF(D3DKMT_QUERYCLOCKCALIBRATION, agx_kmt_queryclockcalibration,
          PhysicalAdapterIndex);
CHECK_OFF(D3DKMT_QUERYCLOCKCALIBRATION, agx_kmt_queryclockcalibration,
          ClockData);

CHECK_SIZE(D3DKMT_ESCAPE, agx_kmt_escape);
CHECK_OFF(D3DKMT_ESCAPE, agx_kmt_escape, hAdapter);
CHECK_OFF(D3DKMT_ESCAPE, agx_kmt_escape, hDevice);
CHECK_OFF(D3DKMT_ESCAPE, agx_kmt_escape, Type);
CHECK_OFF(D3DKMT_ESCAPE, agx_kmt_escape, Flags);
CHECK_OFF(D3DKMT_ESCAPE, agx_kmt_escape, pPrivateDriverData);
CHECK_OFF(D3DKMT_ESCAPE, agx_kmt_escape, PrivateDriverDataSize);
CHECK_OFF(D3DKMT_ESCAPE, agx_kmt_escape, hContext);

/*
 * Every function pointer in the table must be assignment-compatible with the
 * SDK's own PFND3DKMT_* typedef. This is the check that would catch a wrong
 * argument type or a missing const, which no sizeof/offsetof pair can.
 */
static const struct {
   PFND3DKMT_ENUMADAPTERS2 a;
   PFND3DKMT_OPENADAPTERFROMLUID b;
   PFND3DKMT_CLOSEADAPTER c;
   PFND3DKMT_QUERYADAPTERINFO d;
   PFND3DKMT_CREATEDEVICE e;
   PFND3DKMT_DESTROYDEVICE f;
   PFND3DKMT_CREATEALLOCATION2 g;
   PFND3DKMT_QUERYRESOURCEINFO g1;
   PFND3DKMT_QUERYRESOURCEINFOFROMNTHANDLE g2;
   PFND3DKMT_SHAREOBJECTS g3;
   PFND3DKMT_OPENRESOURCE g4;
   PFND3DKMT_OPENRESOURCEFROMNTHANDLE g5;
   PFND3DKMT_DESTROYALLOCATION2 h;
   PFND3DKMT_LOCK2 i;
   PFND3DKMT_UNLOCK2 j;
   PFND3DKMT_INVALIDATECACHE j1;
   PFND3DKMT_MAKERESIDENT k;
   PFND3DKMT_EVICT l;
   PFND3DKMT_CREATEPAGINGQUEUE m;
   PFND3DKMT_DESTROYPAGINGQUEUE n;
   PFND3DKMT_RESERVEGPUVIRTUALADDRESS o;
   PFND3DKMT_MAPGPUVIRTUALADDRESS p;
   PFND3DKMT_UPDATEGPUVIRTUALADDRESS q;
   PFND3DKMT_FREEGPUVIRTUALADDRESS r;
   PFND3DKMT_CREATECONTEXTVIRTUAL s;
   PFND3DKMT_DESTROYCONTEXT t;
   PFND3DKMT_SUBMITCOMMAND u;
   PFND3DKMT_CREATESYNCHRONIZATIONOBJECT2 v;
   PFND3DKMT_DESTROYSYNCHRONIZATIONOBJECT w;
   PFND3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMCPU x;
   PFND3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU y;
   PFND3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU z;
   PFND3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMGPU aa;
   PFND3DKMT_QUERYCLOCKCALIBRATION ab;
   PFND3DKMT_ESCAPE ac;
} agx_d3dkmt_pfn_shape_check = {};

static_assert(sizeof(agx_d3dkmt_pfn_shape_check) ==
                 sizeof(vend::agx_d3dkmt_fns),
              "fn table arity: agx_d3dkmt_fns has a different number of "
              "entry points than this shape check enumerates");

int
main(void)
{
   return 0;
}

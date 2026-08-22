/*
 * Copyright 2026 the Asahi Linux contributors
 * SPDX-License-Identifier: MIT
 */

/*
 * The one place gdi32.dll's D3DKMT thunks are resolved.
 *
 * GetProcAddress rather than a static import, for two reasons that both
 * matter here:
 *
 *  - a static import makes the whole ICD fail to LOAD if any single thunk is
 *    absent, which turns "this Windows build predates one entry point" into
 *    "vulkan_asahi.dll cannot be loaded at all" with no diagnostic;
 *  - a function-pointer table is injectable, so the winsys above it can be
 *    exercised by host tests with a mock kernel. Without that seam, the only
 *    way to test any of this is on hardware that does not exist yet.
 *
 * gdi32.dll is already on build-windows.sh's reviewed import allowlist, and
 * GetModuleHandleW (not LoadLibrary) is used because gdi32 is guaranteed
 * already resident in any process that has loaded a Vulkan ICD.
 */

#include "agx_d3dkmt.h"

#include <windows.h>

bool
agx_d3dkmt_load(struct agx_d3dkmt_fns *fns)
{
   HMODULE gdi32 = GetModuleHandleW(L"gdi32.dll");
   if (gdi32 == NULL)
      gdi32 = LoadLibraryW(L"gdi32.dll");
   if (gdi32 == NULL)
      return false;

   memset(fns, 0, sizeof(*fns));

#define LOAD(field, name)                                                      \
   do {                                                                        \
      FARPROC _p = GetProcAddress(gdi32, name);                                \
      if (_p == NULL)                                                          \
         return false;                                                         \
      *(FARPROC *)(void *)&fns->field = _p;                                    \
   } while (0)

   LOAD(EnumAdapters2, "D3DKMTEnumAdapters2");
   LOAD(OpenAdapterFromLuid, "D3DKMTOpenAdapterFromLuid");
   LOAD(CloseAdapter, "D3DKMTCloseAdapter");
   LOAD(QueryAdapterInfo, "D3DKMTQueryAdapterInfo");

   LOAD(CreateDevice, "D3DKMTCreateDevice");
   LOAD(DestroyDevice, "D3DKMTDestroyDevice");

   LOAD(CreateAllocation2, "D3DKMTCreateAllocation2");
   LOAD(QueryResourceInfo, "D3DKMTQueryResourceInfo");
   LOAD(QueryResourceInfoFromNtHandle,
        "D3DKMTQueryResourceInfoFromNtHandle");
   LOAD(ShareObjects, "D3DKMTShareObjects");
   LOAD(OpenResource, "D3DKMTOpenResource");
   LOAD(OpenResourceFromNtHandle, "D3DKMTOpenResourceFromNtHandle");
   LOAD(DestroyAllocation2, "D3DKMTDestroyAllocation2");
   LOAD(Lock2, "D3DKMTLock2");
   LOAD(Unlock2, "D3DKMTUnlock2");
   LOAD(InvalidateCache, "D3DKMTInvalidateCache");
   LOAD(MakeResident, "D3DKMTMakeResident");
   LOAD(Evict, "D3DKMTEvict");

   LOAD(CreatePagingQueue, "D3DKMTCreatePagingQueue");
   LOAD(DestroyPagingQueue, "D3DKMTDestroyPagingQueue");
   LOAD(ReserveGpuVirtualAddress, "D3DKMTReserveGpuVirtualAddress");
   LOAD(MapGpuVirtualAddress, "D3DKMTMapGpuVirtualAddress");
   LOAD(UpdateGpuVirtualAddress, "D3DKMTUpdateGpuVirtualAddress");
   LOAD(FreeGpuVirtualAddress, "D3DKMTFreeGpuVirtualAddress");

   LOAD(CreateContextVirtual, "D3DKMTCreateContextVirtual");
   LOAD(DestroyContext, "D3DKMTDestroyContext");
   LOAD(SubmitCommand, "D3DKMTSubmitCommand");

   LOAD(CreateSynchronizationObject2, "D3DKMTCreateSynchronizationObject2");
   LOAD(DestroySynchronizationObject, "D3DKMTDestroySynchronizationObject");
   LOAD(SignalSynchronizationObjectFromCpu,
        "D3DKMTSignalSynchronizationObjectFromCpu");
   LOAD(WaitForSynchronizationObjectFromCpu,
        "D3DKMTWaitForSynchronizationObjectFromCpu");
   LOAD(SignalSynchronizationObjectFromGpu,
        "D3DKMTSignalSynchronizationObjectFromGpu");
   LOAD(WaitForSynchronizationObjectFromGpu,
        "D3DKMTWaitForSynchronizationObjectFromGpu");

   LOAD(QueryClockCalibration, "D3DKMTQueryClockCalibration");
   LOAD(Escape, "D3DKMTEscape");

#undef LOAD

   return true;
}

/*
 * Copyright 2026 the Asahi Linux contributors
 * SPDX-License-Identifier: MIT
 */
#pragma once

/*
 * Runtime-device bridge for the Honeykrisp D3DKMT winsys.
 *
 * When enabled, agx_d3dkmt_open_device() does NOT call D3DKMTCreateDevice.
 * Instead it retrieves the D3D runtime's kernel callback table + runtime
 * device handle from the Aurora UMD (AuroraD3D11Triton.dll's
 * AuroraGpuBridgeAcquire* exports), reserves a per-device provider slot, and
 * installs that slot's callback-backed implementations of every DEVICE-scope
 * entry in struct agx_d3dkmt_fns so the winsys renders on the runtime's device
 * -- one VM, one ASID, the allocation the runtime scans out. ADAPTER-scope
 * operations (enumerate, identity, GET_PARAMS/GET_TIME escape) keep using the
 * gdi32 thunks on Honeykrisp's own adapter handle.
 *
 * MULTI-DEVICE: keyed per (runtime device handle / callback table), NOT per
 * process. DWM creates eight D3D devices and each may open more than one
 * winsys; every vkCreateDevice binds to the D3D device that is actually
 * creating it (AuroraGpuBridgeAcquireCurrentThread), gets its own provider slot
 * with its own context-token table, and tears down independently. The registry
 * and per-provider state live in agxw_bridge_core.{h,c}, which is host-tested.
 *
 * Default is OFF: without HONEYKRISP_RT_DEVICE set, none of this is reached and
 * the winsys is byte-for-byte the shipping D3DKMTCreateDevice path.
 */

#include <stdbool.h>
#include <stdint.h>

#include "agxw_bridge_core.h" /* the ws->device sentinel helpers + registry */

struct agx_d3dkmt_winsys;

#ifdef __cplusplus
extern "C" {
#endif

/* Is the runtime-device bridge requested (HONEYKRISP_RT_DEVICE set nonzero)? */
bool agx_d3dkmt_bridge_enabled(void);

/*
 * Reserve a provider slot, acquire the runtime callbacks + device handle from
 * the Aurora UMD, and install this slot's callback-backed device-scope entries
 * into ws->fns. Sets ws->device to the slot's sentinel and ws->device_lease_held
 * to false. Returns false (leaving ws->fns as the gdi32 table) if the UMD is
 * not present, its ABI does not match, no bridge is published for this thread,
 * or every provider slot is in use. Call AFTER agx_d3dkmt_load(&ws->fns).
 */
bool agx_d3dkmt_bridge_install(struct agx_d3dkmt_winsys *ws);

/* Bytes still free in the shared AGBP carveout, or 0 if unavailable.
 * Adapter-scoped AGXW_ESC_GET_POOL escape, throttled internally. Lets the ICD
 * report a heap budget that means something instead of a synthetic constant. */
struct agx_device;
uint64_t agx_d3dkmt_pool_free_bytes(struct agx_device *dev);

/* Release this ws's provider slot (recovered from ws->device's sentinel) and
 * clear its context-token table. Touches no other device's slot. Safe to call
 * when ws is not a bridge device. */
void agx_d3dkmt_bridge_uninstall(struct agx_d3dkmt_winsys *ws);

/* The runtime device handle a bridge winsys is installed on, or NULL if ws is
 * not a bridge device. This is callback/provider identity, not GPU-VA-root
 * identity: WDDM gives every device in one process/adapter the same TTBR0. */
const void *agx_d3dkmt_bridge_rtdevice(const struct agx_d3dkmt_winsys *ws);

/* Process-local GPU-VA-root identity. The bridge DLL's globals are already
 * process scoped, so the adapter LUID uniquely names the WDDM process root
 * shared by all hRTDevices on that adapter. This is the key used by
 * agx_bridge_vma; never key a GPU-VA allocator by hRTDevice. */
uint64_t agx_d3dkmt_bridge_vm_key(const struct agx_d3dkmt_winsys *ws);

/*
 * Record the synchronous completion of an asynchronous paging operation.
 * The callback bridge records the callback result and fence value; the winsys
 * calls this after its monitored-fence wait so GUI-only processes leave an
 * actionable, console-free record when the callback succeeded but completion
 * did not. A no-op for a non-bridge winsys.
 */
enum agx_d3dkmt_bridge_wait_stage {
   AGX_D3DKMT_BRIDGE_WAIT_MAKE_RESIDENT = 1,
   AGX_D3DKMT_BRIDGE_WAIT_UPDATE_GPUVA = 2,
};

void agx_d3dkmt_bridge_log_paging_wait(
   const struct agx_d3dkmt_winsys *ws,
   enum agx_d3dkmt_bridge_wait_stage stage, uint64_t fence_value, int result);

#ifdef __cplusplus
}
#endif

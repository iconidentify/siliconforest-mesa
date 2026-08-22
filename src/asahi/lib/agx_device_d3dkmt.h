/*
 * Copyright 2026 the Asahi Linux contributors
 * SPDX-License-Identifier: MIT
 */
#pragma once

/*
 * The Windows winsys backend for agx_device_ops_t, talking D3DKMT to a WDDM
 * miniport instead of DeviceIoControl to a private ioctl node.
 *
 * WHY: a private ioctl node (\\.\AGXKMD) and dxgkrnl are two kernel
 * interfaces to one GPU. That means two allocators, two GPU VA regimes and
 * two handle namespaces, so a Vulkan image and a D3D texture can never be the
 * same object, no primary can be shared, and DXGI interop is structurally
 * impossible. Vendor Vulkan ICDs on ordinary Windows go through D3DKMT for
 * exactly this reason -- it is what makes VK_KHR_external_memory_win32 and
 * D3D<->Vulkan interop work at all. This backend puts Honeykrisp behind the
 * same kernel interface a D3D UMD uses.
 *
 * LAYERING: everything here lives in src/asahi/lib, the layer Honeykrisp and
 * the Gallium `asahi` driver share, and nothing in it is Vulkan-shaped. A
 * D3D-side consumer (a d3d10umd frontend over Gallium asahi) uses the same
 * two entry points below.
 *
 * OPEN CONTRACT: unlike agx_win_open_device(), which takes a pre-opened NT
 * HANDLE from the caller's CreateFileW, opening is done HERE, keyed by
 * adapter LUID. The reasons are structural, not stylistic:
 *
 *   - "opening" is now five D3DKMT calls plus a caps parse
 *     (EnumAdapters2 -> QueryAdapterInfo -> OpenAdapterFromLuid ->
 *     CreateDevice -> CreatePagingQueue -> ReserveGpuVirtualAddress), not one
 *     CreateFileW. Duplicating that in every consumer guarantees drift.
 *   - the LUID is the identifier a D3D consumer already has. DXGI hands
 *     IDXGIAdapter::GetDesc().AdapterLuid to the UMD, and the D3D10/11 UMD
 *     DDI is given its adapter by the runtime. Keying on LUID means the D3D
 *     path calls agx_d3dkmt_open_device() with an identifier it was given,
 *     while Honeykrisp -- which is handed nothing -- calls
 *     agx_d3dkmt_enumerate_adapters() first.
 *   - the LUID is also the identity Vulkan must report
 *     (VkPhysicalDeviceIDProperties::deviceLUID), which is what lets an
 *     application match a VkPhysicalDevice to a D3D device. A raw NT file
 *     handle carries no such identity.
 */

#include <stdbool.h>
#include <stdint.h>

#include "agx_d3dkmt.h"
#include "agx_d3dkmt_core.h"
#include "agx_device.h"

struct util_sync_provider;

/*
 * Fill `out` with the LUIDs of every WDDM adapter whose
 * KMTQAITYPE_UMDRIVERPRIVATE caps blob identifies it as an AGX GPU this
 * winsys understands. Writes at most `capacity` entries; *count always
 * receives the number found (which may exceed `capacity`). Returns false only
 * if D3DKMT itself is unavailable -- "no AGX adapter present" is a successful
 * enumeration of zero.
 */
bool agx_d3dkmt_enumerate_adapters(struct agx_kmt_luid *out, uint32_t capacity,
                                   uint32_t *count);

/*
 * Open `luid` and install the D3DKMT ops on `dev`. Mirrors
 * agx_virtio_open_device()'s contract: on success the caller runs
 * agx_open_device(memctx, dev) next for the backend-agnostic bring-up
 * (GET_PARAMS, VM_CREATE, zero/scratch/printf BOs, ...).
 */
bool agx_d3dkmt_open_device(struct agx_device *dev,
                            const struct agx_kmt_luid *luid);

/*
 * Tear down what agx_d3dkmt_open_device() built. Call AFTER
 * agx_close_device(), which still owns the BO cache and VMA heaps.
 */
void agx_d3dkmt_close_device(struct agx_device *dev);

/* The adapter LUID `dev` was opened from, for VkPhysicalDeviceIDProperties
 * and for matching against a D3D adapter. */
struct agx_kmt_luid agx_d3dkmt_device_luid(const struct agx_device *dev);

/* Resource-backed external memory. `handle` is an owning NT HANDLE for
 * OPAQUE_WIN32 and the zero-extended, non-owning hGlobalShare identifier for
 * OPAQUE_WIN32_KMT. Import never closes either kind. Export returns a fresh
 * owning NT HANDLE for OPAQUE_WIN32 and the retained identifier for KMT. */
struct agx_bo *agx_d3dkmt_bo_create_external(
   struct agx_device *dev, size_t size, unsigned align,
   enum agx_bo_flags flags,
   enum agx_d3dkmt_external_memory_type external_type, const char *label);

struct agx_bo *agx_d3dkmt_bo_import_external(
   struct agx_device *dev, uintptr_t handle,
   enum agx_d3dkmt_external_memory_type external_type, const char *label);

/* Same-process import of a runtime-owned allocation. The returned BO owns
 * only its GPUVA binding and wrapper; allocation destruction remains with the
 * D3D runtime that created `allocation`. */
struct agx_bo *agx_d3dkmt_bo_import_local(
   struct agx_device *dev, agx_kmt_handle allocation, size_t size,
   const char *label);

int agx_d3dkmt_bo_export_external(
   struct agx_device *dev, struct agx_bo *bo,
   enum agx_d3dkmt_external_memory_type external_type, uintptr_t *handle);

/*
 * The Windows util_sync_provider, backed by D3DKMT monitored fences.
 *
 * Built from `dev` rather than from a bare handle (unlike
 * util_sync_provider_ntagx) because it MUST share the syncobj table with the
 * winsys: drm_asahi_submit carries syncobj handles, so submit() has to be
 * able to resolve the same handles this provider mints. The provider does not
 * own `dev` and must be finalized before agx_d3dkmt_close_device().
 */
struct util_sync_provider *util_sync_provider_d3dkmt(struct agx_device *dev);

/* Capture the recent real-submit dependency chain when a signal-only empty
 * submit finds its queue timeline behind.  Level 0/1 builds make this a
 * no-op, so production carries no logging or counter cost. */
void agx_d3dkmt_diag_sync_gap(struct agx_device *dev, uint32_t queue_id,
                             uint32_t syncobj, uint64_t requested,
                             uint64_t observed);

extern const agx_device_ops_t agx_device_d3dkmt_ops;

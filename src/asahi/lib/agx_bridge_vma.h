/*
 * Copyright 2026 the Asahi Linux contributors
 * SPDX-License-Identifier: MIT
 */
#pragma once

/*
 * Shared GPU-VA arena for the runtime-device bridge.
 *
 * WHY: WDDM owns GPU VA at process/adapter scope. Every runtime device has a
 * distinct KMD device/ASID, but SetRootPageTable binds those ASIDs to the same
 * process TTBR0. Honeykrisp's runtime-device bridge therefore has one GPU-VA
 * address space even when DWM creates several hRTDevices. However,
 * Honeykrisp's per-device VA allocator (agx_va.c over dev->main_heap /
 * dev->usc_heap) assumes it privately owns the whole VM: each device hands out
 * the SAME addresses and Honeykrisp bakes them into the command stream, so in a
 * shared VM device #2..#N map their allocations over device #1's -- and because
 * agx_device.c power-of-two-rounds the heap base, a per-device offset cannot
 * disjoin them. Measured on hardware (0.1.331): LogonUI's 16 devices fail every
 * 4 MiB DXVK chunk while DWM's 2 mostly survive.
 *
 * FIX: one process root => one VA allocator. All bridge devices on the same
 * process/adapter share ONE main_heap and ONE usc_heap (refcounted), so dynamic
 * allocations are globally disjoint. An exact AGX_VA_FIXED collision fails:
 * that flag is Vulkan capture-replay/sparse reservation, and VA equality does
 * not prove backing equality.
 *
 * Honeykrisp's zero/scratch/printf bootstrap BOs bypass agx_va.c and bind
 * explicit absolute aliases. Because those aliases are content-defined and one
 * process root has one live mapping per alias, agx_bridge_vma_bootstrap_bind()
 * lets the FIRST device bind and DEFERS every identical-shape sibling (same
 * addr/size/flags) without binding -- the mapping already exists. A
 * mismatched-shape claim is still refused. When the bound owner exits with
 * deferred siblings live, teardown HANDS THE ALIAS OVER: it binds a successor's
 * identical-shape backing before the departing backing is destroyed, so the
 * shared root never holds a dangling bootstrap PTE. (This replaces the earlier
 * fail-closed model, which killed every concurrent sibling's device open at the
 * zero page -- measured as 48x BOOTSTRAP COLLISION on 0.1.336 -- and is NOT the
 * retired same-address adoption: only an exact-shape claim of a known alias
 * defers; the capture-replay/sparse AGX_VA_FIXED path keeps failing closed in
 * the arena allocator.)
 *
 * This is consulted from agx_va.c only for devices agx_bridge_vma_register()
 * has joined, so the non-bridge path (and the AGXKMD build, which never sees
 * this header) is byte-for-byte unchanged.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

struct agx_device;

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Console-free diagnostic. Appends a line to
 * C:\ProgramData\AuroraSilicon\honeykrisp-va.log so VA reservation/allocation
 * outcomes are readable from GUI processes (DWM/LogonUI have no stderr).
 *
 * Gated by HONEYKRISP_VA_LOG, in the same style as HONEYKRISP_RT_DEVICE:
 * unset == ON (the historical behaviour), "0" or empty == OFF.  Each line
 * costs a CreateDirectory/CreateFile/WriteFile/CloseHandle round trip on the
 * runtime's allocation path, which is why it is switchable at all.
 */
bool agx_bridge_valog_enabled(void);
void agx_bridge_valog(const char *fmt, ...);

/*
 * Join the shared VA arena for dev's process-local adapter root, seeding it
 * (on the first device) with the heap bounds Honeykrisp computed. Self-gating:
 * a no-op unless dev is a d3dkmt bridge device with HONEYKRISP_RT_DEVICE set,
 * so agx_device.c can call it unconditionally in its Windows path. Idempotent
 * per dev. Returns false for a bridge device that cannot safely join (table
 * exhaustion, invalid root identity, or heap-bound mismatch). Non-bridge
 * devices return true without registering.
 */
bool agx_bridge_vma_register(struct agx_device *dev, uint64_t main_start,
                             uint64_t main_size, uint64_t usc_start,
                             uint64_t usc_size);

/* Leave the shared arena; the heaps are freed when the last device leaves. */
void agx_bridge_vma_unregister(struct agx_device *dev);

/* True iff dev has joined a shared arena (fast pointer-membership check, safe
 * for any device including non-bridge ones). */
bool agx_bridge_vma_registered(const struct agx_device *dev);

/*
 * Allocate from the shared arena. size_B already includes Honeykrisp's guard.
 * flags mirrors enum agx_va_flags (only AGX_VA_USC and AGX_VA_FIXED are read).
 * AGX_VA_FIXED claims are exclusive and collisions fail. Returns the GPU VA,
 * or 0 on failure (logged).
 */
uint64_t agx_bridge_vma_alloc(struct agx_device *dev, uint64_t size_B,
                              uint64_t align_B, unsigned flags,
                              uint64_t fixed_va);

/* Release a VA obtained from agx_bridge_vma_alloc. */
void agx_bridge_vma_free(struct agx_device *dev, uint64_t addr, uint64_t size_B,
                         unsigned flags);

struct agx_bo;

/* Serialize and identify a zero/scratch/printf absolute bind. For a bridge
 * device, any overlapping claim not owned by this exact dev/BO mapping fails
 * before agx_bo_bind can overwrite a process-root PTE. For non-bridge devices
 * this is a transparent agx_bo_bind call. */
int agx_bridge_vma_bootstrap_bind(struct agx_device *dev, struct agx_bo *bo,
                                  uint64_t addr, uint64_t size_B,
                                  uint64_t bo_offset, unsigned bind_flags,
                                  const char *label);

/* Unbind every bootstrap alias owned by dev. Idempotent and also called by
 * unregister as a defensive lifetime boundary. */
void agx_bridge_vma_bootstrap_unbind_all(struct agx_device *dev);

#ifdef __cplusplus
}
#endif

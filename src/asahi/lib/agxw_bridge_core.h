/*
 * Copyright 2026 the Asahi Linux contributors
 * SPDX-License-Identifier: MIT
 */
#pragma once

/*
 * Portable core of the runtime-device bridge: the per-device provider registry
 * and the per-provider context token<->handle table.
 *
 * WHY A SEPARATE CORE, exactly as agx_d3dkmt_core.{h,c} is separate: it has no
 * Windows and no Mesa dependency, so the multi-device slot management and the
 * context-token bookkeeping -- the parts most likely to leak or cross wires
 * between two live devices -- are unit-tested natively on a host with no GPU
 * (agxw_bridge_core_tests.c). The Windows half (agx_d3dkmt_bridge.c) adds only
 * the SRW locking and the actual callback invocations around these functions.
 *
 * THREADING: every function here is pure data-structure manipulation and takes
 * NO lock. The Windows caller serializes: one process-wide lock around the
 * registry alloc/free, and one per-provider lock around that provider's
 * context-token operations. Keeping the lock out of the core is what lets the
 * host test drive it directly.
 *
 * Handles are stored as anonymous pointers (void and const void) so this file
 * never names a Windows type; the Windows half casts them to the DDI callback
 * table and the runtime device handle.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Generously bounded, per process. DWM creates 8 D3D devices today and each
 * may open more than one Honeykrisp winsys; 64 slots is ample headroom, and
 * exhaustion fails gracefully (a device open returns an error) rather than
 * crashing or aliasing another device. */
#define AGXW_BRIDGE_MAX_DEVICES 64u
#define AGXW_BRIDGE_MAX_CONTEXTS 64u
#define AGXW_BRIDGE_MAX_RESOURCES 65536u

/* Context token layout (a 32-bit agx_kmt_handle the winsys stores in place of
 * the runtime's pointer-width context HANDLE):
 *
 *   bits 31..28  0xC       marker: recognizably not a real D3DKMT handle
 *   bits 27..20  slot      the owning provider's slot -- makes a token from one
 *                          device never resolve in another, defence in depth
 *   bits 19..0   sequence  monotonic within the provider (no ABA reuse)
 *
 * A token is therefore globally unique across all live providers in the
 * process, not merely within one. */
#define AGXW_BRIDGE_CTX_TOKEN_MARKER 0xC0000000u
#define AGXW_BRIDGE_CTX_TOKEN_SLOT_SHIFT 20u
#define AGXW_BRIDGE_CTX_TOKEN_SEQ_MASK 0x000FFFFFu

/* The callback consumes a pointer-width hRTResource, while the D3DKMT winsys
 * records a 32-bit hResource.  The callback may legitimately return no
 * hKMResource, so bridge-owned tokens keep those handle domains distinct. */
#define AGXW_BRIDGE_RESOURCE_TOKEN_MARKER 0xD0000000u
#define AGXW_BRIDGE_RESOURCE_TOKEN_SLOT_SHIFT 20u
#define AGXW_BRIDGE_RESOURCE_TOKEN_SEQ_MASK 0x000FFFFFu

/*
 * Device sentinel: agx_d3dkmt_open_device stores this in ws->device in bridge
 * mode, with the provider slot in the low 8 bits, so teardown recovers the
 * slot with no new field in struct agx_d3dkmt_winsys. The device-scope shims
 * never read a device handle out of an argument (they use the provider's
 * runtime device handle), so encoding the slot here is safe.
 */
#define AGXW_BRIDGE_DEVICE_SENTINEL_BASE 0xB113D600u
#define AGXW_BRIDGE_DEVICE_SENTINEL_MASK 0xFFFFFF00u

static inline bool
agxw_bridge_is_sentinel(uint32_t dev)
{
   return (dev & AGXW_BRIDGE_DEVICE_SENTINEL_MASK) ==
          AGXW_BRIDGE_DEVICE_SENTINEL_BASE;
}

static inline uint32_t
agxw_bridge_sentinel_for_slot(unsigned slot)
{
   return AGXW_BRIDGE_DEVICE_SENTINEL_BASE | (uint32_t)(slot & 0xFFu);
}

static inline unsigned
agxw_bridge_slot_of_sentinel(uint32_t dev)
{
   return (unsigned)(dev & 0xFFu);
}

struct agxw_bridge_ctx_slot {
   uint32_t token;
   void *handle; /* the runtime's pointer-width context HANDLE */
   bool in_use;
};

struct agxw_bridge_resource_slot {
   uint32_t token;
   uint32_t hKMResource; /* callback output; may be zero */
   uint32_t hAllocation;
   void *hRTResource; /* callback input; owned by the Triton resource */
   bool in_use;
};

struct agxw_bridge_provider_core {
   bool in_use;
   unsigned slot;    /* own slot index, encoded into this provider's tokens */
   const void *cb;   /* const struct agxw_ddi_devicecallbacks * */
   void *hRTDevice;  /* the runtime device handle */
   /* Publisher-owned revocation cell (AuroraGpuBridge v2): before every
    * callback the Windows half compares *alive against alive_value; a
    * mismatch means the runtime device was destroyed (or the slot re-let)
    * and the call must degrade to device-removed instead of executing a
    * stale table. Opaque here; the core never dereferences it. */
   const void *alive;
   uint64_t alive_value;
   /* Publisher-owned in-flight counter (AuroraGpuBridge v3). Held across every
    * callback so unpublish can drain rather than merely revoke. Opaque here;
    * the core never dereferences it. */
   const void *in_flight;
   uint32_t ctx_seq; /* monotonic token source within this provider */
   uint32_t resource_seq;
   struct agxw_bridge_ctx_slot ctx[AGXW_BRIDGE_MAX_CONTEXTS];
   struct agxw_bridge_resource_slot *resources;
   uint32_t resource_capacity;
};

struct agxw_bridge_registry {
   struct agxw_bridge_provider_core prov[AGXW_BRIDGE_MAX_DEVICES];
};

/* -------------------------------------------------------------------------
 * Provider registry (caller holds the registry lock).
 * ------------------------------------------------------------------------- */

/* Reserve a free slot for (cb, hRTDevice). Returns the slot index, or -1 when
 * all AGXW_BRIDGE_MAX_DEVICES are in use. */
int agxw_bridge_provider_alloc(struct agxw_bridge_registry *reg,
                               const void *cb, void *hRTDevice);

/* Release a slot and clear its state (including its context table). A no-op if
 * the slot is out of range or not in use, so double-free is harmless. */
void agxw_bridge_provider_free(struct agxw_bridge_registry *reg,
                               unsigned slot);

/* The provider at a slot, or NULL if the slot is out of range or not in use. */
struct agxw_bridge_provider_core *
agxw_bridge_provider_get(struct agxw_bridge_registry *reg, unsigned slot);

/* Count of live providers (diagnostic / test). */
unsigned agxw_bridge_provider_count(const struct agxw_bridge_registry *reg);

/* -------------------------------------------------------------------------
 * Per-provider context token table (caller holds that provider's ctx lock).
 * ------------------------------------------------------------------------- */

/* Map a runtime context HANDLE to a fresh 32-bit token unique within this
 * provider. Returns 0 when the provider's context table is full. */
uint32_t agxw_bridge_ctx_alloc(struct agxw_bridge_provider_core *p,
                               void *handle);

/* The HANDLE for a token, or NULL if the token is not live in this provider.
 * A token minted by another provider is never found here -- tokens do not
 * cross providers. */
void *agxw_bridge_ctx_lookup(const struct agxw_bridge_provider_core *p,
                             uint32_t token);

/* Release a token. Returns true if it was live, false otherwise. */
bool agxw_bridge_ctx_free(struct agxw_bridge_provider_core *p, uint32_t token);

/* Count of live contexts in a provider (diagnostic / test). */
unsigned agxw_bridge_ctx_count(const struct agxw_bridge_provider_core *p);

/* Per-provider resource-token table (caller holds the resource lock). */
uint32_t agxw_bridge_resource_alloc(struct agxw_bridge_provider_core *p,
                                    void *hRTResource,
                                    uint32_t hKMResource,
                                    uint32_t hAllocation);
const struct agxw_bridge_resource_slot *
agxw_bridge_resource_lookup(const struct agxw_bridge_provider_core *p,
                            uint32_t token);
bool agxw_bridge_resource_free(struct agxw_bridge_provider_core *p,
                               uint32_t token);
unsigned agxw_bridge_resource_count(const struct agxw_bridge_provider_core *p);

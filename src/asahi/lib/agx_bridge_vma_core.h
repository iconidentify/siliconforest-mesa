/*
 * Copyright 2026 the Asahi Linux contributors
 * SPDX-License-Identifier: MIT
 */
#pragma once

/*
 * Portable ownership model for absolute bootstrap GPU-VA aliases.
 *
 * The Windows wrapper serializes access and performs the actual bind/unbind.
 * This core deliberately knows nothing about Windows, Mesa, or D3DKMT so the
 * important rules are host-testable.
 *
 * MODEL (v2, after the 0.1.336 measurement: 48x "BOOTSTRAP COLLISION
 * kind=zero" was every sibling device's open dying at the zero page):
 *
 * One process root has ONE live mapping per bootstrap alias, but MANY sibling
 * agx_devices legitimately construct an identical-shape backing for it --
 * every device runs the same agx_device.c bootstrap and the aliases are
 * content-defined (zero: read-only zeros; scratch: write-only bit bucket;
 * printf: a diagnostics ring whose consumer follows the owner). So:
 *
 *   - The FIRST claimant binds its backing and becomes the owner.
 *   - A sibling whose claim is IDENTICAL IN SHAPE (same addr, same size, same
 *     bind flags) is recorded as a DEFERRED claimant and must NOT bind: the
 *     mapping already exists and any identical-shape backing is equivalent.
 *   - When the owner releases while deferred claimants remain, release
 *     PROMOTES a successor: the caller must bind the successor's backing over
 *     the alias BEFORE the departing owner's backing is destroyed, so the
 *     shared root never holds a dangling bootstrap PTE.
 *   - A claim that merely OVERLAPS, or matches the address with a different
 *     size or different flags, is still a hard COLLISION. This is NOT the
 *     retired "adoption" model: VA equality alone proves nothing; only an
 *     exact-shape claim of a known bootstrap alias defers, and the
 *     capture-replay/sparse AGX_VA_FIXED path keeps failing closed in the
 *     arena allocator, untouched by this file.
 */

#include <stdbool.h>
#include <stdint.h>

#define AGX_BRIDGE_BOOTSTRAP_MAX 3u
/* Sibling devices per alias per process root. DWM was measured at 14 live
 * Honeykrisp devices in one process (0.1.336); registration capacity upstream
 * is 256, so match it and fail gracefully past that. */
#define AGX_BRIDGE_BOOTSTRAP_CLAIMANTS 256u

struct agx_bridge_bootstrap_claimant {
   const void *owner;   /* the claiming agx_device */
   const void *backing; /* its identical-shape backing (opaque agx_bo*) */
   uint64_t allocation; /* its allocation identity, for diagnostics */
   bool in_use;
};

struct agx_bridge_bootstrap_mapping {
   const void *owner;         /* device whose backing is currently bound */
   const void *owner_backing; /* opaque agx_bo* of the bound backing */
   const void *rtdevice;      /* diagnostic: owner's runtime device */
   uint64_t allocation;       /* diagnostic: bound backing's allocation */
   uint64_t addr;
   uint64_t size;
   unsigned flags; /* bind flags; part of the claim shape */
   bool in_use;
   struct agx_bridge_bootstrap_claimant
      deferred[AGX_BRIDGE_BOOTSTRAP_CLAIMANTS];
};

struct agx_bridge_bootstrap_core {
   struct agx_bridge_bootstrap_mapping map[AGX_BRIDGE_BOOTSTRAP_MAX];
};

enum agx_bridge_bootstrap_claim_result {
   AGX_BRIDGE_BOOTSTRAP_CLAIM_INVALID = -3,
   AGX_BRIDGE_BOOTSTRAP_CLAIM_FULL = -2,
   AGX_BRIDGE_BOOTSTRAP_CLAIM_COLLISION = -1,
   AGX_BRIDGE_BOOTSTRAP_CLAIM_SAME = 0,     /* same owner, same backing */
   AGX_BRIDGE_BOOTSTRAP_CLAIM_NEW = 1,      /* caller must bind */
   AGX_BRIDGE_BOOTSTRAP_CLAIM_DEFERRED = 2, /* caller must NOT bind */
};

enum agx_bridge_bootstrap_release_result {
   AGX_BRIDGE_BOOTSTRAP_RELEASE_NOT_FOUND = 0,
   AGX_BRIDGE_BOOTSTRAP_RELEASE_DEFERRED = 1, /* claimant left; no action */
   AGX_BRIDGE_BOOTSTRAP_RELEASE_LAST = 2,     /* caller must unbind */
   AGX_BRIDGE_BOOTSTRAP_RELEASE_HANDOVER = 3, /* caller must bind successor
                                                 BEFORE owner backing dies */
};

enum agx_bridge_bootstrap_claim_result
agx_bridge_bootstrap_claim(struct agx_bridge_bootstrap_core *core,
                           const void *owner, const void *rtdevice,
                           uint64_t allocation, const void *backing,
                           uint64_t addr, uint64_t size, unsigned flags,
                           const struct agx_bridge_bootstrap_mapping **conflict);

/*
 * Release owner's stake in the alias at [addr, addr+size).
 *
 * RELEASE_HANDOVER: *successor is filled with the promoted claimant (already
 * recorded as the new owner); the caller binds successor->backing over the
 * alias. If that bind fails, agx_bridge_bootstrap_demote_owner() drops the
 * failed successor and agx_bridge_bootstrap_promote_next() yields the next
 * one (false = none left, mapping cleared, caller unbinds).
 */
enum agx_bridge_bootstrap_release_result
agx_bridge_bootstrap_release(struct agx_bridge_bootstrap_core *core,
                             const void *owner, uint64_t addr, uint64_t size,
                             struct agx_bridge_bootstrap_claimant *successor);

/* Drop the CURRENT owner of the alias (used when a promoted successor's bind
 * failed). Returns true if the mapping existed and the owner was removed. */
bool agx_bridge_bootstrap_demote_owner(struct agx_bridge_bootstrap_core *core,
                                       uint64_t addr, uint64_t size);

/*
 * After demote_owner: promote the next deferred claimant into the owner seat
 * (fills *successor, returns true; the caller binds its backing). When no
 * claimant remains the mapping is CLEARED and this returns false; the caller
 * must unbind the stale PTE.
 */
bool agx_bridge_bootstrap_promote_next(
   struct agx_bridge_bootstrap_core *core, uint64_t addr, uint64_t size,
   struct agx_bridge_bootstrap_claimant *successor);

unsigned agx_bridge_bootstrap_count(
   const struct agx_bridge_bootstrap_core *core);

/* Live deferred claimants of the alias, for diagnostics/tests. */
unsigned agx_bridge_bootstrap_claimant_count(
   const struct agx_bridge_bootstrap_core *core, uint64_t addr, uint64_t size);

/* The backing a given device has recorded for the alias (as owner or deferred
 * claimant), or NULL if it has no stake. Lets the caller name a COLLISION's
 * cause precisely: same-shape collisions only happen when a device re-claims
 * with a DIFFERENT backing than the one it recorded. */
const void *agx_bridge_bootstrap_backing_of(
   const struct agx_bridge_bootstrap_core *core, uint64_t addr, uint64_t size,
   const void *owner);

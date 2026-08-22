/*
 * Copyright 2026 the Asahi Linux contributors
 * SPDX-License-Identifier: MIT
 */

#include "agx_bridge_vma_core.h"

#include <stddef.h>
#include <string.h>

static bool
interval_valid(uint64_t addr, uint64_t size)
{
   return addr != 0 && size != 0 && addr <= UINT64_MAX - size;
}

static bool
intervals_overlap(uint64_t a, uint64_t a_size, uint64_t b, uint64_t b_size)
{
   return a < b + b_size && b < a + a_size;
}

static struct agx_bridge_bootstrap_mapping *
mapping_find(struct agx_bridge_bootstrap_core *core, uint64_t addr,
             uint64_t size)
{
   for (unsigned i = 0; i < AGX_BRIDGE_BOOTSTRAP_MAX; ++i) {
      struct agx_bridge_bootstrap_mapping *m = &core->map[i];
      if (m->in_use && m->addr == addr && m->size == size)
         return m;
   }
   return NULL;
}

enum agx_bridge_bootstrap_claim_result
agx_bridge_bootstrap_claim(struct agx_bridge_bootstrap_core *core,
                           const void *owner, const void *rtdevice,
                           uint64_t allocation, const void *backing,
                           uint64_t addr, uint64_t size, unsigned flags,
                           const struct agx_bridge_bootstrap_mapping **conflict)
{
   if (conflict != NULL)
      *conflict = NULL;
   if (core == NULL || owner == NULL || rtdevice == NULL || allocation == 0 ||
       backing == NULL || !interval_valid(addr, size))
      return AGX_BRIDGE_BOOTSTRAP_CLAIM_INVALID;

   int free_slot = -1;
   for (unsigned i = 0; i < AGX_BRIDGE_BOOTSTRAP_MAX; ++i) {
      struct agx_bridge_bootstrap_mapping *m = &core->map[i];
      if (!m->in_use) {
         if (free_slot < 0)
            free_slot = (int)i;
         continue;
      }
      if (!intervals_overlap(addr, size, m->addr, m->size))
         continue;

      /* An overlapping claim with a different shape (partial overlap, or a
       * different size / different bind flags at the same base) can never be
       * the same content-defined alias. */
      if (m->addr != addr || m->size != size || m->flags != flags) {
         if (conflict != NULL)
            *conflict = m;
         return AGX_BRIDGE_BOOTSTRAP_CLAIM_COLLISION;
      }

      if (m->owner == owner && m->allocation == allocation &&
          m->owner_backing == backing)
         return AGX_BRIDGE_BOOTSTRAP_CLAIM_SAME;

      /* A device claims each alias at most once; a repeat with a DIFFERENT
       * backing from the same device is a caller bug, not a sibling. */
      if (m->owner == owner) {
         if (conflict != NULL)
            *conflict = m;
         return AGX_BRIDGE_BOOTSTRAP_CLAIM_COLLISION;
      }
      for (unsigned j = 0; j < AGX_BRIDGE_BOOTSTRAP_CLAIMANTS; ++j) {
         const struct agx_bridge_bootstrap_claimant *c = &m->deferred[j];
         if (c->in_use && c->owner == owner) {
            if (c->backing == backing && c->allocation == allocation)
               return AGX_BRIDGE_BOOTSTRAP_CLAIM_SAME;
            if (conflict != NULL)
               *conflict = m;
            return AGX_BRIDGE_BOOTSTRAP_CLAIM_COLLISION;
         }
      }

      /* Identical shape, different sibling device: defer. */
      for (unsigned j = 0; j < AGX_BRIDGE_BOOTSTRAP_CLAIMANTS; ++j) {
         struct agx_bridge_bootstrap_claimant *c = &m->deferred[j];
         if (!c->in_use) {
            c->owner = owner;
            c->backing = backing;
            c->allocation = allocation;
            c->in_use = true;
            return AGX_BRIDGE_BOOTSTRAP_CLAIM_DEFERRED;
         }
      }
      return AGX_BRIDGE_BOOTSTRAP_CLAIM_FULL;
   }

   if (free_slot < 0)
      return AGX_BRIDGE_BOOTSTRAP_CLAIM_FULL;

   struct agx_bridge_bootstrap_mapping *m = &core->map[free_slot];
   memset(m, 0, sizeof(*m));
   m->owner = owner;
   m->owner_backing = backing;
   m->rtdevice = rtdevice;
   m->allocation = allocation;
   m->addr = addr;
   m->size = size;
   m->flags = flags;
   m->in_use = true;
   return AGX_BRIDGE_BOOTSTRAP_CLAIM_NEW;
}

enum agx_bridge_bootstrap_release_result
agx_bridge_bootstrap_release(struct agx_bridge_bootstrap_core *core,
                             const void *owner, uint64_t addr, uint64_t size,
                             struct agx_bridge_bootstrap_claimant *successor)
{
   if (successor != NULL)
      memset(successor, 0, sizeof(*successor));
   if (core == NULL || owner == NULL || !interval_valid(addr, size))
      return AGX_BRIDGE_BOOTSTRAP_RELEASE_NOT_FOUND;

   struct agx_bridge_bootstrap_mapping *m = mapping_find(core, addr, size);
   if (m == NULL)
      return AGX_BRIDGE_BOOTSTRAP_RELEASE_NOT_FOUND;

   if (m->owner != owner) {
      /* A deferred claimant leaving: drop its record, mapping unaffected. */
      for (unsigned j = 0; j < AGX_BRIDGE_BOOTSTRAP_CLAIMANTS; ++j) {
         struct agx_bridge_bootstrap_claimant *c = &m->deferred[j];
         if (c->in_use && c->owner == owner) {
            memset(c, 0, sizeof(*c));
            return AGX_BRIDGE_BOOTSTRAP_RELEASE_DEFERRED;
         }
      }
      return AGX_BRIDGE_BOOTSTRAP_RELEASE_NOT_FOUND;
   }

   /* The bound owner is leaving: promote the first deferred claimant so its
    * identical-shape backing can replace the mapping before the departing
    * backing is destroyed. */
   for (unsigned j = 0; j < AGX_BRIDGE_BOOTSTRAP_CLAIMANTS; ++j) {
      struct agx_bridge_bootstrap_claimant *c = &m->deferred[j];
      if (!c->in_use)
         continue;
      if (successor != NULL)
         *successor = *c;
      m->owner = c->owner;
      m->owner_backing = c->backing;
      m->allocation = c->allocation;
      memset(c, 0, sizeof(*c));
      return AGX_BRIDGE_BOOTSTRAP_RELEASE_HANDOVER;
   }

   memset(m, 0, sizeof(*m));
   return AGX_BRIDGE_BOOTSTRAP_RELEASE_LAST;
}

bool
agx_bridge_bootstrap_demote_owner(struct agx_bridge_bootstrap_core *core,
                                  uint64_t addr, uint64_t size)
{
   if (core == NULL || !interval_valid(addr, size))
      return false;
   struct agx_bridge_bootstrap_mapping *m = mapping_find(core, addr, size);
   if (m == NULL || m->owner == NULL)
      return false;
   m->owner = NULL;
   m->owner_backing = NULL;
   m->allocation = 0;
   return true;
}

bool
agx_bridge_bootstrap_promote_next(
   struct agx_bridge_bootstrap_core *core, uint64_t addr, uint64_t size,
   struct agx_bridge_bootstrap_claimant *successor)
{
   if (successor != NULL)
      memset(successor, 0, sizeof(*successor));
   if (core == NULL || !interval_valid(addr, size))
      return false;
   struct agx_bridge_bootstrap_mapping *m = mapping_find(core, addr, size);
   if (m == NULL)
      return false;
   if (m->owner != NULL)
      return false; /* only meaningful after demote_owner */

   for (unsigned j = 0; j < AGX_BRIDGE_BOOTSTRAP_CLAIMANTS; ++j) {
      struct agx_bridge_bootstrap_claimant *c = &m->deferred[j];
      if (!c->in_use)
         continue;
      if (successor != NULL)
         *successor = *c;
      m->owner = c->owner;
      m->owner_backing = c->backing;
      m->allocation = c->allocation;
      memset(c, 0, sizeof(*c));
      return true;
   }

   memset(m, 0, sizeof(*m));
   return false;
}

unsigned
agx_bridge_bootstrap_count(const struct agx_bridge_bootstrap_core *core)
{
   unsigned count = 0;
   if (core == NULL)
      return 0;
   for (unsigned i = 0; i < AGX_BRIDGE_BOOTSTRAP_MAX; ++i)
      count += core->map[i].in_use ? 1u : 0u;
   return count;
}

const void *
agx_bridge_bootstrap_backing_of(
   const struct agx_bridge_bootstrap_core *core, uint64_t addr, uint64_t size,
   const void *owner)
{
   if (core == NULL || owner == NULL || !interval_valid(addr, size))
      return NULL;
   for (unsigned i = 0; i < AGX_BRIDGE_BOOTSTRAP_MAX; ++i) {
      const struct agx_bridge_bootstrap_mapping *m = &core->map[i];
      if (!m->in_use || m->addr != addr || m->size != size)
         continue;
      if (m->owner == owner)
         return m->owner_backing;
      for (unsigned j = 0; j < AGX_BRIDGE_BOOTSTRAP_CLAIMANTS; ++j)
         if (m->deferred[j].in_use && m->deferred[j].owner == owner)
            return m->deferred[j].backing;
      return NULL;
   }
   return NULL;
}

unsigned
agx_bridge_bootstrap_claimant_count(
   const struct agx_bridge_bootstrap_core *core, uint64_t addr, uint64_t size)
{
   if (core == NULL || !interval_valid(addr, size))
      return 0;
   for (unsigned i = 0; i < AGX_BRIDGE_BOOTSTRAP_MAX; ++i) {
      const struct agx_bridge_bootstrap_mapping *m = &core->map[i];
      if (!m->in_use || m->addr != addr || m->size != size)
         continue;
      unsigned count = 0;
      for (unsigned j = 0; j < AGX_BRIDGE_BOOTSTRAP_CLAIMANTS; ++j)
         count += m->deferred[j].in_use ? 1u : 0u;
      return count;
   }
   return 0;
}

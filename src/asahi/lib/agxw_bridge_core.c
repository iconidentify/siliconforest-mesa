/*
 * Copyright 2026 the Asahi Linux contributors
 * SPDX-License-Identifier: MIT
 */

#include "agxw_bridge_core.h"

#include <stdlib.h>
#include <string.h>

int
agxw_bridge_provider_alloc(struct agxw_bridge_registry *reg, const void *cb,
                           void *hRTDevice)
{
   if (reg == NULL || cb == NULL || hRTDevice == NULL)
      return -1;

   for (unsigned i = 0; i < AGXW_BRIDGE_MAX_DEVICES; ++i) {
      if (!reg->prov[i].in_use) {
         memset(&reg->prov[i], 0, sizeof(reg->prov[i]));
         reg->prov[i].in_use = true;
         reg->prov[i].slot = i;
         reg->prov[i].cb = cb;
         reg->prov[i].hRTDevice = hRTDevice;
         return (int)i;
      }
   }
   return -1;
}

void
agxw_bridge_provider_free(struct agxw_bridge_registry *reg, unsigned slot)
{
   if (reg == NULL || slot >= AGXW_BRIDGE_MAX_DEVICES)
      return;
   free(reg->prov[slot].resources);
   memset(&reg->prov[slot], 0, sizeof(reg->prov[slot]));
}

struct agxw_bridge_provider_core *
agxw_bridge_provider_get(struct agxw_bridge_registry *reg, unsigned slot)
{
   if (reg == NULL || slot >= AGXW_BRIDGE_MAX_DEVICES ||
       !reg->prov[slot].in_use)
      return NULL;
   return &reg->prov[slot];
}

unsigned
agxw_bridge_provider_count(const struct agxw_bridge_registry *reg)
{
   unsigned n = 0;
   if (reg == NULL)
      return 0;
   for (unsigned i = 0; i < AGXW_BRIDGE_MAX_DEVICES; ++i)
      if (reg->prov[i].in_use)
         ++n;
   return n;
}

uint32_t
agxw_bridge_ctx_alloc(struct agxw_bridge_provider_core *p, void *handle)
{
   if (p == NULL || handle == NULL)
      return 0;

   for (unsigned i = 0; i < AGXW_BRIDGE_MAX_CONTEXTS; ++i) {
      if (!p->ctx[i].in_use) {
         /* Monotonic within the provider so a freed-then-reused table slot
          * never reissues a token value that could still be held stale; the
          * provider slot is folded in so the token is unique across every live
          * provider in the process, not merely within this one. */
         p->ctx_seq = (p->ctx_seq + 1u) & AGXW_BRIDGE_CTX_TOKEN_SEQ_MASK;
         uint32_t token =
            AGXW_BRIDGE_CTX_TOKEN_MARKER |
            ((uint32_t)(p->slot & 0xFFu) << AGXW_BRIDGE_CTX_TOKEN_SLOT_SHIFT) |
            (p->ctx_seq & AGXW_BRIDGE_CTX_TOKEN_SEQ_MASK);
         p->ctx[i].token = token;
         p->ctx[i].handle = handle;
         p->ctx[i].in_use = true;
         return token;
      }
   }
   return 0;
}

void *
agxw_bridge_ctx_lookup(const struct agxw_bridge_provider_core *p,
                       uint32_t token)
{
   if (p == NULL || token == 0)
      return NULL;
   for (unsigned i = 0; i < AGXW_BRIDGE_MAX_CONTEXTS; ++i)
      if (p->ctx[i].in_use && p->ctx[i].token == token)
         return p->ctx[i].handle;
   return NULL;
}

bool
agxw_bridge_ctx_free(struct agxw_bridge_provider_core *p, uint32_t token)
{
   if (p == NULL || token == 0)
      return false;
   for (unsigned i = 0; i < AGXW_BRIDGE_MAX_CONTEXTS; ++i) {
      if (p->ctx[i].in_use && p->ctx[i].token == token) {
         p->ctx[i].in_use = false;
         p->ctx[i].handle = NULL;
         p->ctx[i].token = 0;
         return true;
      }
   }
   return false;
}

unsigned
agxw_bridge_ctx_count(const struct agxw_bridge_provider_core *p)
{
   unsigned n = 0;
   if (p == NULL)
      return 0;
   for (unsigned i = 0; i < AGXW_BRIDGE_MAX_CONTEXTS; ++i)
      if (p->ctx[i].in_use)
         ++n;
   return n;
}

uint32_t
agxw_bridge_resource_alloc(struct agxw_bridge_provider_core *p,
                           void *hRTResource, uint32_t hKMResource,
                           uint32_t hAllocation)
{
   if (p == NULL || hRTResource == NULL || hAllocation == 0)
      return 0;

   for (uint32_t i = 0; i < p->resource_capacity; ++i) {
      if (!p->resources[i].in_use) {
         p->resource_seq =
            (p->resource_seq + 1u) & AGXW_BRIDGE_RESOURCE_TOKEN_SEQ_MASK;
         if (p->resource_seq == 0)
            p->resource_seq = 1;
         const uint32_t token =
            AGXW_BRIDGE_RESOURCE_TOKEN_MARKER |
            ((uint32_t)(p->slot & 0xFFu)
             << AGXW_BRIDGE_RESOURCE_TOKEN_SLOT_SHIFT) |
            p->resource_seq;
         p->resources[i].token = token;
         p->resources[i].hKMResource = hKMResource;
         p->resources[i].hAllocation = hAllocation;
         p->resources[i].hRTResource = hRTResource;
         p->resources[i].in_use = true;
         return token;
      }
   }

   if (p->resource_capacity >= AGXW_BRIDGE_MAX_RESOURCES)
      return 0;
   uint32_t old_capacity = p->resource_capacity;
   uint32_t new_capacity = old_capacity == 0 ? 64u : old_capacity * 2u;
   if (new_capacity > AGXW_BRIDGE_MAX_RESOURCES)
      new_capacity = AGXW_BRIDGE_MAX_RESOURCES;
   struct agxw_bridge_resource_slot *grown =
      realloc(p->resources, (size_t)new_capacity * sizeof(*grown));
   if (grown == NULL)
      return 0;
   memset(grown + old_capacity, 0,
          (size_t)(new_capacity - old_capacity) * sizeof(*grown));
   p->resources = grown;
   p->resource_capacity = new_capacity;
   return agxw_bridge_resource_alloc(
      p, hRTResource, hKMResource, hAllocation);
}

const struct agxw_bridge_resource_slot *
agxw_bridge_resource_lookup(const struct agxw_bridge_provider_core *p,
                            uint32_t token)
{
   if (p == NULL || token == 0)
      return NULL;
   for (uint32_t i = 0; i < p->resource_capacity; ++i)
      if (p->resources[i].in_use && p->resources[i].token == token)
         return &p->resources[i];
   return NULL;
}

bool
agxw_bridge_resource_free(struct agxw_bridge_provider_core *p, uint32_t token)
{
   if (p == NULL || token == 0)
      return false;
   for (uint32_t i = 0; i < p->resource_capacity; ++i) {
      if (p->resources[i].in_use && p->resources[i].token == token) {
         memset(&p->resources[i], 0, sizeof(p->resources[i]));
         return true;
      }
   }
   return false;
}

unsigned
agxw_bridge_resource_count(const struct agxw_bridge_provider_core *p)
{
   unsigned n = 0;
   if (p == NULL)
      return 0;
   for (uint32_t i = 0; i < p->resource_capacity; ++i)
      if (p->resources[i].in_use)
         ++n;
   return n;
}

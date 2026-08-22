/*
 * Copyright 2026 the Asahi Linux contributors
 * SPDX-License-Identifier: MIT
 */

/*
 * Portable half of the D3DKMT winsys -- see agx_d3dkmt_core.h for why this is
 * a separate, Mesa-free, Windows-free translation unit.
 */

#include "agx_d3dkmt_core.h"

#include <stdlib.h>
#include <string.h>

int
agx_d3dkmt_read_range_add(struct agxw_submit_read_range *ranges,
                          uint32_t *count, uint32_t capacity,
                          uint64_t gpu_va, uint64_t length)
{
   uint64_t new_end;
   uint32_t first = 0;

   if (ranges == NULL || count == NULL || *count > capacity || length == 0 ||
       gpu_va > UINT64_MAX - length ||
       !agxw_va_range_is_user(gpu_va, length))
      return AGX_D3DKMT_ERR_INVAL;
   new_end = gpu_va + length;

   while (first < *count) {
      uint64_t end = ranges[first].gpu_va + ranges[first].length;
      if (end >= gpu_va)
         break;
      first++;
   }

   if (first == *count || new_end < ranges[first].gpu_va) {
      if (*count == capacity)
         return AGX_D3DKMT_ERR_NOSPACE;
      memmove(&ranges[first + 1], &ranges[first],
              (size_t)(*count - first) * sizeof(*ranges));
      ranges[first] = (struct agxw_submit_read_range){gpu_va, length};
      (*count)++;
      return AGX_D3DKMT_OK;
   }

   uint64_t merged_start = gpu_va < ranges[first].gpu_va
                              ? gpu_va
                              : ranges[first].gpu_va;
   uint64_t merged_end = new_end;
   uint64_t first_end = ranges[first].gpu_va + ranges[first].length;
   if (first_end > merged_end)
      merged_end = first_end;

   uint32_t last = first + 1;
   while (last < *count && ranges[last].gpu_va <= merged_end) {
      uint64_t end = ranges[last].gpu_va + ranges[last].length;
      if (end > merged_end)
         merged_end = end;
      last++;
   }
   ranges[first].gpu_va = merged_start;
   ranges[first].length = merged_end - merged_start;
   if (last > first + 1) {
      memmove(&ranges[first + 1], &ranges[last],
              (size_t)(*count - last) * sizeof(*ranges));
      *count -= last - (first + 1);
   }
   return AGX_D3DKMT_OK;
}

int
agx_d3dkmt_read_ranges_compact(struct agxw_submit_read_range *ranges,
                               uint32_t *count, uint32_t capacity,
                               uint64_t *bridged_bytes)
{
   if (ranges == NULL || count == NULL || (*count != 0 && capacity == 0))
      return AGX_D3DKMT_ERR_INVAL;

   uint64_t bridged = 0;
   for (uint32_t i = 0; i < *count; i++) {
      if (ranges[i].length == 0 ||
          ranges[i].gpu_va > UINT64_MAX - ranges[i].length ||
          !agxw_va_range_is_user(ranges[i].gpu_va, ranges[i].length) ||
          (i != 0 && ranges[i - 1].gpu_va + ranges[i - 1].length >
                         ranges[i].gpu_va))
         return AGX_D3DKMT_ERR_INVAL;
   }

   while (*count > capacity) {
      uint32_t best = UINT32_MAX;
      uint64_t best_gap = UINT64_MAX;

      for (uint32_t i = 0; i + 1 < *count; i++) {
         const uint64_t end = ranges[i].gpu_va + ranges[i].length;
         const uint64_t next_end =
            ranges[i + 1].gpu_va + ranges[i + 1].length;
         const uint64_t gap = ranges[i + 1].gpu_va - end;
         const uint64_t merged_length = next_end - ranges[i].gpu_va;

         if (!agxw_va_range_is_user(ranges[i].gpu_va, merged_length))
            continue;
         if (best == UINT32_MAX || gap < best_gap) {
            best = i;
            best_gap = gap;
         }
      }

      if (best == UINT32_MAX)
         return AGX_D3DKMT_ERR_NOSPACE;

      const uint64_t merged_end =
         ranges[best + 1].gpu_va + ranges[best + 1].length;
      ranges[best].length = merged_end - ranges[best].gpu_va;
      memmove(&ranges[best + 1], &ranges[best + 2],
              (size_t)(*count - best - 2) * sizeof(*ranges));
      (*count)--;
      if (UINT64_MAX - bridged < best_gap)
         bridged = UINT64_MAX;
      else
         bridged += best_gap;
   }

   if (bridged_bytes != NULL)
      *bridged_bytes = bridged;
   return AGX_D3DKMT_OK;
}

static bool
dirty_range_align(uint64_t bo_size, uint64_t offset, uint64_t length,
                  uint64_t *start, uint64_t *end)
{
   if (bo_size == 0 || (bo_size & (AGX_D3DKMT_PAGE_SIZE - 1)) != 0 ||
       offset > bo_size || length > bo_size - offset)
      return false;

   if (length == 0) {
      *start = *end = offset;
      return true;
   }

   *start = offset & ~(AGX_D3DKMT_PAGE_SIZE - 1);
   uint64_t byte_end = offset + length;
   if (byte_end > UINT64_MAX - (AGX_D3DKMT_PAGE_SIZE - 1))
      return false;
   *end = (byte_end + AGX_D3DKMT_PAGE_SIZE - 1) &
          ~(AGX_D3DKMT_PAGE_SIZE - 1);
   if (*end > bo_size)
      *end = bo_size;
   return *start < *end;
}

static void
dirty_next_epoch(struct agx_d3dkmt_dirty_state *state)
{
   state->cpu_write_epoch++;
   if (state->cpu_write_epoch == 0)
      state->cpu_write_epoch = 1;
}

static void
dirty_make_whole(struct agx_d3dkmt_dirty_state *state)
{
   state->whole_bo = true;
   state->range_count = 0;
}

static bool
dirty_insert_aligned(struct agx_d3dkmt_dirty_state *state, uint64_t start,
                     uint64_t end)
{
   if (state->whole_bo)
      return true;

   uint32_t first = 0;
   while (first < state->range_count) {
      const uint64_t range_end = state->ranges[first].offset +
                                 state->ranges[first].length;
      if (range_end >= start)
         break;
      first++;
   }

   if (first == state->range_count || end < state->ranges[first].offset) {
      if (state->range_count == AGX_D3DKMT_DIRTY_RANGES_PER_BO) {
         dirty_make_whole(state);
         return true;
      }

      memmove(&state->ranges[first + 1], &state->ranges[first],
              (size_t)(state->range_count - first) * sizeof(state->ranges[0]));
      state->ranges[first] =
         (struct agx_d3dkmt_dirty_range){start, end - start};
      state->range_count++;
      return true;
   }

   uint64_t merged_start = start < state->ranges[first].offset
                              ? start
                              : state->ranges[first].offset;
   uint64_t merged_end = end;
   const uint64_t first_end = state->ranges[first].offset +
                              state->ranges[first].length;
   if (first_end > merged_end)
      merged_end = first_end;

   uint32_t last = first + 1;
   while (last < state->range_count &&
          state->ranges[last].offset <= merged_end) {
      const uint64_t range_end = state->ranges[last].offset +
                                 state->ranges[last].length;
      if (range_end > merged_end)
         merged_end = range_end;
      last++;
   }

   state->ranges[first] = (struct agx_d3dkmt_dirty_range){
      merged_start, merged_end - merged_start};
   if (last > first + 1) {
      memmove(&state->ranges[first + 1], &state->ranges[last],
              (size_t)(state->range_count - last) * sizeof(state->ranges[0]));
      state->range_count -= last - (first + 1);
   }
   return true;
}

void
agx_d3dkmt_dirty_reset(struct agx_d3dkmt_dirty_state *state,
                       uint64_t identity)
{
   memset(state, 0, sizeof(*state));
   state->identity = identity;
}

void
agx_d3dkmt_dirty_reset_mapped(struct agx_d3dkmt_dirty_state *state,
                              uint64_t identity, uint64_t bo_size,
                              bool mapped)
{
   agx_d3dkmt_dirty_reset(state, identity);
   if (mapped)
      (void)agx_d3dkmt_dirty_mark_whole(state, bo_size);
}

enum agx_d3dkmt_dirty_result
agx_d3dkmt_dirty_mark(struct agx_d3dkmt_dirty_state *state, uint64_t bo_size,
                      uint64_t offset, uint64_t length)
{
   if (state == NULL || state->identity == 0 || offset > bo_size ||
       length > bo_size - offset)
      return AGX_D3DKMT_DIRTY_INVALID;
   if (length == 0)
      return AGX_D3DKMT_DIRTY_OK;

   uint64_t start, end;
   if (!dirty_range_align(bo_size, offset, length, &start, &end))
      return AGX_D3DKMT_DIRTY_INVALID;

   dirty_next_epoch(state);
   dirty_insert_aligned(state, start, end);
   return AGX_D3DKMT_DIRTY_OK;
}

enum agx_d3dkmt_dirty_result
agx_d3dkmt_dirty_mark_whole(struct agx_d3dkmt_dirty_state *state,
                            uint64_t bo_size)
{
   if (state == NULL || state->identity == 0 || bo_size == 0 ||
       (bo_size & (AGX_D3DKMT_PAGE_SIZE - 1)) != 0)
      return AGX_D3DKMT_DIRTY_INVALID;

   dirty_next_epoch(state);
   dirty_make_whole(state);
   return AGX_D3DKMT_DIRTY_OK;
}

bool
agx_d3dkmt_dirty_detach(struct agx_d3dkmt_dirty_state *state,
                        struct agx_d3dkmt_dirty_snapshot *snapshot)
{
   if (state == NULL || snapshot == NULL ||
       (!state->whole_bo && state->range_count == 0))
      return false;

   memset(snapshot, 0, sizeof(*snapshot));
   snapshot->identity = state->identity;
   snapshot->cpu_write_epoch = state->cpu_write_epoch;
   snapshot->range_count = state->range_count;
   snapshot->whole_bo = state->whole_bo;
   memcpy(snapshot->ranges, state->ranges,
          (size_t)state->range_count * sizeof(snapshot->ranges[0]));

   state->range_count = 0;
   state->whole_bo = false;
   return true;
}

enum agx_d3dkmt_dirty_result
agx_d3dkmt_dirty_restore(struct agx_d3dkmt_dirty_state *state,
                         uint64_t bo_size,
                         const struct agx_d3dkmt_dirty_snapshot *snapshot)
{
   if (state == NULL || snapshot == NULL || snapshot->identity == 0 ||
       snapshot->range_count > AGX_D3DKMT_DIRTY_RANGES_PER_BO)
      return AGX_D3DKMT_DIRTY_INVALID;
   if (state->identity != snapshot->identity)
      return AGX_D3DKMT_DIRTY_STALE;

   if (snapshot->cpu_write_epoch > state->cpu_write_epoch)
      state->cpu_write_epoch = snapshot->cpu_write_epoch;
   if (snapshot->whole_bo) {
      dirty_make_whole(state);
      return AGX_D3DKMT_DIRTY_OK;
   }

   for (uint32_t i = 0; i < snapshot->range_count; i++) {
      uint64_t start, end;
      if (!dirty_range_align(bo_size, snapshot->ranges[i].offset,
                             snapshot->ranges[i].length, &start, &end))
         return AGX_D3DKMT_DIRTY_INVALID;
      dirty_insert_aligned(state, start, end);
   }
   return AGX_D3DKMT_DIRTY_OK;
}

enum agx_d3dkmt_dirty_result
agx_d3dkmt_dirty_commit(struct agx_d3dkmt_dirty_state *state,
                        const struct agx_d3dkmt_dirty_snapshot *snapshot)
{
   if (state == NULL || snapshot == NULL || snapshot->identity == 0)
      return AGX_D3DKMT_DIRTY_INVALID;
   if (state->identity != snapshot->identity)
      return AGX_D3DKMT_DIRTY_STALE;

   if (snapshot->cpu_write_epoch > state->clean_epoch)
      state->clean_epoch = snapshot->cpu_write_epoch;
   return AGX_D3DKMT_DIRTY_OK;
}

bool
agx_d3dkmt_dirty_has_pending(const struct agx_d3dkmt_dirty_state *state)
{
   return state != NULL && (state->whole_bo || state->range_count != 0);
}

void
agx_d3dkmt_cpu_map_reset(struct agx_d3dkmt_cpu_map_state *map, bool mapped)
{
   if (map == NULL)
      return;

   map->map_class = mapped ? AGX_D3DKMT_CPU_MAP_UNKNOWN
                           : AGX_D3DKMT_CPU_MAP_UNMAPPED;
   map->conservative = mapped;
}

bool
agx_d3dkmt_cpu_map_request(struct agx_d3dkmt_cpu_map_state *map,
                           struct agx_d3dkmt_dirty_state *dirty,
                           uint64_t bo_size,
                           enum agx_d3dkmt_cpu_map_class requested,
                           bool tracked_allowed)
{
   if (map == NULL || dirty == NULL ||
       (requested != AGX_D3DKMT_CPU_MAP_RAW &&
        requested != AGX_D3DKMT_CPU_MAP_TRACKED))
      return false;

   if (requested == AGX_D3DKMT_CPU_MAP_TRACKED && tracked_allowed &&
       map->map_class != AGX_D3DKMT_CPU_MAP_RAW) {
      /* A fresh map or cache reuse first publishes a complete snapshot. Cache
       * reset already installed that whole snapshot, so do not manufacture a
       * second epoch while it is still pending. */
      if (map->map_class != AGX_D3DKMT_CPU_MAP_TRACKED &&
          !dirty->whole_bo &&
          agx_d3dkmt_dirty_mark_whole(dirty, bo_size) !=
             AGX_D3DKMT_DIRTY_OK)
         goto raw;

      map->map_class = AGX_D3DKMT_CPU_MAP_TRACKED;
      map->conservative = false;
      return true;
   }

raw:
   /* Raw/application pointers have no callback for their next store. Ensure
    * the first raw lifetime is published now; build-submit will republish it
    * after every accepted submit while conservative remains set. */
   if ((map->map_class != AGX_D3DKMT_CPU_MAP_RAW || !map->conservative) &&
       agx_d3dkmt_dirty_mark_whole(dirty, bo_size) !=
          AGX_D3DKMT_DIRTY_OK)
      return false;

   map->map_class = AGX_D3DKMT_CPU_MAP_RAW;
   map->conservative = true;
   return false;
}

/* -------------------------------------------------------------------------
 * Handle pool.
 * ------------------------------------------------------------------------- */
void
agx_d3dkmt_pool_init(struct agx_d3dkmt_handle_pool *pool)
{
   memset(pool, 0, sizeof(*pool));
   /* Dense handle 0 is reserved: agx_bo_bind() passes handle 0 to mean
    * "no BO" (agx_device.c:139), so it must never name a live allocation. */
   pool->next = 1;
}

void
agx_d3dkmt_pool_finish(struct agx_d3dkmt_handle_pool *pool)
{
   free(pool->records);
   free(pool->freelist);
   memset(pool, 0, sizeof(*pool));
}

static bool
pool_grow(struct agx_d3dkmt_handle_pool *pool, uint32_t need)
{
   if (need <= pool->capacity)
      return true;

   uint32_t cap = pool->capacity ? pool->capacity : 64;
   while (cap < need) {
      if (cap > UINT32_MAX / 2)
         return false;
      cap *= 2;
   }

   struct agx_d3dkmt_allocation_record *records =
      calloc(cap, sizeof(*records));
   uint32_t *fl = malloc((size_t)cap * sizeof(*fl));
   if (!records || !fl) {
      free(records);
      free(fl);
      return false;
   }

   if (pool->capacity > 0) {
      memcpy(records, pool->records,
             (size_t)pool->capacity * sizeof(*records));
      memcpy(fl, pool->freelist,
             (size_t)pool->capacity * sizeof(*fl));
   }

   free(pool->records);
   free(pool->freelist);
   pool->records = records;
   pool->freelist = fl;

   pool->capacity = cap;
   return true;
}

uint32_t
agx_d3dkmt_pool_alloc(struct agx_d3dkmt_handle_pool *pool, agx_kmt_handle kmt)
{
   const struct agx_d3dkmt_allocation_record record = {
      .allocation = kmt,
   };
   return agx_d3dkmt_pool_alloc_record(pool, &record);
}

uint32_t
agx_d3dkmt_pool_alloc_record(
   struct agx_d3dkmt_handle_pool *pool,
   const struct agx_d3dkmt_allocation_record *record)
{
   if (record == NULL || record->allocation == 0)
      return 0;

   if (record->borrowed) {
      if (record->external_type !=
             AGX_D3DKMT_EXTERNAL_MEMORY_OPAQUE_WIN32_KMT ||
          record->resource != 0 || record->global_share != 0)
         return 0;
   } else {
      if (record->external_type != AGX_D3DKMT_EXTERNAL_MEMORY_NONE &&
          record->resource == 0)
         return 0;
      if (record->external_type ==
             AGX_D3DKMT_EXTERNAL_MEMORY_OPAQUE_WIN32_KMT &&
          record->global_share == 0)
         return 0;
      if (record->external_type !=
             AGX_D3DKMT_EXTERNAL_MEMORY_OPAQUE_WIN32_KMT &&
          record->global_share != 0)
         return 0;
   }

   if (pool->nfree > 0) {
      uint32_t dense = pool->freelist[--pool->nfree];
      pool->records[dense] = *record;
      return dense;
   }

   if (pool->next == UINT32_MAX)
      return 0;
   if (!pool_grow(pool, pool->next + 1))
      return 0;

   uint32_t dense = pool->next++;
   pool->records[dense] = *record;
   return dense;
}

agx_kmt_handle
agx_d3dkmt_pool_get(const struct agx_d3dkmt_handle_pool *pool, uint32_t dense)
{
   if (dense == 0 || dense >= pool->capacity)
      return 0;
   return pool->records[dense].allocation;
}

bool
agx_d3dkmt_pool_get_record(const struct agx_d3dkmt_handle_pool *pool,
                           uint32_t dense,
                           struct agx_d3dkmt_allocation_record *record)
{
   if (record == NULL || dense == 0 || dense >= pool->capacity ||
       pool->records[dense].allocation == 0)
      return false;

   *record = pool->records[dense];
   return true;
}

agx_kmt_handle
agx_d3dkmt_pool_release(struct agx_d3dkmt_handle_pool *pool, uint32_t dense)
{
   struct agx_d3dkmt_allocation_record record;
   if (!agx_d3dkmt_pool_release_record(pool, dense, &record))
      return 0;
   return record.allocation;
}

bool
agx_d3dkmt_pool_release_record(
   struct agx_d3dkmt_handle_pool *pool, uint32_t dense,
   struct agx_d3dkmt_allocation_record *record)
{
   if (record == NULL || dense == 0 || dense >= pool->capacity ||
       pool->records[dense].allocation == 0)
      return false;

   *record = pool->records[dense];
   memset(&pool->records[dense], 0, sizeof(pool->records[dense]));
   /* freelist is sized with records, so nfree can never exceed capacity. */
   pool->freelist[pool->nfree++] = dense;
   return true;
}

/* -------------------------------------------------------------------------
 * VM_BIND translation.
 * ------------------------------------------------------------------------- */
static bool
in_usc(const struct agx_d3dkmt_policy *p, uint64_t addr, uint64_t range)
{
   if (p->usc_size == 0)
      return false;

   return addr >= p->usc_base && (addr - p->usc_base) < p->usc_size &&
          range <= p->usc_size - (addr - p->usc_base);
}

static int
translate_one(const struct agx_d3dkmt_bind *b,
              const struct agx_d3dkmt_policy *policy,
              struct agx_kmt_updategpuva_op *op)
{
   /*
    * Contract rule G1 (CONTRACT.md SS4.6): every base, offset and size is a
    * multiple of 16 KiB. Validated with the miniport's own predicate rather
    * than a second copy of the rule, so the two sides can never disagree
    * about what is legal. agx_bo_bind() asserts the same thing upstream
    * (agx_device.c:134-136), but an assert compiles out in release and a
    * misaligned SizeInBytes silently corrupts a page-table update.
    */
   if (!agxw_va_range_is_bindable(b->addr, b->range) ||
       (b->offset % AGXW_PAGE_SIZE) != 0)
      return AGX_D3DKMT_ERR_INVAL;

   /* CONTRACT.md SS4.2: the user window is NOT contiguous. This rejects a
    * bind that would straddle or land on the kernel carve-out page. */
   if (!agxw_va_range_is_user(b->addr, b->range))
      return AGX_D3DKMT_ERR_INVAL;

   memset(op, 0, sizeof(*op));

   if (b->flags & AGX_D3DKMT_BIND_UNBIND) {
      op->OperationType = AGX_KMT_UPDATEGPUVA_UNMAP;
      op->Unmap.BaseAddress = b->addr;
      op->Unmap.SizeInBytes = b->range;
      op->Unmap.Protection.Value = AGX_KMT_GPUVA_PROT_NOACCESS;
      return AGX_D3DKMT_OK;
   }

   if (b->kmt_handle == 0)
      return AGX_D3DKMT_ERR_INVAL;

   uint64_t prot = 0;
   uint64_t drv = 0;

   if (b->flags & AGX_D3DKMT_BIND_WRITE)
      prot |= AGX_KMT_GPUVA_PROT_WRITE;

   /* WDDM cannot encode write-without-read; AGX's AP bits can, so the
    * distinction rides in DriverProtection (CONTRACT.md SS4.7). */
   if ((b->flags & AGX_D3DKMT_BIND_WRITE) && !(b->flags & AGX_D3DKMT_BIND_READ))
      drv |= AGXW_VAPROT_WRITE_ONLY;

   /* A bind op carries no execute bit -- drm_asahi_gem_bind_op has no such
    * field -- so executability is derived from the address, exactly as the
    * Linux driver derives it from the VA region. */
   if (in_usc(policy, b->addr, b->range)) {
      /* VidMm enforces W^X. The BO may be generically writable, but once it
       * is placed in the dedicated shader window AGX only fetches it; CPU
       * uploads use the allocation mapping rather than GPU write access.
       * A Write+Execute request otherwise returns a NoExecute DXGK_PTE. */
      prot &= ~AGX_KMT_GPUVA_PROT_WRITE;
      drv &= ~(uint64_t)AGXW_VAPROT_WRITE_ONLY;
      prot |= AGX_KMT_GPUVA_PROT_EXECUTE;
      drv |= AGXW_VAPROT_USC;
   }

   /* One page aliased across the whole range, natively (CONTRACT.md SS4.7).
    * No expansion, no page-table doubling: one operation. */
   if (b->flags & AGX_D3DKMT_BIND_SINGLE_PAGE)
      drv |= AGXW_VAPROT_SINGLE_PAGE;

   op->OperationType = AGX_KMT_UPDATEGPUVA_MAP_PROTECT;
   op->MapProtect.BaseAddress = b->addr;
   op->MapProtect.SizeInBytes = b->range;
   op->MapProtect.hAllocation = b->kmt_handle;
   op->MapProtect.AllocationOffsetInBytes = b->offset;
   op->MapProtect.AllocationSizeInBytes =
      (b->flags & AGX_D3DKMT_BIND_SINGLE_PAGE) ? AGXW_PAGE_SIZE : b->range;
   op->MapProtect.Protection.Value = prot;
   op->MapProtect.DriverProtection = drv;

   return AGX_D3DKMT_OK;
}

int
agx_d3dkmt_translate_binds(const struct agx_d3dkmt_bind *binds, uint32_t count,
                           const struct agx_d3dkmt_policy *policy,
                           struct agx_kmt_updategpuva_op *out,
                           uint32_t out_capacity, uint32_t *out_count)
{
   if (out_count == NULL || policy == NULL || (count > 0 && binds == NULL))
      return AGX_D3DKMT_ERR_INVAL;

   *out_count = count;

   if (out == NULL)
      return AGX_D3DKMT_OK;

   if (count > out_capacity)
      return AGX_D3DKMT_ERR_NOSPACE;

   for (uint32_t i = 0; i < count; ++i) {
      int ret = translate_one(&binds[i], policy, &out[i]);
      if (ret != AGX_D3DKMT_OK) {
         *out_count = 0;
         return ret;
      }
   }

   return AGX_D3DKMT_OK;
}

int
agx_d3dkmt_bind_policy(uint64_t shader_base,
                       const struct agx_d3dkmt_policy *runtime,
                       struct agx_d3dkmt_policy *out)
{
   if (runtime == NULL || out == NULL ||
       shader_base < AGX_D3DKMT_USC_MIN_BASE ||
       (shader_base & (AGX_D3DKMT_USC_ALIGNMENT - 1)) != 0 ||
       shader_base > UINT64_MAX - AGX_D3DKMT_USC_SIZE ||
       !agxw_va_range_is_user(shader_base, AGX_D3DKMT_USC_SIZE) ||
       runtime->usc_base != shader_base ||
       runtime->usc_size != AGX_D3DKMT_USC_SIZE)
      return AGX_D3DKMT_ERR_INVAL;

   *out = *runtime;
   /* Deliberately source the executable bounds from the independently stored
    * agx_device field, even after proving the winsys copy agrees. */
   out->usc_base = shader_base;
   out->usc_size = AGX_D3DKMT_USC_SIZE;
   return AGX_D3DKMT_OK;
}

int
agx_d3dkmt_validate_translated_binds(
   const struct agx_d3dkmt_bind *binds, uint32_t count,
   const struct agx_d3dkmt_policy *policy,
   const struct agx_kmt_updategpuva_op *ops, uint32_t op_count)
{
   if (policy == NULL || op_count != count ||
       (count > 0 && (binds == NULL || ops == NULL)))
      return AGX_D3DKMT_ERR_INVAL;

   for (uint32_t i = 0; i < count; ++i) {
      struct agx_kmt_updategpuva_op expected;
      if (translate_one(&binds[i], policy, &expected) != AGX_D3DKMT_OK ||
          memcmp(&expected, &ops[i], sizeof(expected)) != 0)
         return AGX_D3DKMT_ERR_INVAL;

      if (ops[i].OperationType == AGX_KMT_UPDATEGPUVA_MAP_PROTECT) {
         const bool usc =
            (ops[i].MapProtect.DriverProtection & AGXW_VAPROT_USC) != 0;
         const bool execute =
            (ops[i].MapProtect.Protection.Value &
             AGX_KMT_GPUVA_PROT_EXECUTE) != 0;
         const bool write =
            (ops[i].MapProtect.Protection.Value &
             AGX_KMT_GPUVA_PROT_WRITE) != 0;

         /* An executable classification below the first legal production
          * USC base is never recoverable.  Refuse it independently of the
          * policy comparison above so this remains a hard tripwire if the
          * translator is changed later. */
         if (usc &&
             (ops[i].MapProtect.BaseAddress < AGX_D3DKMT_USC_MIN_BASE ||
              !execute || write))
            return AGX_D3DKMT_ERR_INVAL;
         if (!usc && execute)
            return AGX_D3DKMT_ERR_INVAL;
      }
   }

   return AGX_D3DKMT_OK;
}

uint32_t
agx_d3dkmt_va_split_carveout(uint64_t base, uint64_t size,
                             struct agx_d3dkmt_va_range out[2])
{
   const uint64_t gran = AGX_D3DKMT_RESERVATION_GRANULARITY;
   const uint64_t mask = gran - 1;
   const uint64_t cb = AGXW_VM_KERNEL_CARVEOUT_BASE & ~mask;
   const uint64_t ce =
      (AGXW_VM_KERNEL_CARVEOUT_BASE + AGXW_VM_KERNEL_CARVEOUT_SIZE + mask) &
      ~mask;
   uint32_t n = 0;

   if (size == 0 || base > UINT64_MAX - size || base > UINT64_MAX - mask)
      return 0;

   uint64_t end = base + size;
   base = (base + mask) & ~mask;
   end &= ~mask;
   if (base >= end)
      return 0;

   /* Entirely below or entirely above: one inward-aligned range. */
   if (end <= cb || base >= ce) {
      out[0].base = base;
      out[0].size = end - base;
      return 1;
   }

   if (base < cb) {
      out[n].base = base;
      out[n].size = cb - base;
      n++;
   }

   if (end > ce) {
      out[n].base = ce;
      out[n].size = end - ce;
      n++;
   }

   return n;
}

/* -------------------------------------------------------------------------
 * Sync table.
 * ------------------------------------------------------------------------- */
void
agx_d3dkmt_sync_table_init(struct agx_d3dkmt_sync_table *t)
{
   memset(t, 0, sizeof(*t));
   /* Handle 0 is reserved: vk_drm_syncobj and hk_queue.c both treat a zero
    * syncobj handle as "none". */
   t->next = 1;
}

void
agx_d3dkmt_sync_table_finish(struct agx_d3dkmt_sync_table *t)
{
   free(t->slots);
   free(t->freelist);
   memset(t, 0, sizeof(*t));
}

static bool
sync_grow(struct agx_d3dkmt_sync_table *t, uint32_t need)
{
   if (need <= t->capacity)
      return true;

   uint32_t cap = t->capacity ? t->capacity : 32;
   while (cap < need) {
      if (cap > UINT32_MAX / 2)
         return false;
      cap *= 2;
   }

   struct agx_d3dkmt_sync *slots =
      realloc(t->slots, (size_t)cap * sizeof(*slots));
   if (!slots)
      return false;
   t->slots = slots;
   memset(t->slots + t->capacity, 0,
          (size_t)(cap - t->capacity) * sizeof(*slots));

   uint32_t *fl = realloc(t->freelist, (size_t)cap * sizeof(*fl));
   if (!fl)
      return false;
   t->freelist = fl;

   t->capacity = cap;
   return true;
}

uint32_t
agx_d3dkmt_sync_table_add(struct agx_d3dkmt_sync_table *t, agx_kmt_handle kmt,
                          const volatile uint64_t *cpu_va,
                          uint64_t initial_value)
{
   uint32_t handle;

   if (kmt == 0)
      return 0;

   if (t->nfree > 0) {
      handle = t->freelist[--t->nfree];
   } else {
      if (t->next == UINT32_MAX || !sync_grow(t, t->next + 1))
         return 0;
      handle = t->next++;
   }

   t->slots[handle] = (struct agx_d3dkmt_sync){
      .kmt = kmt,
      .base = 0,
      .last_request = initial_value,
      .cpu_va = cpu_va,
   };
   return handle;
}

struct agx_d3dkmt_sync *
agx_d3dkmt_sync_table_get(struct agx_d3dkmt_sync_table *t, uint32_t handle)
{
   if (handle == 0 || handle >= t->capacity || t->slots[handle].kmt == 0)
      return NULL;
   return &t->slots[handle];
}

agx_kmt_handle
agx_d3dkmt_sync_table_remove(struct agx_d3dkmt_sync_table *t, uint32_t handle)
{
   struct agx_d3dkmt_sync *s = agx_d3dkmt_sync_table_get(t, handle);
   if (s == NULL)
      return 0;

   agx_kmt_handle kmt = s->kmt;
   memset(s, 0, sizeof(*s));
   t->freelist[t->nfree++] = handle;
   return kmt;
}

uint64_t
agx_d3dkmt_sync_current(const struct agx_d3dkmt_sync *s)
{
   if (s->cpu_va != NULL) {
      uint64_t kernel = *s->cpu_va;
      /* The GPU can be ahead of anything we requested (another process's
       * shared fence), and we can be ahead of the GPU (a request not yet
       * retired). "Current" for signalling decisions is the larger. */
      return kernel > s->last_request ? kernel : s->last_request;
   }

   return s->last_request;
}

uint64_t
agx_d3dkmt_binary_signal_value(const struct agx_d3dkmt_sync *s, bool *skip)
{
   uint64_t target = s->base + 1;

   if (skip != NULL)
      *skip = agx_d3dkmt_sync_current(s) >= target;

   return target;
}

uint64_t
agx_d3dkmt_binary_wait_value(const struct agx_d3dkmt_sync *s)
{
   return s->base + 1;
}

uint64_t
agx_d3dkmt_binary_reset_base(const struct agx_d3dkmt_sync *s)
{
   return agx_d3dkmt_sync_current(s);
}

void
agx_d3dkmt_sync_transfer_values(const struct agx_d3dkmt_sync *src,
                                const struct agx_d3dkmt_sync *dst,
                                uint64_t src_point, uint64_t dst_point,
                                uint64_t *src_value, uint64_t *dst_value)
{
   /* DRM syncobj transfer uses point zero to name the binary payload.  A
    * D3DKMT monitored fence never has a replaceable binary payload, so the
    * portable epoch model maps that payload to base + 1.  Non-zero points
    * are timeline values and pass through unchanged. */
   *src_value = src_point ? src_point : agx_d3dkmt_binary_wait_value(src);
   *dst_value = dst_point ? dst_point
                          : agx_d3dkmt_binary_signal_value(dst, NULL);
}

/* -------------------------------------------------------------------------
 * Timeouts.
 * ------------------------------------------------------------------------- */
uint32_t
agx_d3dkmt_timeout_to_ms(int64_t abs_timeout_nsec, int64_t now_nsec)
{
   if (abs_timeout_nsec == INT64_MAX)
      return AGX_D3DKMT_INFINITE;

   if (abs_timeout_nsec <= now_nsec)
      return 0;

   /* Both operands are non-negative-difference here, so this cannot
    * overflow: abs > now, and both are int64. */
   uint64_t delta_ns = (uint64_t)(abs_timeout_nsec - now_nsec);
   uint64_t ms = (delta_ns + 999999ull) / 1000000ull;

   if (ms >= AGX_D3DKMT_INFINITE)
      return AGX_D3DKMT_INFINITE - 1;

   return (uint32_t)ms;
}

uint32_t
agx_d3dkmt_first_signaled(const uint64_t *current, const uint64_t *wanted,
                          uint32_t count)
{
   for (uint32_t i = 0; i < count; ++i) {
      if (current[i] >= wanted[i])
         return i;
   }

   return UINT32_MAX;
}

enum agx_d3dkmt_cpu_wait_state
agx_d3dkmt_cpu_wait_probe(const volatile uint64_t *const *current,
                          const uint64_t *wanted, uint32_t count,
                          bool wait_any, uint32_t *first_signaled)
{
   if (current == NULL || wanted == NULL || count == 0)
      return AGX_D3DKMT_CPU_WAIT_INVALID;

   for (uint32_t i = 0; i < count; ++i) {
      if (current[i] == NULL)
         return AGX_D3DKMT_CPU_WAIT_INVALID;
   }

   uint32_t first = UINT32_MAX;
   for (uint32_t i = 0; i < count; ++i) {
      if (*current[i] >= wanted[i]) {
         if (first == UINT32_MAX)
            first = i;
      } else if (!wait_any) {
         return AGX_D3DKMT_CPU_WAIT_PENDING;
      }
   }

   if (first == UINT32_MAX)
      return AGX_D3DKMT_CPU_WAIT_PENDING;

   if (first_signaled != NULL)
      *first_signaled = wait_any ? first : 0;

   return AGX_D3DKMT_CPU_WAIT_SATISFIED;
}

/* -------------------------------------------------------------------------
 * Contract blob helpers.
 * ------------------------------------------------------------------------- */
int
agx_d3dkmt_parse_identity(const void *blob, uint32_t blob_size,
                          struct agx_d3dkmt_policy *policy,
                          struct agxw_adapter_identity *out)
{
   struct agxw_adapter_identity id;

   if (blob == NULL || policy == NULL || out == NULL)
      return AGX_D3DKMT_ERR_INVAL;

   /*
    * Width is asserted, never inferred. The D3D runtime does not validate
    * private-data sizes across the user/kernel boundary (CONTRACT.md SS12.1
    * makes the same argument for the allocation blob), so a short reply is a
    * different adapter, not a truncated ours.
    */
   if (blob_size < sizeof(id))
      return AGX_D3DKMT_ERR_INVAL;

   memcpy(&id, blob, sizeof(id));

   if (id.magic != AGXW_ADAPTER_IDENTITY_MAGIC)
      return AGX_D3DKMT_ERR_INVAL;
   if (id.version != AGXW_ADAPTER_IDENTITY_VERSION)
      return AGX_D3DKMT_ERR_INVAL;
   if (id.struct_size != sizeof(id))
      return AGX_D3DKMT_ERR_INVAL;
   if (id.contract_version != AGXW_CONTRACT_VERSION)
      return AGX_D3DKMT_ERR_INVAL;
   if (id.params_blob_size != AGXW_PARAMS_GLOBAL_SIZE)
      return AGX_D3DKMT_ERR_INVAL;

   *policy = agx_d3dkmt_policy_default();
   *out = id;
   return AGX_D3DKMT_OK;
}

uint32_t
agx_d3dkmt_agxw_alloc_flags(uint32_t agx_bo_flags)
{
   uint32_t flags = 0;

   if (agx_bo_flags & AGX_D3DKMT_BO_WRITEBACK)
      flags |= AGXW_ALLOC_WRITEBACK;

   /* Mutually exclusive by contract: an allocation is either private to this
    * device's VM or shareable, never both (agxw_contract.h:139-143). */
   if (agx_bo_flags & AGX_D3DKMT_BO_SHAREABLE)
      flags |= AGXW_ALLOC_SHAREABLE;
   else
      flags |= AGXW_ALLOC_VM_PRIVATE;

   /* Aurora scanout donor: latches this allocation in the KMD as the pending
    * content donor for the next ADOPT_EXISTING primary of this process.  The
    * contract validator refuses a cached or VM-private donor, so the caller
    * must have kept the BO shareable and write-combined. */
   if (agx_bo_flags & AGX_D3DKMT_BO_SCANOUT_DONOR)
      flags |= AGXW_ALLOC_SCANOUT_DONOR;

   return flags;
}

uint32_t
agx_d3dkmt_cpu_visible_flags(uint32_t agx_bo_flags)
{
   if (agx_bo_flags & AGX_D3DKMT_BO_EXEC)
      agx_bo_flags |= AGX_D3DKMT_BO_WRITEBACK;

   return agx_bo_flags;
}

uint32_t
agx_d3dkmt_create_flags(uint32_t agx_bo_flags)
{
   enum agx_d3dkmt_external_memory_type type =
      (agx_bo_flags & AGX_D3DKMT_BO_SHAREABLE)
      ? AGX_D3DKMT_EXTERNAL_MEMORY_OPAQUE_WIN32
      : AGX_D3DKMT_EXTERNAL_MEMORY_NONE;
   return agx_d3dkmt_create_flags_external(agx_bo_flags, type);
}

uint32_t
agx_d3dkmt_create_flags_external(
   uint32_t agx_bo_flags,
   enum agx_d3dkmt_external_memory_type external_type)
{
   if (external_type > AGX_D3DKMT_EXTERNAL_MEMORY_OPAQUE_WIN32_KMT)
      return 0;
   if (external_type != AGX_D3DKMT_EXTERNAL_MEMORY_NONE &&
       !(agx_bo_flags &
         (AGX_D3DKMT_BO_SHARED | AGX_D3DKMT_BO_SHAREABLE)))
      return 0;

   uint32_t flags = 0;

   if (external_type != AGX_D3DKMT_EXTERNAL_MEMORY_NONE) {
      /* A global share handle always names a RESOURCE, never a bare
       * allocation. One allocation per resource is the miniport contract. */
      flags |= AGX_KMT_CREATEALLOC_CREATERESOURCE |
               AGX_KMT_CREATEALLOC_CREATESHARED;

      if (external_type == AGX_D3DKMT_EXTERNAL_MEMORY_OPAQUE_WIN32)
         flags |= AGX_KMT_CREATEALLOC_NTSECURITYSHARING;
   }

   /* Honeykrisp memsets or fully overwrites everything it allocates, and
    * zeroing a fresh BO is pure cost on a UMA part. */
   flags |= AGX_KMT_CREATEALLOC_ALLOWNOTZEROED;

   /* NEVER ExistingSysMem: CONTRACT.md SS3.6 rejects it with
    * STATUS_INVALID_PARAMETER because AGX BOs must be 16 KiB
    * physically-chunked and arbitrary user pages are not. */
   return flags;
}

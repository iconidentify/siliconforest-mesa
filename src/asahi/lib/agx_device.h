/*
 * Copyright 2021 Alyssa Rosenzweig
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>
#ifdef HAVE_LIBDRM
#include <xf86drm.h>
#endif
#include "drm-uapi/asahi_drm.h"
#include "util/ralloc.h"
#include "util/simple_mtx.h"
#include "util/sparse_array.h"
#include "util/timespec.h"
#include "util/u_atomic.h"
#include "util/u_printf.h"
#include "util/vma.h"

/* Internal-only marker consumed by the D3DKMT winsys. It is never sent to
 * drm_asahi; queue.c only emits it when write_set_contract is true. */
#define AGX_SUBMIT_WRITE_SET_COMPLETE (1u << 31)
#include "agx_bo.h"
#include "agx_chip.h"
#include "agx_pack.h"
#include "decode.h"
#include "layout.h"
#include "libagx_dgc.h"

/*
 * honeykrisp-h1-execution.md SS2.1 row D2: vdrm.h and asahi_proto.h are not
 * self-contained without libdrm (asahi_proto.h embeds struct vdrm_shmem,
 * pulled from vdrm.h's own libdrm-using guts) and neither is needed by this
 * header itself -- only by pointer (struct vdrm_device *dev->vdrm,
 * struct asahi_ccmd_submit_res *agx_submit_virt::extres below). On Windows
 * (no HAVE_LIBDRM) forward-declare instead; the real includes move into the
 * virtio-only TUs that need the full definitions (agx_device_virtio.c,
 * agx_device.c under HAVE_LIBDRM).
 */
#ifdef HAVE_LIBDRM
#include "vdrm.h"
#include "asahi_proto.h"
#else
struct vdrm_device;
struct asahi_ccmd_submit_res;
#endif

enum agx_dbg {
   AGX_DBG_TRACE = BITFIELD_BIT(0),
   AGX_DBG_BODUMP = BITFIELD_BIT(1),
   AGX_DBG_NO16 = BITFIELD_BIT(2),
   AGX_DBG_DIRTY = BITFIELD_BIT(3),
   AGX_DBG_PRECOMPILE = BITFIELD_BIT(4),
   AGX_DBG_PERF = BITFIELD_BIT(5),
   AGX_DBG_NOCOMPRESS = BITFIELD_BIT(6),
   AGX_DBG_NOCLUSTER = BITFIELD_BIT(7),
   AGX_DBG_SYNC = BITFIELD_BIT(8),
   AGX_DBG_STATS = BITFIELD_BIT(9),
   AGX_DBG_RESOURCE = BITFIELD_BIT(10),
   AGX_DBG_BATCH = BITFIELD_BIT(11),
   AGX_DBG_NOWC = BITFIELD_BIT(12),
   AGX_DBG_SYNCTVB = BITFIELD_BIT(13),
   AGX_DBG_SMALLTILE = BITFIELD_BIT(14),
   AGX_DBG_NOMSAA = BITFIELD_BIT(15),
   AGX_DBG_NOSHADOW = BITFIELD_BIT(16),
   AGX_DBG_BODUMPVERBOSE = BITFIELD_BIT(17),
   AGX_DBG_SCRATCH = BITFIELD_BIT(18),
   AGX_DBG_NOSOFT = BITFIELD_BIT(19),
   AGX_DBG_FEEDBACK = BITFIELD_BIT(20),
   AGX_DBG_1QUEUE = BITFIELD_BIT(21),
   AGX_DBG_NOMERGE = BITFIELD_BIT(22),
};

/* How many power-of-two levels in the BO cache do we want? 2^14 minimum chosen
 * as it is the page size that all allocations are rounded to
 */
#define MIN_BO_CACHE_BUCKET (14) /* 2^14 = 16KB */
#define MAX_BO_CACHE_BUCKET (22) /* 2^22 = 4MB */

/* Fencepost problem, hence the off-by-one */
#define NR_BO_CACHE_BUCKETS (MAX_BO_CACHE_BUCKET - MIN_BO_CACHE_BUCKET + 1)

/* Forward decl only, do not pull in all of NIR */
struct nir_shader;

#define BARRIER_RENDER  (1 << DRM_ASAHI_SUBQUEUE_RENDER)
#define BARRIER_COMPUTE (1 << DRM_ASAHI_SUBQUEUE_COMPUTE)

struct agx_submit_virt {
   uint32_t extres_count;
   struct asahi_ccmd_submit_res *extres;
   uint32_t ring_idx;
};

typedef struct {
   struct agx_bo *(*bo_alloc)(struct agx_device *dev, size_t size, size_t align,
                              enum agx_bo_flags flags);
   int (*bo_bind)(struct agx_device *dev, struct drm_asahi_gem_bind_op *ops,
                  uint32_t count);
   void (*bo_mmap)(struct agx_device *dev, struct agx_bo *bo, void *fixed_addr);
   ssize_t (*get_params)(struct agx_device *dev, void *buf, size_t size);
   int (*submit)(struct agx_device *dev, struct drm_asahi_submit *submit,
                 struct agx_submit_virt *virt);
   int (*bo_bind_object)(struct agx_device *dev,
                         struct drm_asahi_gem_bind_object *bind);
   int (*bo_unbind_object)(struct agx_device *dev, uint32_t object_handle);

   /*
    * honeykrisp-h1-execution.md SS1 "widen the table by three ops": lets
    * agx_device.c's remaining out-of-ops-table drm/OS call sites
    * (asahi_simple_ioctl's four callers -- VM_CREATE, QUEUE_CREATE,
    * QUEUE_DESTROY, GET_TIME -- plus agx_bo_free()'s munmap and GEM_CLOSE
    * tail) route through the ops table like every other backend operation,
    * so a non-libdrm backend (e.g. the Windows winsys, agx_device_win.c)
    * never needs its own copy of agx_bo_free()/agx_create_command_queue()/
    * agx_destroy_command_queue()/agx_get_gpu_timestamp() -- only these three
    * new leaf ops.
    *
    * simple_ioctl: passthrough for the four asahi_simple_ioctl callers;
    * `cmd` is always one of the DRM_IOCTL_ASAHI_* pinned literals. The Linux
    * body (asahi_simple_ioctl) already dispatches on dev->is_virtio itself,
    * so the SAME function is installed in both agx_device_drm_ops and
    * agx_virtio_device_ops.
    *
    * bo_munmap: unmap bo->_map (bo->size bytes). Only ever called from
    * agx_bo_free() when bo->_map is non-NULL.
    *
    * bo_free: release the OS/kernel object referenced by `handle` (GEM_CLOSE
    * on Linux). Deliberately takes the raw handle, not `struct agx_bo *`,
    * unlike bo_munmap/bo_mmap: agx_bo_free() must memset() the `bo`
    * bookkeeping struct to all-zero *before* issuing the kernel-visible free
    * (see the comment at its call site -- avoids a lookup-table race with a
    * concurrently reused handle number), so by the time this op runs
    * `bo->handle` has already been zeroed and cannot be read back out of
    * `bo`.
    */
   int (*simple_ioctl)(struct agx_device *dev, unsigned cmd, void *req);
   void (*bo_munmap)(struct agx_device *dev, struct agx_bo *bo);
   int (*bo_sync_for_device)(struct agx_device *dev, struct agx_bo *bo,
                             size_t offset, size_t size);
   int (*bo_sync_for_cpu)(struct agx_device *dev, struct agx_bo *bo,
                          size_t offset, size_t size);
   void (*bo_mark_cpu_dirty)(struct agx_device *dev, struct agx_bo *bo,
                             size_t offset, size_t size, bool precise);
   void (*bo_reset_cpu_dirty)(struct agx_device *dev, struct agx_bo *bo);
   void (*bo_discard_cpu_dirty)(struct agx_device *dev, struct agx_bo *bo);
   void (*bo_free)(struct agx_device *dev, uint32_t handle);
} agx_device_ops_t;

int agx_bo_bind(struct agx_device *dev, struct agx_bo *bo, uint64_t addr,
                size_t size_B, uint64_t offset_B, uint32_t flags);

int agx_bind_timestamps(struct agx_device *dev, struct agx_bo *bo,
                        uint32_t *handle);

/*
 * Linux/virtio-shared op bodies backing the three ops added just above.
 * Both agx_device_drm_ops (agx_device.c) and agx_virtio_device_ops
 * (agx_device_virtio.c) install the SAME functions here: generic DRM-core
 * behavior (GEM_CLOSE, plain munmap) and asahi_simple_ioctl's own existing
 * is_virtio dispatch don't differ between the two Linux backends -- exactly
 * why this code was unconditional (not backend-specific) before this ops
 * widening existed. Declared here (like agx_bo_bind/agx_bind_timestamps
 * above) so agx_device_virtio.c can reference the agx_device.c definitions.
 */
int asahi_simple_ioctl(struct agx_device *dev, unsigned cmd, void *req);
void agx_drm_bo_munmap(struct agx_device *dev, struct agx_bo *bo);
int agx_drm_bo_sync_for_device(struct agx_device *dev, struct agx_bo *bo,
                               size_t offset, size_t size);
void agx_drm_bo_close(struct agx_device *dev, uint32_t handle);

struct agx_device {
   uint32_t debug;

   /* Precompiled libagx binary table */
   const uint32_t **libagx_programs;

   char name[64];
   struct drm_asahi_params_global params;
   bool is_virtio;
   bool write_set_contract;
   agx_device_ops_t ops;

   /* vdrm device */
   struct vdrm_device *vdrm;
   uint32_t next_blob_id;

   /* Device handle */
   int fd;

   /*
    * honeykrisp-h1-execution.md SS1: Windows winsys handle (agx_device_win.c
    * agx_win_open_device()), used as the DeviceIoControl target for every
    * ntagx_ioctl() call. NULL/unused on Linux/virtio, where `fd` above is
    * the live handle instead; `fd` stays -1 on Windows (opaque pointer, not
    * cast to/from `int fd` -- real type is an NT HANDLE, kept void* here so
    * this header stays includable without <windows.h>).
    */
   void *hdev;

   /* VM handle */
   uint32_t vm_id;

   /* Global queue handle */
   uint32_t queue_id;

   /* VMA heaps */
   simple_mtx_t vma_lock;
   uint64_t shader_base;
   struct util_vma_heap main_heap;
   struct util_vma_heap usc_heap;
   uint64_t guard_size;

   /* To emulate sparse-resident buffers, we map buffers in both the bottom half
    * and top half of the address space. sparse_ro_offset controls the
    * partitioning. This is a power-of-two that &'s zero in bottom (read-write)
    * buffers but non-zero in top (read-only) shadow mappings.
    *
    * In other words, given an address X, we can check if it is in the top half
    * if (X & sparse_ro_offset) != 0.
    *
    * Given a bottom half address X, we can get the top half address
    * equivalently as (X + sparse_ro_offset) or (X | sparse_ro_offset).
    */
   uint64_t sparse_ro_offset;

   struct agx_bo *zero_bo, *scratch_bo;

   struct renderonly *ro;

   /*
    * honeykrisp-h1-execution.md SS2.1 "pthread inventory": simple_mtx_t
    * (already used elsewhere in this struct, e.g. vma_lock/bo_cache.lock
    * above) instead of pthread_mutex_t -- Windows-clean, mechanical rename,
    * no Linux behavior change (simple_mtx_t is a thin pthread wrapper
    * there).
    */
   simple_mtx_t bo_map_lock;
   struct util_sparse_array bo_map;
   uint32_t max_handle;

   /* Membership generation for the D3DKMT submission-retention set. Every
    * live GPU VA is retained independently from the precise CPU-dirty set. */
   uint32_t read_set_generation;

#ifdef _WIN32
   /* Independent from bo_map_lock so a persistent-map write can publish an
    * interval without serializing BO lookup. Submit takes bo_map_lock first,
    * then this lock, which is also the lifetime-reset lock order. */
   simple_mtx_t d3dkmt_dirty_lock;
   /* Serializes detaching CPU-dirty ownership through SubmitCommand's accept
    * result. Without this gate a second context could observe an empty clean
    * set while the first context temporarily owned the only dirty snapshot. */
   simple_mtx_t d3dkmt_submit_clean_lock;
   uint64_t d3dkmt_next_bo_identity;
#endif

   struct {
      simple_mtx_t lock;

      /* List containing all cached BOs sorted in LRU (Least Recently Used)
       * order so we can quickly evict BOs that are more than 1 second old.
       */
      struct list_head lru;

      /* The BO cache is a set of buckets with power-of-two sizes.  Each bucket
       * is a linked list of free panfrost_bo objects.
       */
      struct list_head buckets[NR_BO_CACHE_BUCKETS];

      /* Current size of the BO cache in bytes (sum of sizes of cached BOs) */
      size_t size;

      /* Number of hits/misses for the BO cache */
      uint64_t hits, misses;
   } bo_cache;

   struct agxdecode_ctx *agxdecode;

   /* Prepacked USC Sampler word to bind the txf sampler, used for
    * precompiled shaders on both drivers.
    */
   struct agx_usc_sampler_packed txf_sampler;

   /* Simplified device selection */
   enum agx_chip chip;

   struct {
      uint64_t num;
      uint64_t den;
   } user_timestamp_to_ns;

   struct u_printf_ctx printf;
};

static inline int
agx_bo_sync_for_device(struct agx_bo *bo, size_t offset, size_t size)
{
   assert(bo != NULL && bo->dev != NULL);
   assert(offset <= bo->size && size <= bo->size - offset);
   return bo->dev->ops.bo_sync_for_device(bo->dev, bo, offset, size);
}

static inline int
agx_bo_sync_for_cpu(struct agx_bo *bo, size_t offset, size_t size)
{
   assert(bo != NULL && bo->dev != NULL);
   assert(offset <= bo->size && size <= bo->size - offset);
   return bo->dev->ops.bo_sync_for_cpu
             ? bo->dev->ops.bo_sync_for_cpu(bo->dev, bo, offset, size)
             : 0;
}

static inline void
agx_bo_mark_cpu_dirty(struct agx_bo *bo, size_t offset, size_t size,
                      bool precise)
{
   assert(bo != NULL && bo->dev != NULL);
   assert(offset <= bo->size && size <= bo->size - offset);
   if (bo->dev->ops.bo_mark_cpu_dirty)
      bo->dev->ops.bo_mark_cpu_dirty(bo->dev, bo, offset, size, precise);
}

static inline void
agx_bo_reset_cpu_dirty(struct agx_bo *bo)
{
   if (bo->dev->ops.bo_reset_cpu_dirty)
      bo->dev->ops.bo_reset_cpu_dirty(bo->dev, bo);
}

static inline void
agx_bo_discard_cpu_dirty(struct agx_bo *bo)
{
   if (bo->dev->ops.bo_discard_cpu_dirty)
      bo->dev->ops.bo_discard_cpu_dirty(bo->dev, bo);
}

/*
 * Determine if an address is in the read-only section. See the documentation
 * for sparse_ro_offset.
 */
static inline bool
agx_addr_is_ro(struct agx_device *dev, uint64_t addr)
{
   return (addr & dev->sparse_ro_offset);
}

/*
 * Convert a read-write address to its read-only shadow address. See the
 * documentation for sparse_ro_offset.
 */
static inline uint64_t
agx_rw_addr_to_ro(struct agx_device *dev, uint64_t addr)
{
   assert(!agx_addr_is_ro(dev, addr));
   return addr + dev->sparse_ro_offset;
}

static inline void *
agx_bo_map_placed(struct agx_bo *bo, void *fixed_addr)
{
   if (!bo->_map) {
      bo->dev->ops.bo_mmap(bo->dev, bo, fixed_addr);
      if (bo->_map) {
         p_atomic_inc(&bo->dev->read_set_generation);
         /* A raw persistent pointer has no later write callback. Keep the BO
          * conservative for its mapped lifetime; HOST_COHERENT memory may be
          * written again without a Vulkan Flush callback. */
         agx_bo_mark_cpu_dirty(bo, 0, bo->size, false);
      }
   }

   return bo->_map;
}

static inline void *
agx_bo_map(struct agx_bo *bo)
{
   return agx_bo_map_placed(bo, NULL);
}

static inline bool
agx_has_soft_fault(struct agx_device *dev)
{
   return (dev->params.features & DRM_ASAHI_FEATURE_SOFT_FAULTS) &&
          !(dev->debug & AGX_DBG_NOSOFT);
}

static uint32_t
agx_usc_addr(struct agx_device *dev, uint64_t addr)
{
   assert(addr >= dev->shader_base);
   assert((addr - dev->shader_base) <= UINT32_MAX);

   return addr - dev->shader_base;
}

bool agx_open_device(void *memctx, struct agx_device *dev);

void agx_close_device(struct agx_device *dev);

static inline struct agx_bo *
agx_lookup_bo(struct agx_device *dev, uint32_t handle)
{
   return util_sparse_array_get(&dev->bo_map, handle);
}

uint32_t agx_create_command_queue(struct agx_device *dev,
                                  enum drm_asahi_priority priority);
int agx_destroy_command_queue(struct agx_device *dev, uint32_t queue_id);

int agx_import_sync_file(struct agx_device *dev, struct agx_bo *bo, int fd);
int agx_export_sync_file(struct agx_device *dev, struct agx_bo *bo);

void agx_debug_fault(struct agx_device *dev, uint64_t addr);

uint64_t agx_get_gpu_timestamp(struct agx_device *dev);

static inline uint64_t
agx_gpu_timestamp_to_ns(struct agx_device *dev, uint64_t gpu_timestamp)
{
   return (gpu_timestamp * dev->user_timestamp_to_ns.num) /
          dev->user_timestamp_to_ns.den;
}

void agx_get_device_uuid(const struct agx_device *dev, void *uuid);
void agx_get_driver_uuid(void *uuid);
unsigned agx_get_num_cores(const struct agx_device *dev);

struct agx_device_key agx_gather_device_key(struct agx_device *dev);

struct agx_va *agx_va_alloc(struct agx_device *dev, uint64_t size_B,
                            uint64_t align_B, enum agx_va_flags flags,
                            uint64_t fixed_va);
void agx_va_free(struct agx_device *dev, struct agx_va *va, bool unbind);

static inline struct drm_asahi_cmd_header
agx_cmd_header(bool compute, uint16_t barrier_vdm, uint16_t barrier_cdm)
{
   return (struct drm_asahi_cmd_header){
      .cmd_type = compute ? DRM_ASAHI_CMD_COMPUTE : DRM_ASAHI_CMD_RENDER,
      .size = compute ? sizeof(struct drm_asahi_cmd_compute)
                      : sizeof(struct drm_asahi_cmd_render),
      .vdm_barrier = barrier_vdm,
      .cdm_barrier = barrier_cdm,
   };
}

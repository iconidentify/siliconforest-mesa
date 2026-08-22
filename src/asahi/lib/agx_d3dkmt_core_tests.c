/*
 * Copyright 2026 the Asahi Linux contributors
 * SPDX-License-Identifier: MIT
 */

/*
 * Host tests for agx_d3dkmt_core.c. Never built by meson; compiled and run
 * natively (macOS/Linux, no Windows, no GPU, no miniport) by
 * gpu/honeykrisp-port/tests/run-host-tests.sh.
 *
 * These cover exactly the four places where an AGX concept and a WDDM concept
 * do not line up, because those are the places a wrong answer is silent:
 * handle spaces, VM_BIND expansion, binary-syncobj epochs, and absolute ->
 * relative timeout conversion. Everything else in the winsys is a D3DKMT call
 * whose behaviour cannot be known until the miniport exists.
 */

#include "agx_d3dkmt_core.h"

#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(cond)                                                            \
   do {                                                                        \
      checks++;                                                                \
      if (!(cond)) {                                                           \
         fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
         failures++;                                                           \
      }                                                                        \
   } while (0)

#define CHECK_EQ(a, b)                                                         \
   do {                                                                        \
      checks++;                                                                \
      uint64_t _a = (uint64_t)(a), _b = (uint64_t)(b);                         \
      if (_a != _b) {                                                          \
         fprintf(stderr,                                                       \
                 "FAIL %s:%d: %s == %s (%" PRIu64 " vs %" PRIu64 ")\n",        \
                 __FILE__, __LINE__, #a, #b, _a, _b);                          \
         failures++;                                                           \
      }                                                                        \
   } while (0)

/* ------------------------------------------------------------------------ */
static void
test_handle_pool_is_dense(void)
{
   struct agx_d3dkmt_handle_pool pool;
   agx_d3dkmt_pool_init(&pool);

   /* D3DKMT handles are large and sparse: dxgkrnl packs a table index and an
    * instance counter into one UINT. Feed the pool values shaped like real
    * ones and require that the dense handles it returns stay small, because
    * agx_device.c:835 / agx_bo.c:245 iterate 0..max_handle and
    * agx_lookup_bo() ALLOCATES a sparse-array node for every index. */
   static const agx_kmt_handle kmt[] = {
      0x40000001u, 0x40000002u, 0x80001234u, 0xC0FFEE01u, 0x7FFFFFF0u,
   };
   uint32_t dense[5];

   for (unsigned i = 0; i < 5; ++i) {
      dense[i] = agx_d3dkmt_pool_alloc(&pool, kmt[i]);
      CHECK(dense[i] != 0);
      CHECK(dense[i] <= 5);
      CHECK_EQ(agx_d3dkmt_pool_get(&pool, dense[i]), kmt[i]);
   }

   /* Handle 0 must never be handed out: agx_bo_bind() uses it as "no BO". */
   for (unsigned i = 0; i < 5; ++i)
      CHECK(dense[i] != 0);
   CHECK_EQ(agx_d3dkmt_pool_get(&pool, 0), 0);

   /* Release returns the D3DKMT handle, which is what ops.bo_free() needs
    * AFTER agx_bo_free() has already zeroed struct agx_bo. */
   CHECK_EQ(agx_d3dkmt_pool_release(&pool, dense[2]), 0x80001234u);
   CHECK_EQ(agx_d3dkmt_pool_get(&pool, dense[2]), 0);
   CHECK_EQ(agx_d3dkmt_pool_release(&pool, dense[2]), 0);

   /* And the dense handle is reused, so max_handle tracks live BOs. */
   uint32_t again = agx_d3dkmt_pool_alloc(&pool, 0xAAAAAAAAu);
   CHECK_EQ(again, dense[2]);
   CHECK_EQ(agx_d3dkmt_pool_get(&pool, again), 0xAAAAAAAAu);

   /* A zero D3DKMT handle is not a valid allocation. */
   CHECK_EQ(agx_d3dkmt_pool_alloc(&pool, 0), 0);

   /* Resource/global-handle identity survives agx_bo's memset and is returned
    * atomically with the allocation owner. KMT is explicitly non-owning: it
    * is metadata, while hResource is the object DestroyAllocation2 closes. */
   struct agx_d3dkmt_allocation_record shared = {
      .allocation = 0x4000abcdu,
      .resource = 0x5000abcdu,
      .global_share = 0x6000abcdu,
      .external_type = AGX_D3DKMT_EXTERNAL_MEMORY_OPAQUE_WIN32_KMT,
   };
   uint32_t shared_dense = agx_d3dkmt_pool_alloc_record(&pool, &shared);
   CHECK(shared_dense != 0);
   struct agx_d3dkmt_allocation_record got;
   CHECK(agx_d3dkmt_pool_get_record(&pool, shared_dense, &got));
   CHECK_EQ(got.allocation, shared.allocation);
   CHECK_EQ(got.resource, shared.resource);
   CHECK_EQ(got.global_share, shared.global_share);
   CHECK_EQ(got.external_type, shared.external_type);
   memset(&got, 0, sizeof(got));
   CHECK(agx_d3dkmt_pool_release_record(&pool, shared_dense, &got));
   CHECK_EQ(got.resource, shared.resource);
   CHECK(!agx_d3dkmt_pool_get_record(&pool, shared_dense, &got));

   shared.resource = 0;
   CHECK_EQ(agx_d3dkmt_pool_alloc_record(&pool, &shared), 0);
   shared.resource = 0x5000abcdu;
   shared.global_share = 0;
   CHECK_EQ(agx_d3dkmt_pool_alloc_record(&pool, &shared), 0);

   /* A tagged local import borrows a process-local allocation. It has no
    * resource/global-share owner in this winsys and must preserve that
    * lifetime bit through the dense-handle pool. */
   const uintptr_t tagged =
      (uintptr_t)(AGX_D3DKMT_LOCAL_ALLOCATION_TAG | UINT64_C(0x40001234));
   CHECK(agx_d3dkmt_is_local_allocation_handle(tagged));
   CHECK_EQ(agx_d3dkmt_local_allocation_handle(tagged), 0x40001234u);
   CHECK(!agx_d3dkmt_is_local_allocation_handle(0x40001234u));
   CHECK_EQ(agx_d3dkmt_local_allocation_handle(
               (uintptr_t)AGX_D3DKMT_LOCAL_ALLOCATION_TAG), 0);

   struct agx_d3dkmt_allocation_record borrowed = {
      .allocation = agx_d3dkmt_local_allocation_handle(tagged),
      .external_type = AGX_D3DKMT_EXTERNAL_MEMORY_OPAQUE_WIN32_KMT,
      .borrowed = true,
   };
   uint32_t borrowed_dense = agx_d3dkmt_pool_alloc_record(&pool, &borrowed);
   CHECK(borrowed_dense != 0);
   memset(&got, 0, sizeof(got));
   CHECK(agx_d3dkmt_pool_release_record(&pool, borrowed_dense, &got));
   CHECK_EQ(got.allocation, borrowed.allocation);
   CHECK_EQ(got.external_type, borrowed.external_type);
   CHECK(got.borrowed);

   borrowed.resource = 1;
   CHECK_EQ(agx_d3dkmt_pool_alloc_record(&pool, &borrowed), 0);
   borrowed.resource = 0;
   borrowed.global_share = 1;
   CHECK_EQ(agx_d3dkmt_pool_alloc_record(&pool, &borrowed), 0);
   borrowed.global_share = 0;
   borrowed.external_type = AGX_D3DKMT_EXTERNAL_MEMORY_OPAQUE_WIN32;
   CHECK_EQ(agx_d3dkmt_pool_alloc_record(&pool, &borrowed), 0);

   /* Churn: 4000 create/destroy cycles must not grow the dense space. */
   for (unsigned i = 0; i < 4000; ++i) {
      uint32_t h = agx_d3dkmt_pool_alloc(&pool, 0x40000000u + i);
      CHECK(h <= 6);
      CHECK_EQ(agx_d3dkmt_pool_release(&pool, h), 0x40000000u + i);
   }

   agx_d3dkmt_pool_finish(&pool);
}

static void
test_cpu_visible_executable_flags(void)
{
   CHECK_EQ(agx_d3dkmt_cpu_visible_flags(0), 0);
   CHECK_EQ(agx_d3dkmt_cpu_visible_flags(AGX_D3DKMT_BO_WRITEBACK),
            AGX_D3DKMT_BO_WRITEBACK);
   CHECK_EQ(agx_d3dkmt_cpu_visible_flags(AGX_D3DKMT_BO_EXEC),
            AGX_D3DKMT_BO_EXEC | AGX_D3DKMT_BO_WRITEBACK);
   CHECK_EQ(agx_d3dkmt_cpu_visible_flags(AGX_D3DKMT_BO_EXEC |
                                         AGX_D3DKMT_BO_LOW_VA),
            AGX_D3DKMT_BO_EXEC | AGX_D3DKMT_BO_LOW_VA |
               AGX_D3DKMT_BO_WRITEBACK);
}

/* ------------------------------------------------------------------------ */
static void
test_bind_plain(void)
{
   struct agx_d3dkmt_policy policy = agx_d3dkmt_policy_default();
   struct agx_kmt_updategpuva_op ops[8];
   uint32_t n = 0;

   struct agx_d3dkmt_bind rw = {
      .flags = AGX_D3DKMT_BIND_READ | AGX_D3DKMT_BIND_WRITE,
      .kmt_handle = 0x40000007u,
      .offset = AGX_D3DKMT_PAGE_SIZE,
      .range = 4 * AGX_D3DKMT_PAGE_SIZE,
      .addr = 0x1000000000ull,
   };

   CHECK_EQ(agx_d3dkmt_translate_binds(&rw, 1, &policy, ops, 8, &n),
            AGX_D3DKMT_OK);
   CHECK_EQ(n, 1);
   CHECK_EQ(ops[0].OperationType, AGX_KMT_UPDATEGPUVA_MAP_PROTECT);
   CHECK_EQ(ops[0].MapProtect.BaseAddress, 0x1000000000ull);
   CHECK_EQ(ops[0].MapProtect.SizeInBytes, 4 * AGX_D3DKMT_PAGE_SIZE);
   CHECK_EQ(ops[0].MapProtect.hAllocation, 0x40000007u);
   CHECK_EQ(ops[0].MapProtect.AllocationOffsetInBytes, AGX_D3DKMT_PAGE_SIZE);
   CHECK_EQ(ops[0].MapProtect.AllocationSizeInBytes, 4 * AGX_D3DKMT_PAGE_SIZE);
   CHECK(ops[0].MapProtect.Protection.Value & AGX_KMT_GPUVA_PROT_WRITE);
   /* Execute is NOT unconditional: it is derived from the USC window, which
    * this policy does not have (CONTRACT.md SS3.3, SS4.7). */
   CHECK(!(ops[0].MapProtect.Protection.Value & AGX_KMT_GPUVA_PROT_EXECUTE));
   CHECK_EQ(ops[0].MapProtect.DriverProtection, 0);

   /* READ without WRITE is read-only: WDDM has no Read bit, so the absence
    * of Write is the whole encoding. */
   struct agx_d3dkmt_bind ro = rw;
   ro.flags = AGX_D3DKMT_BIND_READ;
   CHECK_EQ(agx_d3dkmt_translate_binds(&ro, 1, &policy, ops, 8, &n),
            AGX_D3DKMT_OK);
   CHECK_EQ(n, 1);
   CHECK(!(ops[0].MapProtect.Protection.Value & AGX_KMT_GPUVA_PROT_WRITE));
}

static void
test_bind_unbind(void)
{
   struct agx_d3dkmt_policy policy = agx_d3dkmt_policy_default();
   struct agx_kmt_updategpuva_op ops[4];
   uint32_t n = 0;

   /* UNBIND carries no handle (asahi_drm.h: "@handle: GEM object to bind
    * (except for UNBIND)"), so a zero handle must be accepted here and
    * rejected everywhere else. */
   struct agx_d3dkmt_bind unbind = {
      .flags = AGX_D3DKMT_BIND_UNBIND,
      .kmt_handle = 0,
      .range = 2 * AGX_D3DKMT_PAGE_SIZE,
      .addr = 0x1800000000ull,
   };

   CHECK_EQ(agx_d3dkmt_translate_binds(&unbind, 1, &policy, ops, 4, &n),
            AGX_D3DKMT_OK);
   CHECK_EQ(n, 1);
   CHECK_EQ(ops[0].OperationType, AGX_KMT_UPDATEGPUVA_UNMAP);
   CHECK_EQ(ops[0].Unmap.BaseAddress, 0x1800000000ull);
   CHECK_EQ(ops[0].Unmap.SizeInBytes, 2 * AGX_D3DKMT_PAGE_SIZE);
   CHECK_EQ(ops[0].Unmap.Protection.Value, AGX_KMT_GPUVA_PROT_NOACCESS);

   struct agx_d3dkmt_bind no_handle = {
      .flags = AGX_D3DKMT_BIND_READ,
      .kmt_handle = 0,
      .range = AGX_D3DKMT_PAGE_SIZE,
      .addr = 0x1800000000ull,
   };
   CHECK_EQ(agx_d3dkmt_translate_binds(&no_handle, 1, &policy, ops, 4, &n),
            AGX_D3DKMT_ERR_INVAL);
}

static void
test_bind_alignment_is_rejected(void)
{
   struct agx_d3dkmt_policy policy = agx_d3dkmt_policy_default();
   struct agx_kmt_updategpuva_op ops[4];
   uint32_t n = 0;

   struct agx_d3dkmt_bind base = {
      .flags = AGX_D3DKMT_BIND_READ,
      .kmt_handle = 1,
      .offset = 0,
      .range = AGX_D3DKMT_PAGE_SIZE,
      .addr = 0x1400000000ull,
   };

   struct agx_d3dkmt_bind bad;

   bad = base;
   bad.range = 0;
   CHECK_EQ(agx_d3dkmt_translate_binds(&bad, 1, &policy, ops, 4, &n),
            AGX_D3DKMT_ERR_INVAL);

   bad = base;
   bad.range = 4096; /* 4 KiB is a WDDM page, not an AGX page */
   CHECK_EQ(agx_d3dkmt_translate_binds(&bad, 1, &policy, ops, 4, &n),
            AGX_D3DKMT_ERR_INVAL);

   bad = base;
   bad.addr = base.addr + 4096;
   CHECK_EQ(agx_d3dkmt_translate_binds(&bad, 1, &policy, ops, 4, &n),
            AGX_D3DKMT_ERR_INVAL);

   bad = base;
   bad.offset = 4096;
   CHECK_EQ(agx_d3dkmt_translate_binds(&bad, 1, &policy, ops, 4, &n),
            AGX_D3DKMT_ERR_INVAL);
}

static void
test_bind_single_page_is_one_op(void)
{
   struct agx_d3dkmt_policy policy = agx_d3dkmt_policy_default();
   struct agx_kmt_updategpuva_op ops[4];
   uint32_t n = 0;

   /*
    * CONTRACT.md SS4.7: BIND_SINGLE_PAGE is a DriverProtection bit, so a
    * 1 GiB sparse scratch bind (hk_bind_scratch(), hk_buffer.c:81-99) is ONE
    * page-table operation. Getting this wrong is not a correctness bug but a
    * 65536x one, so it is worth a test.
    */
   struct agx_d3dkmt_bind sp = {
      .flags = AGX_D3DKMT_BIND_READ | AGX_D3DKMT_BIND_WRITE |
               AGX_D3DKMT_BIND_SINGLE_PAGE,
      .kmt_handle = 0x40000009u,
      .offset = 2 * AGXW_PAGE_SIZE,
      .range = 1ull << 30,
      .addr = 0x1000000000ull,
   };

   CHECK_EQ(agx_d3dkmt_translate_binds(&sp, 1, &policy, ops, 4, &n),
            AGX_D3DKMT_OK);
   CHECK_EQ(n, 1);
   CHECK_EQ(ops[0].OperationType, AGX_KMT_UPDATEGPUVA_MAP_PROTECT);
   CHECK_EQ(ops[0].MapProtect.BaseAddress, sp.addr);
   CHECK_EQ(ops[0].MapProtect.SizeInBytes, 1ull << 30);
   /* The aliased source is a single page at `offset`, not the whole range. */
   CHECK_EQ(ops[0].MapProtect.AllocationOffsetInBytes, 2 * AGXW_PAGE_SIZE);
   CHECK_EQ(ops[0].MapProtect.AllocationSizeInBytes, AGXW_PAGE_SIZE);
   CHECK(ops[0].MapProtect.DriverProtection & AGXW_VAPROT_SINGLE_PAGE);

   /* And a plain bind must NOT set it. */
   sp.flags &= ~(uint32_t)AGX_D3DKMT_BIND_SINGLE_PAGE;
   sp.range = 4 * AGXW_PAGE_SIZE;
   CHECK_EQ(agx_d3dkmt_translate_binds(&sp, 1, &policy, ops, 4, &n),
            AGX_D3DKMT_OK);
   CHECK(!(ops[0].MapProtect.DriverProtection & AGXW_VAPROT_SINGLE_PAGE));
   CHECK_EQ(ops[0].MapProtect.AllocationSizeInBytes, 4 * AGXW_PAGE_SIZE);
}

static void
test_bind_write_only_and_usc(void)
{
   struct agx_d3dkmt_policy policy = agx_d3dkmt_policy_default();
   struct agx_kmt_updategpuva_op ops[4];
   uint32_t n = 0;

   policy.usc_base = 0x100000000ull; /* 4 GiB */
   policy.usc_size = 0x100000000ull; /* 4 GiB window */

   /* Write without read: WDDM has no encoding, so it rides in
    * DriverProtection (CONTRACT.md SS4.7). */
   struct agx_d3dkmt_bind wo = {
      .flags = AGX_D3DKMT_BIND_WRITE,
      .kmt_handle = 5,
      .range = AGXW_PAGE_SIZE,
      .addr = 0x1000000000ull,
   };
   CHECK_EQ(agx_d3dkmt_translate_binds(&wo, 1, &policy, ops, 4, &n),
            AGX_D3DKMT_OK);
   CHECK(ops[0].MapProtect.Protection.Value & AGX_KMT_GPUVA_PROT_WRITE);
   CHECK(ops[0].MapProtect.DriverProtection & AGXW_VAPROT_WRITE_ONLY);
   /* Not in the USC window: no Execute. */
   CHECK(!(ops[0].MapProtect.Protection.Value & AGX_KMT_GPUVA_PROT_EXECUTE));
   CHECK(!(ops[0].MapProtect.DriverProtection & AGXW_VAPROT_USC));

   /* Read+write is not write-only. */
   wo.flags = AGX_D3DKMT_BIND_READ | AGX_D3DKMT_BIND_WRITE;
   CHECK_EQ(agx_d3dkmt_translate_binds(&wo, 1, &policy, ops, 4, &n),
            AGX_D3DKMT_OK);
   CHECK(!(ops[0].MapProtect.DriverProtection & AGXW_VAPROT_WRITE_ONLY));

   /*
    * Inside the USC window the bind becomes executable. This is the whole of
    * AGX_BO_EXEC's expression under the contract: a bind op carries no
    * execute bit, so the address is the only signal (CONTRACT.md SS3.3).
    */
   struct agx_d3dkmt_bind usc = {
      .flags = AGX_D3DKMT_BIND_READ,
      .kmt_handle = 6,
      .range = 2 * AGXW_PAGE_SIZE,
      .addr = policy.usc_base,
   };
   CHECK_EQ(agx_d3dkmt_translate_binds(&usc, 1, &policy, ops, 4, &n),
            AGX_D3DKMT_OK);
   CHECK(ops[0].MapProtect.Protection.Value & AGX_KMT_GPUVA_PROT_EXECUTE);
   CHECK(ops[0].MapProtect.DriverProtection & AGXW_VAPROT_USC);

   /* A generic RW BO becomes RX after placement in the USC VA window. */
   usc.flags = AGX_D3DKMT_BIND_READ | AGX_D3DKMT_BIND_WRITE;
   CHECK_EQ(agx_d3dkmt_translate_binds(&usc, 1, &policy, ops, 4, &n),
            AGX_D3DKMT_OK);
   CHECK(ops[0].MapProtect.Protection.Value & AGX_KMT_GPUVA_PROT_EXECUTE);
   CHECK(!(ops[0].MapProtect.Protection.Value & AGX_KMT_GPUVA_PROT_WRITE));
   CHECK(!(ops[0].MapProtect.DriverProtection & AGXW_VAPROT_WRITE_ONLY));
   usc.flags = AGX_D3DKMT_BIND_READ;

   /* Last page of the window is still inside it. */
   usc.addr = policy.usc_base + policy.usc_size - AGXW_PAGE_SIZE;
   usc.range = AGXW_PAGE_SIZE;
   CHECK_EQ(agx_d3dkmt_translate_binds(&usc, 1, &policy, ops, 4, &n),
            AGX_D3DKMT_OK);
   CHECK(ops[0].MapProtect.DriverProtection & AGXW_VAPROT_USC);

   /* One page past it is not, and a range straddling the end is not. */
   usc.addr = policy.usc_base + policy.usc_size;
   CHECK_EQ(agx_d3dkmt_translate_binds(&usc, 1, &policy, ops, 4, &n),
            AGX_D3DKMT_OK);
   CHECK(!(ops[0].MapProtect.DriverProtection & AGXW_VAPROT_USC));

   usc.addr = policy.usc_base + policy.usc_size - AGXW_PAGE_SIZE;
   usc.range = 2 * AGXW_PAGE_SIZE;
   CHECK_EQ(agx_d3dkmt_translate_binds(&usc, 1, &policy, ops, 4, &n),
            AGX_D3DKMT_OK);
   CHECK(!(ops[0].MapProtect.DriverProtection & AGXW_VAPROT_USC));
}

static void
test_production_bind_policy_guard(void)
{
   struct agx_d3dkmt_policy runtime = agx_d3dkmt_policy_default();
   struct agx_d3dkmt_policy immutable;
   struct agx_kmt_updategpuva_op op;
   uint32_t n = 0;

   runtime.usc_base = AGX_D3DKMT_USC_MIN_BASE;
   runtime.usc_size = AGX_D3DKMT_USC_SIZE;
   CHECK_EQ(agx_d3dkmt_bind_policy(runtime.usc_base, &runtime, &immutable),
            AGX_D3DKMT_OK);
   CHECK_EQ(immutable.usc_base, AGX_D3DKMT_USC_MIN_BASE);
   CHECK_EQ(immutable.usc_size, AGX_D3DKMT_USC_SIZE);

   /* This is the exact production zero-page envelope after Windows pads the
    * native 16 KiB request to one 64 KiB VidMm page. */
   struct agx_d3dkmt_bind zero = {
      .flags = AGX_D3DKMT_BIND_READ,
      .kmt_handle = 7,
      .range = AGX_D3DKMT_RESERVATION_GRANULARITY,
      .addr = UINT64_C(0x100000000),
   };
   CHECK_EQ(agx_d3dkmt_translate_binds(&zero, 1, &immutable, &op, 1, &n),
            AGX_D3DKMT_OK);
   CHECK_EQ(n, 1);
   CHECK_EQ(op.MapProtect.DriverProtection, 0);
   CHECK_EQ(op.MapProtect.Protection.Value, 0);
   CHECK_EQ(agx_d3dkmt_validate_translated_binds(
               &zero, 1, &immutable, &op, 1),
            AGX_D3DKMT_OK);

   /* Reproduce the impossible callback shape from the 0x10e/0xb capture.
    * It must die in userspace before D3DKMTUpdateGpuVirtualAddress. */
   op.MapProtect.DriverProtection = AGXW_VAPROT_USC;
   CHECK_EQ(agx_d3dkmt_validate_translated_binds(
               &zero, 1, &immutable, &op, 1),
            AGX_D3DKMT_ERR_INVAL);
   op.MapProtect.Protection.Value = AGX_KMT_GPUVA_PROT_EXECUTE;
   CHECK_EQ(agx_d3dkmt_validate_translated_binds(
               &zero, 1, &immutable, &op, 1),
            AGX_D3DKMT_ERR_INVAL);

   /* A corrupt winsys policy cannot move the executable window onto the
    * fixed zero page, even if its base and size are internally consistent. */
   struct agx_d3dkmt_policy corrupt = runtime;
   corrupt.usc_base = UINT64_C(0x100000000);
   CHECK_EQ(agx_d3dkmt_bind_policy(corrupt.usc_base, &corrupt, &immutable),
            AGX_D3DKMT_ERR_INVAL);
   CHECK_EQ(agx_d3dkmt_bind_policy(runtime.usc_base, &corrupt, &immutable),
            AGX_D3DKMT_ERR_INVAL);
   CHECK_EQ(agx_d3dkmt_bind_policy(runtime.usc_base + AGXW_PAGE_SIZE,
                                   &runtime, &immutable),
            AGX_D3DKMT_ERR_INVAL);

   /* The first legal production USC mapping remains RX and passes the final
    * operation-array validation. */
   struct agx_d3dkmt_bind usc = {
      .flags = AGX_D3DKMT_BIND_READ | AGX_D3DKMT_BIND_WRITE,
      .kmt_handle = 8,
      .range = AGX_D3DKMT_RESERVATION_GRANULARITY,
      .addr = runtime.usc_base,
   };
   CHECK_EQ(agx_d3dkmt_bind_policy(runtime.usc_base, &runtime, &immutable),
            AGX_D3DKMT_OK);
   CHECK_EQ(agx_d3dkmt_translate_binds(&usc, 1, &immutable, &op, 1, &n),
            AGX_D3DKMT_OK);
   CHECK(op.MapProtect.DriverProtection & AGXW_VAPROT_USC);
   CHECK(op.MapProtect.Protection.Value & AGX_KMT_GPUVA_PROT_EXECUTE);
   CHECK(!(op.MapProtect.Protection.Value & AGX_KMT_GPUVA_PROT_WRITE));
   CHECK_EQ(agx_d3dkmt_validate_translated_binds(
               &usc, 1, &immutable, &op, 1),
            AGX_D3DKMT_OK);
}

static void
test_bind_rejects_kernel_carveout(void)
{
   struct agx_d3dkmt_policy policy = agx_d3dkmt_policy_default();
   struct agx_kmt_updategpuva_op ops[4];
   uint32_t n = 0;

   /*
    * CONTRACT.md SS4.2: a single 16 KiB kernel/firmware page sits in the
    * MIDDLE of the user VA window. Code that treats [vm_start, vm_end) as one
    * unbroken range is wrong; this is the test that says so.
    */
   struct agx_d3dkmt_bind on_it = {
      .flags = AGX_D3DKMT_BIND_READ,
      .kmt_handle = 1,
      .range = AGXW_PAGE_SIZE,
      .addr = AGXW_VM_KERNEL_CARVEOUT_BASE,
   };
   CHECK_EQ(agx_d3dkmt_translate_binds(&on_it, 1, &policy, ops, 4, &n),
            AGX_D3DKMT_ERR_INVAL);

   /* Straddling it from below. */
   struct agx_d3dkmt_bind straddle = on_it;
   straddle.addr = AGXW_VM_KERNEL_CARVEOUT_BASE - AGXW_PAGE_SIZE;
   straddle.range = 4 * AGXW_PAGE_SIZE;
   CHECK_EQ(agx_d3dkmt_translate_binds(&straddle, 1, &policy, ops, 4, &n),
            AGX_D3DKMT_ERR_INVAL);

   /* Immediately below and immediately above are both fine. */
   struct agx_d3dkmt_bind below = on_it;
   below.addr = AGXW_VM_KERNEL_CARVEOUT_BASE - AGXW_PAGE_SIZE;
   CHECK_EQ(agx_d3dkmt_translate_binds(&below, 1, &policy, ops, 4, &n),
            AGX_D3DKMT_OK);

   struct agx_d3dkmt_bind above = on_it;
   above.addr = AGXW_VM_KERNEL_CARVEOUT_BASE + AGXW_VM_KERNEL_CARVEOUT_SIZE;
   CHECK_EQ(agx_d3dkmt_translate_binds(&above, 1, &policy, ops, 4, &n),
            AGX_D3DKMT_OK);

   /* Outside the user window entirely. */
   struct agx_d3dkmt_bind low = on_it;
   low.addr = 0;
   CHECK_EQ(agx_d3dkmt_translate_binds(&low, 1, &policy, ops, 4, &n),
            AGX_D3DKMT_ERR_INVAL);
}

static void
test_va_split_carveout(void)
{
   struct agx_d3dkmt_va_range r[2];
   const uint64_t cb = AGXW_VM_KERNEL_CARVEOUT_BASE;
   const uint64_t cs = AGXW_VM_KERNEL_CARVEOUT_SIZE;
   const uint64_t gran = AGX_D3DKMT_RESERVATION_GRANULARITY;

   /* The 16 KiB AGX lower bound is rounded inward to WDDM's 64 KiB ABI. */
   CHECK_EQ(agx_d3dkmt_va_split_carveout(AGXW_VM_USER_START, cb - AGXW_VM_USER_START, r), 1);
   CHECK_EQ(r[0].base, gran);
   CHECK_EQ(r[0].size, cb - gran);

   /* A sub-64 KiB range cannot be reserved through D3DKMT. */
   CHECK_EQ(agx_d3dkmt_va_split_carveout(cb + cs, AGXW_PAGE_SIZE, r), 0);

   /* Spanning it omits the complete 64 KiB block containing the carve-out. */
   uint64_t base = cb - gran;
   uint64_t size = 3 * gran;
   CHECK_EQ(agx_d3dkmt_va_split_carveout(base, size, r), 2);
   CHECK_EQ(r[0].base, base);
   CHECK_EQ(r[0].size, gran);
   CHECK_EQ(r[1].base, cb + gran);
   CHECK_EQ(r[1].size, gran);
   CHECK_EQ(r[0].size + r[1].size + gran, size);

   /* Ending exactly at the block and starting exactly after it are legal. */
   CHECK_EQ(agx_d3dkmt_va_split_carveout(cb - gran, gran, r), 1);
   CHECK_EQ(agx_d3dkmt_va_split_carveout(cb + gran, gran, r), 1);

   /* Exactly the carve-out block: nothing reservable. */
   CHECK_EQ(agx_d3dkmt_va_split_carveout(cb, gran, r), 0);

   /* Starting on it and extending above: one aligned range above. */
   CHECK_EQ(agx_d3dkmt_va_split_carveout(cb, 2 * gran, r), 1);
   CHECK_EQ(r[0].base, cb + gran);
   CHECK_EQ(r[0].size, gran);

   CHECK_EQ(agx_d3dkmt_va_split_carveout(cb, 0, r), 0);

   /* The whole user window becomes two legal WDDM reservations. */
   CHECK_EQ(agx_d3dkmt_va_split_carveout(AGXW_VM_USER_START,
                                         AGXW_VM_USER_END - AGXW_VM_USER_START,
                                         r),
            2);
   CHECK_EQ(r[0].base % gran, 0);
   CHECK_EQ(r[0].size % gran, 0);
   CHECK_EQ(r[1].base % gran, 0);
   CHECK_EQ(r[1].size % gran, 0);
   CHECK_EQ(r[0].base, gran);
   CHECK_EQ(r[1].base, cb + gran);

   CHECK_EQ(agx_d3dkmt_va_split_carveout(UINT64_MAX - gran + 1, gran,
                                         r),
            0);
}

static void
test_bind_sizing_and_overflow(void)
{
   struct agx_d3dkmt_policy policy = agx_d3dkmt_policy_default();
   struct agx_kmt_updategpuva_op ops[2];
   uint32_t n = 0;

   /* Batching: hk_queue.c submits whole arrays of binds in one VM_BIND, and
    * CONTRACT.md SS4.4 wants that preserved as one UpdateGpuVirtualAddress
    * call for its atomicity. One AGX bind is one WDDM operation. */
   struct agx_d3dkmt_bind batch[3] = {
      {.flags = AGX_D3DKMT_BIND_READ,
       .kmt_handle = 1,
       .range = AGX_D3DKMT_PAGE_SIZE,
       .addr = 0x1600000000ull},
      {.flags = AGX_D3DKMT_BIND_UNBIND,
       .range = AGX_D3DKMT_PAGE_SIZE,
       .addr = 0x1600004000ull},
      {.flags = AGX_D3DKMT_BIND_READ | AGX_D3DKMT_BIND_WRITE,
       .kmt_handle = 2,
       .range = AGX_D3DKMT_PAGE_SIZE,
       .addr = 0x1600008000ull},
   };

   /* Sizing pass. */
   CHECK_EQ(agx_d3dkmt_translate_binds(batch, 3, &policy, NULL, 0, &n),
            AGX_D3DKMT_OK);
   CHECK_EQ(n, 3);

   /* Too small a buffer reports the requirement instead of overrunning. */
   unsigned char canary[sizeof(ops)];
   memset(ops, 0xAB, sizeof(ops));
   memset(canary, 0xAB, sizeof(canary));
   CHECK_EQ(agx_d3dkmt_translate_binds(batch, 3, &policy, ops, 2, &n),
            AGX_D3DKMT_ERR_NOSPACE);
   CHECK_EQ(n, 3);
   CHECK_EQ(memcmp(ops, canary, sizeof(ops)), 0);

   struct agx_kmt_updategpuva_op big[8];
   CHECK_EQ(agx_d3dkmt_translate_binds(batch, 3, &policy, big, 8, &n),
            AGX_D3DKMT_OK);
   CHECK_EQ(n, 3);
   CHECK_EQ(big[0].OperationType, AGX_KMT_UPDATEGPUVA_MAP_PROTECT);
   CHECK_EQ(big[1].OperationType, AGX_KMT_UPDATEGPUVA_UNMAP);
   CHECK_EQ(big[2].OperationType, AGX_KMT_UPDATEGPUVA_MAP_PROTECT);

   /* One bad bind in a batch fails the whole batch: VM_BIND is atomic and a
    * partially-applied page-table update is worse than none. */
   batch[1].range = 4096;
   CHECK_EQ(agx_d3dkmt_translate_binds(batch, 3, &policy, big, 8, &n),
            AGX_D3DKMT_ERR_INVAL);
   CHECK_EQ(n, 0);

   CHECK_EQ(agx_d3dkmt_translate_binds(NULL, 0, &policy, big, 8, &n),
            AGX_D3DKMT_OK);
   CHECK_EQ(n, 0);
}

/* ------------------------------------------------------------------------ */
static void
test_binary_sync_epoch(void)
{
   struct agx_d3dkmt_sync_table t;
   agx_d3dkmt_sync_table_init(&t);

   /* Model the kernel's CPU-visible fence storage (Q5). */
   volatile uint64_t fence = 0;

   uint32_t h = agx_d3dkmt_sync_table_add(&t, 0x40000011u, &fence, 0);
   CHECK(h != 0);
   struct agx_d3dkmt_sync *s = agx_d3dkmt_sync_table_get(&t, h);
   CHECK(s != NULL);

   /* Fresh syncobj is unsignalled: waiting wants 1, nothing has reached it. */
   CHECK_EQ(agx_d3dkmt_binary_wait_value(s), 1);
   CHECK_EQ(agx_d3dkmt_sync_current(s), 0);

   bool skip = true;
   CHECK_EQ(agx_d3dkmt_binary_signal_value(s, &skip), 1);
   CHECK(!skip);

   /* The winsys performs the signal and records it. */
   s->last_request = 1;
   fence = 1;
   CHECK_EQ(agx_d3dkmt_sync_current(s), 1);

   /* Signalling an already-signalled binary syncobj is legal in DRM and must
    * NOT become a fence rewind here. */
   CHECK_EQ(agx_d3dkmt_binary_signal_value(s, &skip), 1);
   CHECK(skip);

   /* Reset: the fence cannot go backwards, so the epoch moves forwards. */
   s->base = agx_d3dkmt_binary_reset_base(s);
   CHECK_EQ(s->base, 1);
   CHECK_EQ(agx_d3dkmt_binary_wait_value(s), 2);
   CHECK_EQ(agx_d3dkmt_binary_signal_value(s, &skip), 2);
   CHECK(!skip);

   /* And the fence value really did not move. */
   CHECK_EQ(fence, 1);

   /* Signal/reset round trips must keep working indefinitely. */
   for (unsigned i = 0; i < 100; ++i) {
      bool sk = true;
      uint64_t v = agx_d3dkmt_binary_signal_value(s, &sk);
      CHECK(!sk);
      s->last_request = v;
      fence = v;
      CHECK(agx_d3dkmt_sync_current(s) >= agx_d3dkmt_binary_wait_value(s));
      s->base = agx_d3dkmt_binary_reset_base(s);
      CHECK(agx_d3dkmt_sync_current(s) < agx_d3dkmt_binary_wait_value(s));
   }

   /* Timeline use of the same object never touches base, so point N is
    * fence value N exactly. */
   uint32_t h2 = agx_d3dkmt_sync_table_add(&t, 0x40000012u, NULL, 0);
   struct agx_d3dkmt_sync *s2 = agx_d3dkmt_sync_table_get(&t, h2);
   CHECK_EQ(s2->base, 0);
   s2->last_request = 42;
   CHECK_EQ(agx_d3dkmt_sync_current(s2), 42);

   /* Without a CPU mapping, query() reports what we asked for. Documented
    * degradation, not silent: this is the Q5 fallback. */
   CHECK(s2->cpu_va == NULL);

   /* DRM transfer point zero is the binary payload, not monitored-fence
    * value zero.  QueueWaitIdle depends on this exact timeline->binary
    * conversion: signalling zero would leave its fresh binary wait stuck on
    * base + 1 forever.  The inverse conversion protects binary queue waits. */
   uint64_t src_value = 0, dst_value = 0;
   agx_d3dkmt_sync_transfer_values(s2, s, 42, 0, &src_value, &dst_value);
   CHECK_EQ(src_value, 42);
   CHECK_EQ(dst_value, agx_d3dkmt_binary_signal_value(s, NULL));
   agx_d3dkmt_sync_transfer_values(s, s2, 0, 43, &src_value, &dst_value);
   CHECK_EQ(src_value, agx_d3dkmt_binary_wait_value(s));
   CHECK_EQ(dst_value, 43);
   agx_d3dkmt_sync_transfer_values(s2, s2, 7, 9, &src_value, &dst_value);
   CHECK_EQ(src_value, 7);
   CHECK_EQ(dst_value, 9);

   CHECK_EQ(agx_d3dkmt_sync_table_remove(&t, h), 0x40000011u);
   CHECK(agx_d3dkmt_sync_table_get(&t, h) == NULL);
   CHECK_EQ(agx_d3dkmt_sync_table_remove(&t, h), 0);
   CHECK_EQ(agx_d3dkmt_sync_table_add(&t, 0x40000013u, NULL, 0), h);

   agx_d3dkmt_sync_table_finish(&t);
}

static void
test_sync_current_takes_the_max(void)
{
   struct agx_d3dkmt_sync s;
   volatile uint64_t fence = 7;

   memset(&s, 0, sizeof(s));
   s.kmt = 1;
   s.cpu_va = &fence;

   /* Kernel ahead of us (e.g. a shared fence another process signalled). */
   s.last_request = 3;
   CHECK_EQ(agx_d3dkmt_sync_current(&s), 7);

   /* Us ahead of the kernel (a submitted-but-not-retired signal). Taking the
    * max is what stops signal() from issuing a rewinding duplicate. */
   s.last_request = 11;
   CHECK_EQ(agx_d3dkmt_sync_current(&s), 11);
}

/* ------------------------------------------------------------------------ */
static void
test_timeout_conversion(void)
{
   const int64_t now = 1000ll * 1000 * 1000 * 100; /* 100 s */

   /* INT64_MAX is DRM's "wait forever". */
   CHECK_EQ(agx_d3dkmt_timeout_to_ms(INT64_MAX, now), AGX_D3DKMT_INFINITE);

   /* A deadline already in the past is a poll, not a hang. */
   CHECK_EQ(agx_d3dkmt_timeout_to_ms(now, now), 0);
   CHECK_EQ(agx_d3dkmt_timeout_to_ms(now - 1, now), 0);
   CHECK_EQ(agx_d3dkmt_timeout_to_ms(0, now), 0);

   /* Exact conversion. */
   CHECK_EQ(agx_d3dkmt_timeout_to_ms(now + 5000000, now), 5);

   /* Sub-millisecond deadlines round UP: rounding down would turn a real
    * wait into a zero-length poll and spin. */
   CHECK_EQ(agx_d3dkmt_timeout_to_ms(now + 1, now), 1);
   CHECK_EQ(agx_d3dkmt_timeout_to_ms(now + 999999, now), 1);
   CHECK_EQ(agx_d3dkmt_timeout_to_ms(now + 1000001, now), 2);

   /* A long-but-finite deadline must never be reported as INFINITE. */
   int64_t huge = now + (int64_t)5000000 * 1000000000ll;
   CHECK_EQ(agx_d3dkmt_timeout_to_ms(huge, now), AGX_D3DKMT_INFINITE - 1);
   CHECK(agx_d3dkmt_timeout_to_ms(INT64_MAX - 1, now) != AGX_D3DKMT_INFINITE);
}

static void
test_first_signaled(void)
{
   const uint64_t current[4] = {0, 5, 9, 100};
   const uint64_t wanted[4] = {1, 9, 9, 1};

   CHECK_EQ(agx_d3dkmt_first_signaled(current, wanted, 4), 2);
   CHECK_EQ(agx_d3dkmt_first_signaled(current, wanted, 2), UINT32_MAX);
   CHECK_EQ(agx_d3dkmt_first_signaled(current, wanted, 0), UINT32_MAX);

   const uint64_t all_short[2] = {0, 0};
   const uint64_t all_want[2] = {1, 1};
   CHECK_EQ(agx_d3dkmt_first_signaled(all_short, all_want, 2), UINT32_MAX);
}

static void
test_cpu_wait_probe(void)
{
   volatile uint64_t fence[3] = {4, 9, 2};
   const volatile uint64_t *current[3] = {&fence[0], &fence[1], &fence[2]};
   const uint64_t wanted[3] = {5, 9, 3};
   uint32_t first = UINT32_MAX;

   CHECK_EQ(agx_d3dkmt_cpu_wait_probe(current, wanted, 3, true, &first),
            AGX_D3DKMT_CPU_WAIT_SATISFIED);
   CHECK_EQ(first, 1);
   CHECK_EQ(agx_d3dkmt_cpu_wait_probe(current, wanted, 3, false, &first),
            AGX_D3DKMT_CPU_WAIT_PENDING);

   fence[0] = 5;
   fence[2] = 3;
   first = UINT32_MAX;
   CHECK_EQ(agx_d3dkmt_cpu_wait_probe(current, wanted, 3, false, &first),
            AGX_D3DKMT_CPU_WAIT_SATISFIED);
   CHECK_EQ(first, 0);

   const volatile uint64_t *missing[1] = {NULL};
   const volatile uint64_t *missing_after_pending[2] = {&fence[0], NULL};
   const uint64_t one[1] = {1};
   const uint64_t two[2] = {UINT64_MAX, 1};
   CHECK_EQ(agx_d3dkmt_cpu_wait_probe(NULL, one, 1, true, NULL),
            AGX_D3DKMT_CPU_WAIT_INVALID);
   CHECK_EQ(agx_d3dkmt_cpu_wait_probe(missing, one, 1, true, NULL),
            AGX_D3DKMT_CPU_WAIT_INVALID);
   CHECK_EQ(agx_d3dkmt_cpu_wait_probe(missing_after_pending, two, 2, false,
                                      NULL),
            AGX_D3DKMT_CPU_WAIT_INVALID);
   CHECK_EQ(agx_d3dkmt_cpu_wait_probe(current, NULL, 1, true, NULL),
            AGX_D3DKMT_CPU_WAIT_INVALID);
   CHECK_EQ(agx_d3dkmt_cpu_wait_probe(current, wanted, 0, true, NULL),
            AGX_D3DKMT_CPU_WAIT_INVALID);
}

struct cpu_wait_worker {
   atomic_bool *start;
   bool failed;
   unsigned seed;
};

static void *
cpu_wait_worker(void *data)
{
   struct cpu_wait_worker *worker = data;
   while (!atomic_load_explicit(worker->start, memory_order_acquire))
      sched_yield();

   for (unsigned iteration = 0; iteration < 20000; ++iteration) {
      volatile uint64_t fence[3] = {
         worker->seed + iteration,
         worker->seed + iteration + 2,
         worker->seed + iteration + 1,
      };
      const volatile uint64_t *current[3] = {
         &fence[0], &fence[1], &fence[2],
      };
      const uint64_t wanted[3] = {
         worker->seed + iteration + 1,
         worker->seed + iteration + 2,
         worker->seed + iteration + 2,
      };
      uint32_t first = UINT32_MAX;

      if (agx_d3dkmt_cpu_wait_probe(current, wanted, 3, true, &first) !=
             AGX_D3DKMT_CPU_WAIT_SATISFIED ||
          first != 1 ||
          agx_d3dkmt_cpu_wait_probe(current, wanted, 3, false, NULL) !=
             AGX_D3DKMT_CPU_WAIT_PENDING) {
         worker->failed = true;
         return NULL;
      }

      fence[0] = wanted[0];
      fence[2] = wanted[2];
      if (agx_d3dkmt_cpu_wait_probe(current, wanted, 3, false, &first) !=
             AGX_D3DKMT_CPU_WAIT_SATISFIED ||
          first != 0) {
         worker->failed = true;
         return NULL;
      }
   }

   return NULL;
}

static void
test_cpu_wait_probe_is_reentrant(void)
{
   enum { THREAD_COUNT = 8 };
   pthread_t threads[THREAD_COUNT];
   struct cpu_wait_worker workers[THREAD_COUNT];
   atomic_bool start = ATOMIC_VAR_INIT(false);
   unsigned created = 0;

   for (unsigned i = 0; i < THREAD_COUNT; ++i) {
      workers[i] = (struct cpu_wait_worker){
         .start = &start,
         .seed = i * 100000,
      };
      if (pthread_create(&threads[i], NULL, cpu_wait_worker, &workers[i]) != 0)
         break;
      created++;
   }

   CHECK_EQ(created, THREAD_COUNT);
   atomic_store_explicit(&start, true, memory_order_release);
   for (unsigned i = 0; i < created; ++i) {
      CHECK_EQ(pthread_join(threads[i], NULL), 0);
      CHECK(!workers[i].failed);
   }
}

/* ------------------------------------------------------------------------ */
static void
test_identity_parsing(void)
{
   struct agx_d3dkmt_policy policy;
   struct agxw_adapter_identity out;
   struct agxw_adapter_identity id = {
      .magic = AGXW_ADAPTER_IDENTITY_MAGIC,
      .version = AGXW_ADAPTER_IDENTITY_VERSION,
      .struct_size = sizeof(struct agxw_adapter_identity),
      .chip_id = AGXW_J414S_CHIP_ID,
      .gpu_generation = AGXW_J414S_GPU_GENERATION,
      .gpu_variant = AGXW_J414S_GPU_VARIANT,
      .gpu_revision = AGXW_J414S_GPU_REVISION,
      .contract_version = AGXW_CONTRACT_VERSION,
      .params_blob_size = AGXW_PARAMS_GLOBAL_SIZE,
   };

   CHECK_EQ(agx_d3dkmt_parse_identity(&id, sizeof(id), &policy, &out),
            AGX_D3DKMT_OK);
   CHECK_EQ(out.chip_id, 0x6020u);
   CHECK_EQ(out.gpu_variant, 'S');
   CHECK_EQ(policy.node_ordinal, AGXW_NODE_3D);
   CHECK(policy.needs_residency);

   /*
    * A foreign vendor's UMDRIVERPRIVATE blob must be rejected, not misparsed.
    * This is the entire adapter discriminator: CONTRACT.md SS2.3 rules out
    * VendorId/DeviceId on an ACPI-enumerated adapter with no PCI config
    * space, so if this check is wrong Honeykrisp claims someone else's GPU.
    */
   struct agxw_adapter_identity bad = id;
   bad.magic = 0x12345678u;
   CHECK_EQ(agx_d3dkmt_parse_identity(&bad, sizeof(bad), &policy, &out),
            AGX_D3DKMT_ERR_INVAL);

   /* Contract skew in either direction is a hard failure. */
   bad = id;
   bad.contract_version = AGXW_CONTRACT_VERSION + 1;
   CHECK_EQ(agx_d3dkmt_parse_identity(&bad, sizeof(bad), &policy, &out),
            AGX_D3DKMT_ERR_INVAL);

   bad = id;
   bad.version = AGXW_ADAPTER_IDENTITY_VERSION + 1;
   CHECK_EQ(agx_d3dkmt_parse_identity(&bad, sizeof(bad), &policy, &out),
            AGX_D3DKMT_ERR_INVAL);

   /* A params blob we would mis-size is a hard failure too: the escape
    * payload size is pinned at 592 on both sides. */
   bad = id;
   bad.params_blob_size = 512;
   CHECK_EQ(agx_d3dkmt_parse_identity(&bad, sizeof(bad), &policy, &out),
            AGX_D3DKMT_ERR_INVAL);

   /* Width is asserted, never inferred. */
   CHECK_EQ(agx_d3dkmt_parse_identity(&id, sizeof(id) - 1, &policy, &out),
            AGX_D3DKMT_ERR_INVAL);
   CHECK_EQ(agx_d3dkmt_parse_identity(&id, 0, &policy, &out),
            AGX_D3DKMT_ERR_INVAL);

   bad = id;
   bad.struct_size = sizeof(bad) + 8;
   CHECK_EQ(agx_d3dkmt_parse_identity(&bad, sizeof(bad), &policy, &out),
            AGX_D3DKMT_ERR_INVAL);
}

static void
test_alloc_flags(void)
{
   /*
    * CONTRACT.md SS3.3: LOW_VA and EXEC must NOT appear in the allocation
    * blob. They are a VA-placement property and a mapping-protection property
    * respectively -- true on Linux too, where drm_asahi_gem_flags has exactly
    * WRITEBACK and VM_PRIVATE. Putting them here would have been wrong in a
    * way that surfaced late.
    */
   uint32_t exec = agx_d3dkmt_agxw_alloc_flags(AGX_D3DKMT_BO_EXEC |
                                               AGX_D3DKMT_BO_LOW_VA);
   CHECK_EQ(exec & ~(uint32_t)(AGXW_ALLOC_VM_PRIVATE), 0);

   CHECK(agx_d3dkmt_agxw_alloc_flags(AGX_D3DKMT_BO_WRITEBACK) &
         AGXW_ALLOC_WRITEBACK);
   CHECK(!(agx_d3dkmt_agxw_alloc_flags(0) & AGXW_ALLOC_WRITEBACK));
   CHECK(agx_d3dkmt_agxw_alloc_flags(AGX_D3DKMT_BO_SHAREABLE |
                                     AGX_D3DKMT_BO_SCANOUT_DONOR) ==
         (AGXW_ALLOC_SHAREABLE | AGXW_ALLOC_SCANOUT_DONOR));
   CHECK(!(agx_d3dkmt_agxw_alloc_flags(AGX_D3DKMT_BO_SHAREABLE) &
           AGXW_ALLOC_SCANOUT_DONOR));

   /* Shareable and VM-private are mutually exclusive by contract. */
   uint32_t shared = agx_d3dkmt_agxw_alloc_flags(AGX_D3DKMT_BO_SHARED);
   CHECK(shared & AGXW_ALLOC_VM_PRIVATE);
   CHECK(!(shared & AGXW_ALLOC_SHAREABLE));
   CHECK(!(agx_d3dkmt_create_flags(AGX_D3DKMT_BO_SHARED) &
           AGX_KMT_CREATEALLOC_CREATERESOURCE));

   uint32_t shareable = agx_d3dkmt_agxw_alloc_flags(AGX_D3DKMT_BO_SHAREABLE);
   CHECK(shareable & AGXW_ALLOC_SHAREABLE);
   CHECK(!(shareable & AGXW_ALLOC_VM_PRIVATE));

   uint32_t priv = agx_d3dkmt_agxw_alloc_flags(AGX_D3DKMT_BO_WRITEBACK);
   CHECK(priv & AGXW_ALLOC_VM_PRIVATE);
   CHECK(!(priv & AGXW_ALLOC_SHAREABLE));

   /* Nothing outside the contract's flag set is ever produced. */
   for (uint32_t f = 0; f < 64; ++f) {
      CHECK_EQ(agx_d3dkmt_agxw_alloc_flags(f) & ~(uint32_t)AGXW_ALLOC_FLAGS_ALL,
               0);
   }

   /* And the blob the miniport will validate must actually validate. */
   struct agxw_alloc_private alloc = {
      .version = AGXW_ALLOC_PRIVATE_VERSION,
      .struct_size = sizeof(struct agxw_alloc_private),
      .size = 4 * AGXW_PAGE_SIZE,
      .flags = agx_d3dkmt_agxw_alloc_flags(AGX_D3DKMT_BO_WRITEBACK),
      .align_log2 = AGXW_PAGE_BITS,
   };
   CHECK_EQ(agxw_alloc_private_validate(&alloc, sizeof(alloc)), 0);
   CHECK_EQ(sizeof(alloc), AGXW_ALLOC_PRIVATE_SIZE);

   /* D3DKMT-side creation flags. */
   uint32_t cf =
      agx_d3dkmt_create_flags(AGX_D3DKMT_BO_SHAREABLE);
   CHECK(cf & AGX_KMT_CREATEALLOC_CREATERESOURCE);
   CHECK(cf & AGX_KMT_CREATEALLOC_CREATESHARED);
   /* NT-handle sharing, not the legacy global handle: a global shared handle
    * cannot be duplicated into another process the way
    * VK_KHR_external_memory_win32 and D3D11.1 shared NT handles require. */
   CHECK(cf & AGX_KMT_CREATEALLOC_NTSECURITYSHARING);
   CHECK(!(agx_d3dkmt_create_flags(0) & AGX_KMT_CREATEALLOC_CREATESHARED));

   uint32_t kmt = agx_d3dkmt_create_flags_external(
      AGX_D3DKMT_BO_SHARED,
      AGX_D3DKMT_EXTERNAL_MEMORY_OPAQUE_WIN32_KMT);
   CHECK(kmt & AGX_KMT_CREATEALLOC_CREATERESOURCE);
   CHECK(kmt & AGX_KMT_CREATEALLOC_CREATESHARED);
   CHECK(!(kmt & AGX_KMT_CREATEALLOC_NTSECURITYSHARING));

   /* ExistingSysMem is refused by the miniport (CONTRACT.md SS3.6) because
    * AGX BOs must be 16 KiB physically chunked. Never request it. */
   CHECK(!(agx_d3dkmt_create_flags(0xFFFFFFFFu) &
           AGX_KMT_CREATEALLOC_EXISTINGSYSMEM));
}

static void
test_read_range_canonicalization(void)
{
   struct agxw_submit_read_range ranges[4] = {0};
   uint32_t count = 0;

   /* The submit ABI describes native-page ranges. A byte-exact DMA payload
    * must therefore be represented by its containing staging allocation. */
   CHECK_EQ(agx_d3dkmt_read_range_add(
               ranges, &count, 4, 0x1500000000ull, 0x100),
            AGX_D3DKMT_ERR_INVAL);
   CHECK_EQ(count, 0);

   CHECK_EQ(agx_d3dkmt_read_range_add(
               ranges, &count, 4, 0x1500040000ull, 0x4000),
            AGX_D3DKMT_OK);
   CHECK_EQ(agx_d3dkmt_read_range_add(
               ranges, &count, 4, 0x1500000000ull, 0x4000),
            AGX_D3DKMT_OK);
   CHECK_EQ(count, 2);
   CHECK_EQ(ranges[0].gpu_va, 0x1500000000ull);

   /* Adjacent executable-pool slices coalesce into one native range. */
   CHECK_EQ(agx_d3dkmt_read_range_add(
               ranges, &count, 4, 0x1500004000ull, 0x3c000),
            AGX_D3DKMT_OK);
   CHECK_EQ(count, 1);
   CHECK_EQ(ranges[0].length, 0x44000);

   CHECK(agx_d3dkmt_read_range_add(
            ranges, &count, 4, UINT64_MAX - 1, 4) != AGX_D3DKMT_OK);

   count = 0;
   for (uint32_t i = 0; i < 4; i++)
      CHECK_EQ(agx_d3dkmt_read_range_add(
                  ranges, &count, 4,
                  0x1600000000ull + (uint64_t)i * 0x8000, 0x4000),
               AGX_D3DKMT_OK);
   CHECK_EQ(agx_d3dkmt_read_range_add(
               ranges, &count, 4, 0x1700000000ull, 0x4000),
            AGX_D3DKMT_ERR_NOSPACE);
}

static void
test_read_range_compaction(void)
{
   enum { RANGE_COUNT = 300, CAPACITY = 256 };
   struct agxw_submit_read_range ranges[RANGE_COUNT] = {0};
   const uint64_t base = UINT64_C(0x1800000000);
   const uint64_t stride = UINT64_C(0x14000);
   uint32_t count = 0;
   uint64_t bridged = 0;

   for (uint32_t i = 0; i < RANGE_COUNT; i++) {
      CHECK_EQ(agx_d3dkmt_read_range_add(
                  ranges, &count, RANGE_COUNT,
                  base + (uint64_t)i * stride, AGX_D3DKMT_PAGE_SIZE),
               AGX_D3DKMT_OK);
   }
   CHECK_EQ(count, RANGE_COUNT);
   CHECK_EQ(agx_d3dkmt_read_ranges_compact(
               ranges, &count, CAPACITY, &bridged),
            AGX_D3DKMT_OK);
   CHECK_EQ(count, CAPACITY);
   CHECK_EQ(bridged,
            (uint64_t)(RANGE_COUNT - CAPACITY) *
               (stride - AGX_D3DKMT_PAGE_SIZE));

   /* Compaction may cover guards but may not omit one byte of an input BO. */
   uint32_t output = 0;
   for (uint32_t input = 0; input < RANGE_COUNT; input++) {
      const uint64_t start = base + (uint64_t)input * stride;
      const uint64_t end = start + AGX_D3DKMT_PAGE_SIZE;
      while (output + 1 < count &&
             ranges[output].gpu_va + ranges[output].length <= start)
         output++;
      CHECK(ranges[output].gpu_va <= start);
      CHECK(ranges[output].gpu_va + ranges[output].length >= end);
   }

   /* The protected carveout divides the user window into two legal spans.
    * Even when its numerical gap is smallest, it is never bridgeable. */
   struct agxw_submit_read_range split[4] = {
      {AGXW_VM_KERNEL_CARVEOUT_BASE - UINT64_C(0x10000),
       AGX_D3DKMT_PAGE_SIZE},
      {AGXW_VM_KERNEL_CARVEOUT_BASE - UINT64_C(0x8000),
       AGX_D3DKMT_PAGE_SIZE},
      {AGXW_VM_KERNEL_CARVEOUT_BASE + AGXW_VM_KERNEL_CARVEOUT_SIZE,
       AGX_D3DKMT_PAGE_SIZE},
      {AGXW_VM_KERNEL_CARVEOUT_BASE + AGXW_VM_KERNEL_CARVEOUT_SIZE +
          UINT64_C(0x8000),
       AGX_D3DKMT_PAGE_SIZE},
   };
   count = 4;
   bridged = 0;
   CHECK_EQ(agx_d3dkmt_read_ranges_compact(split, &count, 2, &bridged),
            AGX_D3DKMT_OK);
   CHECK_EQ(count, 2);
   CHECK_EQ(bridged, UINT64_C(0x8000));
   CHECK(agxw_va_range_is_user(split[0].gpu_va, split[0].length));
   CHECK(agxw_va_range_is_user(split[1].gpu_va, split[1].length));
   CHECK(split[0].gpu_va + split[0].length <=
         AGXW_VM_KERNEL_CARVEOUT_BASE);
   CHECK(split[1].gpu_va >=
         AGXW_VM_KERNEL_CARVEOUT_BASE + AGXW_VM_KERNEL_CARVEOUT_SIZE);
}

static void
test_dirty_interval_merge_and_overflow(void)
{
   const uint64_t bo_size = 0x50000;
   struct agx_d3dkmt_dirty_state state;
   agx_d3dkmt_dirty_reset(&state, 17);

   CHECK_EQ(agx_d3dkmt_dirty_mark(&state, bo_size, 0x4100, 0x20),
            AGX_D3DKMT_DIRTY_OK);
   CHECK_EQ(agx_d3dkmt_dirty_mark(&state, bo_size, 0x8000, 0x4000),
            AGX_D3DKMT_DIRTY_OK);
   CHECK_EQ(state.range_count, 1);
   CHECK_EQ(state.ranges[0].offset, 0x4000);
   CHECK_EQ(state.ranges[0].length, 0x8000);

   agx_d3dkmt_dirty_reset(&state, 18);
   for (uint32_t i = 0; i < AGX_D3DKMT_DIRTY_RANGES_PER_BO; i++) {
      CHECK_EQ(agx_d3dkmt_dirty_mark(
                  &state, bo_size, (uint64_t)i * 0x8000, 1),
               AGX_D3DKMT_DIRTY_OK);
   }
   CHECK(!state.whole_bo);
   CHECK_EQ(state.range_count, AGX_D3DKMT_DIRTY_RANGES_PER_BO);
   CHECK_EQ(agx_d3dkmt_dirty_mark(&state, bo_size, 0x44000, 1),
            AGX_D3DKMT_DIRTY_OK);
   CHECK(state.whole_bo);
   CHECK_EQ(state.range_count, 0);
}

static void
test_dirty_detach_restore_and_stale_identity(void)
{
   struct agx_d3dkmt_dirty_state state;
   struct agx_d3dkmt_dirty_snapshot snapshot;
   agx_d3dkmt_dirty_reset(&state, 91);
   CHECK_EQ(agx_d3dkmt_dirty_mark(&state, 0x20000, 0x8000, 4),
            AGX_D3DKMT_DIRTY_OK);
   const uint64_t write_epoch = state.cpu_write_epoch;
   CHECK(agx_d3dkmt_dirty_detach(&state, &snapshot));
   CHECK(!agx_d3dkmt_dirty_has_pending(&state));

   /* A concurrent write after detach is retained when a failed submission
    * restores its older snapshot. */
   CHECK_EQ(agx_d3dkmt_dirty_mark(&state, 0x20000, 0x10000, 4),
            AGX_D3DKMT_DIRTY_OK);
   CHECK_EQ(agx_d3dkmt_dirty_restore(&state, 0x20000, &snapshot),
            AGX_D3DKMT_DIRTY_OK);
   CHECK_EQ(state.range_count, 2);
   CHECK(state.cpu_write_epoch > write_epoch);

   /* Cache reuse establishes a new identity. An old submission may neither
    * restore nor commit state into that new logical BO lifetime. */
   agx_d3dkmt_dirty_reset(&state, 92);
   CHECK_EQ(agx_d3dkmt_dirty_restore(&state, 0x20000, &snapshot),
            AGX_D3DKMT_DIRTY_STALE);
   CHECK_EQ(agx_d3dkmt_dirty_commit(&state, &snapshot),
            AGX_D3DKMT_DIRTY_STALE);
   CHECK(!agx_d3dkmt_dirty_has_pending(&state));
}

static void
test_dirty_repeated_unchanged_submit(void)
{
   struct agx_d3dkmt_dirty_state state;
   struct agx_d3dkmt_dirty_snapshot snapshot;
   agx_d3dkmt_dirty_reset(&state, 123);
   CHECK_EQ(agx_d3dkmt_dirty_mark(&state, 0x10000, 0, 4),
            AGX_D3DKMT_DIRTY_OK);
   CHECK(agx_d3dkmt_dirty_detach(&state, &snapshot));
   CHECK_EQ(agx_d3dkmt_dirty_commit(&state, &snapshot),
            AGX_D3DKMT_DIRTY_OK);
   CHECK_EQ(state.clean_epoch, snapshot.cpu_write_epoch);
   CHECK(!agx_d3dkmt_dirty_detach(&state, &snapshot));
}

static void
test_dirty_cached_mapping_reuse(void)
{
   struct agx_d3dkmt_dirty_state state;
   struct agx_d3dkmt_dirty_snapshot snapshot;

   agx_d3dkmt_dirty_reset_mapped(&state, 801, 0x20000, false);
   CHECK(!agx_d3dkmt_dirty_has_pending(&state));

   /* The cache keeps Lock2's pointer while changing logical BO identity.
    * Reuse must publish the entire new mapped lifetime before refcnt becomes
    * visible, even when no new mmap callback will occur. */
   agx_d3dkmt_dirty_reset_mapped(&state, 802, 0x20000, true);
   CHECK(agx_d3dkmt_dirty_has_pending(&state));
   CHECK(agx_d3dkmt_dirty_detach(&state, &snapshot));
   CHECK_EQ(snapshot.identity, 802);
   CHECK(snapshot.whole_bo);
   CHECK_EQ(snapshot.range_count, 0);
   CHECK_EQ(agx_d3dkmt_dirty_commit(&state, &snapshot),
            AGX_D3DKMT_DIRTY_OK);
   CHECK(!agx_d3dkmt_dirty_has_pending(&state));
}

static void
test_tracked_map_first_snapshot_accept_restore_and_exact_writes(void)
{
   const uint64_t bo_size = 0x40000;
   struct agx_d3dkmt_dirty_state dirty;
   struct agx_d3dkmt_dirty_snapshot snapshot;
   struct agx_d3dkmt_cpu_map_state map;

   agx_d3dkmt_dirty_reset(&dirty, 901);
   agx_d3dkmt_cpu_map_reset(&map, false);
   CHECK(agx_d3dkmt_cpu_map_request(
      &map, &dirty, bo_size, AGX_D3DKMT_CPU_MAP_TRACKED, true));
   CHECK_EQ(map.map_class, AGX_D3DKMT_CPU_MAP_TRACKED);
   CHECK(!map.conservative);
   CHECK(agx_d3dkmt_dirty_detach(&dirty, &snapshot));
   CHECK(snapshot.whole_bo);

   /* A rejected SubmitCommand restores the initial ownership snapshot. */
   CHECK_EQ(agx_d3dkmt_dirty_restore(&dirty, bo_size, &snapshot),
            AGX_D3DKMT_DIRTY_OK);
   CHECK(agx_d3dkmt_dirty_detach(&dirty, &snapshot));
   CHECK(snapshot.whole_bo);

   /* Once accepted, an unchanged tracked mapping contributes no clean range;
    * the next proven CPU write contributes only its aligned interval. */
   CHECK_EQ(agx_d3dkmt_dirty_commit(&dirty, &snapshot),
            AGX_D3DKMT_DIRTY_OK);
   CHECK(!agx_d3dkmt_dirty_has_pending(&dirty));
   CHECK(agx_d3dkmt_cpu_map_request(
      &map, &dirty, bo_size, AGX_D3DKMT_CPU_MAP_TRACKED, true));
   CHECK(!agx_d3dkmt_dirty_has_pending(&dirty));
   CHECK_EQ(agx_d3dkmt_dirty_mark(&dirty, bo_size, 0x14123, 0x800),
            AGX_D3DKMT_DIRTY_OK);
   CHECK(agx_d3dkmt_dirty_detach(&dirty, &snapshot));
   CHECK(!snapshot.whole_bo);
   CHECK_EQ(snapshot.range_count, 1);
   CHECK_EQ(snapshot.ranges[0].offset, 0x14000);
   CHECK_EQ(snapshot.ranges[0].length, 0x4000);
}

static void
test_tracked_map_cache_reuse_and_conservative_exclusions(void)
{
   const uint64_t bo_size = 0x20000;
   struct agx_d3dkmt_dirty_state dirty;
   struct agx_d3dkmt_dirty_snapshot snapshot;
   struct agx_d3dkmt_cpu_map_state map;

   /* A cached Lock2 pointer starts UNKNOWN with a whole snapshot. Claiming it
    * as tracked preserves that one snapshot, then becomes precise. */
   agx_d3dkmt_dirty_reset_mapped(&dirty, 910, bo_size, true);
   agx_d3dkmt_cpu_map_reset(&map, true);
   CHECK_EQ(map.map_class, AGX_D3DKMT_CPU_MAP_UNKNOWN);
   CHECK(map.conservative);
   const uint64_t reset_epoch = dirty.cpu_write_epoch;
   CHECK(agx_d3dkmt_cpu_map_request(
      &map, &dirty, bo_size, AGX_D3DKMT_CPU_MAP_TRACKED, true));
   CHECK_EQ(dirty.cpu_write_epoch, reset_epoch);
   CHECK(!map.conservative);
   CHECK(agx_d3dkmt_dirty_detach(&dirty, &snapshot));
   CHECK(snapshot.whole_bo);
   CHECK_EQ(agx_d3dkmt_dirty_commit(&dirty, &snapshot),
            AGX_D3DKMT_DIRTY_OK);
   CHECK(!agx_d3dkmt_dirty_has_pending(&dirty));

   /* Shared, imported, and exported BOs all pass tracked_allowed=false at the
    * winsys boundary. They fall back to RAW and remain whole-BO conservative.
    */
   for (unsigned exclusion = 0; exclusion < 3; exclusion++) {
      agx_d3dkmt_dirty_reset(&dirty, 920 + exclusion);
      agx_d3dkmt_cpu_map_reset(&map, false);
      CHECK(!agx_d3dkmt_cpu_map_request(
         &map, &dirty, bo_size, AGX_D3DKMT_CPU_MAP_TRACKED, false));
      CHECK_EQ(map.map_class, AGX_D3DKMT_CPU_MAP_RAW);
      CHECK(map.conservative);
      CHECK(agx_d3dkmt_dirty_detach(&dirty, &snapshot));
      CHECK(snapshot.whole_bo);
      CHECK_EQ(agx_d3dkmt_dirty_commit(&dirty, &snapshot),
               AGX_D3DKMT_DIRTY_OK);
      CHECK(!agx_d3dkmt_dirty_has_pending(&dirty));

      /* This is the build-submit rule: a still-live conservative pointer may
       * have written again without a callback, so republish the whole BO. */
      if (map.conservative)
         CHECK_EQ(agx_d3dkmt_dirty_mark_whole(&dirty, bo_size),
                  AGX_D3DKMT_DIRTY_OK);
      CHECK(agx_d3dkmt_dirty_detach(&dirty, &snapshot));
      CHECK(snapshot.whole_bo);
   }

   /* HOST_COHERENT/application maps explicitly request RAW even for private
    * BOs and therefore have the same conservative behavior. */
   agx_d3dkmt_dirty_reset(&dirty, 930);
   agx_d3dkmt_cpu_map_reset(&map, false);
   CHECK(!agx_d3dkmt_cpu_map_request(
      &map, &dirty, bo_size, AGX_D3DKMT_CPU_MAP_RAW, true));
   CHECK_EQ(map.map_class, AGX_D3DKMT_CPU_MAP_RAW);
   CHECK(map.conservative);
   CHECK(agx_d3dkmt_dirty_detach(&dirty, &snapshot));
   CHECK(snapshot.whole_bo);

   /* A raw pointer stays valid for the logical BO lifetime. A later internal
    * caller cannot revoke its unobservable write capability by requesting a
    * tracked view of the same mapping. */
   CHECK_EQ(agx_d3dkmt_dirty_commit(&dirty, &snapshot),
            AGX_D3DKMT_DIRTY_OK);
   CHECK(!agx_d3dkmt_cpu_map_request(
      &map, &dirty, bo_size, AGX_D3DKMT_CPU_MAP_TRACKED, true));
   CHECK_EQ(map.map_class, AGX_D3DKMT_CPU_MAP_RAW);
   CHECK(map.conservative);

   /* Unmap ends the raw pointer's lifetime but must publish stores made since
    * the last accepted submit before dropping conservative ownership. */
   CHECK_EQ(agx_d3dkmt_dirty_mark_whole(&dirty, bo_size),
            AGX_D3DKMT_DIRTY_OK);
   agx_d3dkmt_cpu_map_reset(&map, false);
   CHECK_EQ(map.map_class, AGX_D3DKMT_CPU_MAP_UNMAPPED);
   CHECK(!map.conservative);
   CHECK(agx_d3dkmt_dirty_detach(&dirty, &snapshot));
   CHECK(snapshot.whole_bo);
}

static void
test_invalidate_does_not_publish_a_flush(void)
{
   struct agx_d3dkmt_dirty_state state;
   struct agx_d3dkmt_dirty_snapshot snapshot;
   agx_d3dkmt_dirty_reset(&state, 700);

   /* GPU -> CPU invalidation does not call dirty_mark. The following detach
    * is the observable contract used by agx_bo_sync_for_cpu. */
   CHECK(!agx_d3dkmt_dirty_detach(&state, &snapshot));

   /* CPU -> GPU flush does publish the flushed interval. */
   CHECK_EQ(agx_d3dkmt_dirty_mark(&state, 0x10000, 0x4000, 16),
            AGX_D3DKMT_DIRTY_OK);
   CHECK(agx_d3dkmt_dirty_detach(&state, &snapshot));
   CHECK_EQ(snapshot.range_count, 1);
   CHECK_EQ(snapshot.ranges[0].offset, 0x4000);
}

static void
test_contract_blobs_we_emit(void)
{
   /* Every blob this port hands the kernel must pass the kernel's own
    * validator. Running the miniport's validators here -- rather than a
    * private copy of the rules -- is the point of agxw_contract.c being
    * WDK-free. */
   struct agxw_context_private ctx = {
      .version = AGXW_CONTEXT_PRIVATE_VERSION,
      .struct_size = sizeof(struct agxw_context_private),
      .priority = AGXW_PRIORITY_MEDIUM,
      .usc_exec_base = 0x100000000ull,
   };
   CHECK_EQ(agxw_context_private_validate(&ctx, sizeof(ctx), 0x100000000ull,
                                          0x100000000ull),
            0);

   struct {
      struct agxw_submit_private_v3 header;
      struct agxw_submit_read_range ranges[1];
   } submit = {
      .header = {
         .base = {
            .version = AGXW_SUBMIT_PRIVATE_VERSION_V3,
            .struct_size = sizeof(struct agxw_submit_private_v3),
            .flags = AGXW_SUBMIT_READ_SET_COMPLETE,
         },
         .read_range_count = 1,
      },
      .ranges = {{0x1500000000ull, 0x4000}},
   };
   CHECK_EQ(agxw_submit_private_validate(&submit.header.base,
                                          sizeof(submit)), 0);

   /* A non-zero in_sync_count is rejected: AppleAgxGpu's submit path refuses
    * it (submit.c:629-630) and this front end inherits the gap, so input
    * dependencies go through WaitForSynchronizationObjectFromGpu instead. */
   submit.header.base.in_sync_count = 1;
   CHECK(agxw_submit_private_validate(&submit.header.base,
                                      sizeof(submit)) != 0);

   struct {
      struct agxw_submit_private_v5 header;
      struct agxw_submit_read_range ranges[2];
   } submit_v5 = {
      .header = {
         .base = {
            .version = AGXW_SUBMIT_PRIVATE_VERSION_V5,
            .struct_size = sizeof(struct agxw_submit_private_v5),
            .flags = AGXW_SUBMIT_READ_SET_COMPLETE |
                     AGXW_SUBMIT_CLEAN_SET_COMPLETE,
         },
         .read_range_count = 1,
         .clean_range_count = 1,
      },
      .ranges = {
         {0x1500000000ull, 0x8000},
         {0x1500004000ull, 0x4000},
      },
   };
   CHECK_EQ(agxw_submit_private_validate(&submit_v5.header.base,
                                          sizeof(submit_v5)), 0);

   struct {
      struct agxw_escape_header hdr;
      uint8_t payload[AGXW_PARAMS_GLOBAL_SIZE];
   } esc = {
      .hdr =
         {
            .magic = AGXW_ESCAPE_MAGIC,
            .version = AGXW_ESCAPE_VERSION,
            .op = AGXW_ESC_GET_PARAMS,
            .payload_size = AGXW_PARAMS_GLOBAL_SIZE,
         },
   };
   uint32_t required = 0;
   CHECK_EQ(agxw_escape_validate(&esc.hdr, sizeof(esc), &required), 0);
   CHECK_EQ(required, AGXW_PARAMS_GLOBAL_SIZE);

   struct {
      struct agxw_escape_header hdr;
      uint8_t payload[AGXW_GET_TIME_SIZE];
   } t = {
      .hdr =
         {
            .magic = AGXW_ESCAPE_MAGIC,
            .version = AGXW_ESCAPE_VERSION,
            .op = AGXW_ESC_GET_TIME,
            .payload_size = AGXW_GET_TIME_SIZE,
         },
   };
   CHECK_EQ(agxw_escape_validate(&t.hdr, sizeof(t), &required), 0);
   CHECK_EQ(required, AGXW_GET_TIME_SIZE);
}

int
main(void)
{
   test_handle_pool_is_dense();
   test_cpu_visible_executable_flags();
   test_bind_plain();
   test_bind_unbind();
   test_bind_alignment_is_rejected();
   test_bind_single_page_is_one_op();
   test_bind_write_only_and_usc();
   test_production_bind_policy_guard();
   test_bind_rejects_kernel_carveout();
   test_va_split_carveout();
   test_bind_sizing_and_overflow();
   test_binary_sync_epoch();
   test_sync_current_takes_the_max();
   test_timeout_conversion();
   test_first_signaled();
   test_cpu_wait_probe();
   test_cpu_wait_probe_is_reentrant();
   test_identity_parsing();
   test_alloc_flags();
   test_read_range_canonicalization();
   test_read_range_compaction();
   test_dirty_interval_merge_and_overflow();
   test_dirty_detach_restore_and_stale_identity();
   test_dirty_repeated_unchanged_submit();
   test_dirty_cached_mapping_reuse();
   test_tracked_map_first_snapshot_accept_restore_and_exact_writes();
   test_tracked_map_cache_reuse_and_conservative_exclusions();
   test_invalidate_does_not_publish_a_flush();
   test_contract_blobs_we_emit();

   if (failures) {
      fprintf(stderr, "agx_d3dkmt_core: %d/%d checks FAILED\n", failures,
              checks);
      return 1;
   }

   printf("agx_d3dkmt_core: %d checks passed\n", checks);
   return 0;
}

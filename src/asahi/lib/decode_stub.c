/*
 * Copyright 2026 the Asahi Linux contributors
 * SPDX-License-Identifier: MIT
 */

/*
 * honeykrisp-h1-execution.md SS2.2 row D3: decode.c (libasahi_decode) is
 * POSIX-only (`#include <sys/mman.h>`, decode.c:17) -- AGX_DBG_TRACE's
 * command-stream disassembler is a debug/trace tool, not required for a
 * working driver. This stub provides exactly the six agxdecode_* symbols
 * still referenced, unconditionally, from Windows-compiled TUs
 * (agx_device.c:648,767 -- agxdecode_new_context/destroy_context;
 * agx_bo.c:343,397 and agx_device.c:385 -- agxdecode_track_alloc/free;
 * hk_queue.c:857,860 -- agxdecode_drm_cmdbuf/agxdecode_next_frame), so the
 * link succeeds; AGX_DBG_TRACE itself simply becomes a no-op on Windows
 * (dev->debug & AGX_DBG_TRACE gates every one of these call sites, so a
 * user who never sets ASAHI_MESA_DEBUG=trace never notices).
 *
 * Selected by src/asahi/lib/meson.build instead of decode.c when
 * host_machine.system() == 'windows' (Step 8/9 meson wiring); never built
 * on Linux, so decode.c's real implementation is unaffected there.
 */

#include <stdint.h>

#include "decode.h"

struct agxdecode_ctx {
   int unused;
};

static struct agxdecode_ctx stub_ctx;

struct agxdecode_ctx *
agxdecode_new_context(uint64_t shader_base)
{
   return &stub_ctx;
}

void
agxdecode_destroy_context(struct agxdecode_ctx *ctx)
{
}

void
agxdecode_next_frame(void)
{
}

void
agxdecode_track_alloc(struct agxdecode_ctx *ctx, struct agx_bo *alloc)
{
}

void
agxdecode_track_free(struct agxdecode_ctx *ctx, struct agx_bo *bo)
{
}

void
agxdecode_drm_cmdbuf(struct agxdecode_ctx *ctx,
                     struct drm_asahi_params_global *params,
                     struct util_dynarray *cmdbuf, bool verbose)
{
}

/*
 * Copyright 2026 the Asahi Linux contributors
 * SPDX-License-Identifier: MIT
 */
#pragma once

/*
 * honeykrisp-h1-execution.md SS1, SS5.4. Windows winsys backend for
 * agx_device_ops_t -- the counterpart of agx_device_virtio.h, but talking
 * DeviceIoControl to the AGX Windows KMD instead of vdrm to a virtio-gpu
 * host, over the ioctl surface pinned in kmd-uapi-core/kmd_uapi.h.
 *
 * NOT build-verified: this header (and its .c) can only be *compiled* with a
 * real llvm-mingw or MSVC toolchain (aarch64-w64-mingw32-clang or cl.exe),
 * neither of which is installed on this host. See the H1 patch series
 * verification notes for exactly what WAS checked (syntax-only, narrowed
 * shape checks) on this macOS/no-mingw host.
 */

#include <stdbool.h>
#include <stdint.h>

#include "agx_device.h"

/* Forward decl only (pointer use below) -- avoids pulling
 * "util/u_sync_provider.h" into every agx_device_win.h consumer just for
 * util_sync_provider_ntagx()'s return type. */
struct util_sync_provider;

/*
 * Install the Windows ops table on `dev` and stash the winsys device handle
 * (an NT HANDLE from CreateFileW, opaque here as `void *` so this header
 * stays includable without <windows.h>) in dev->hdev. Mirrors
 * agx_virtio_open_device()'s contract exactly: the caller runs
 * agx_open_device(memctx, dev) next to do the rest of the (backend-agnostic)
 * device bring-up (GET_PARAMS, VM_CREATE, zero/scratch/printf BOs, ...).
 *
 * KNOWN GAP (flagged, honeykrisp-h1-execution.md SS2.1 row ":511-535"):
 * agx_open_device() itself still unconditionally does
 * `dev->ops = agx_device_drm_ops;` near its top and then a drmGetVersion()
 * probe that overwrites dev->ops again -- both HAVE_LIBDRM/Linux-only code
 * that has not yet been guarded behind `if (dev->ops.get_params == NULL)`
 * (that restructuring, plus the HAVE_LIBDRM guards making drmGetVersion()
 * itself compile out on Windows, is scoped to a later header-hygiene step,
 * not part of this file). Until that lands, a caller of
 * agx_win_open_device() followed by agx_open_device() would have its preset
 * dev->ops clobbered on a build where agx_device.c's Linux paths are also
 * compiled in -- not a concern yet since this file is not wired into any
 * meson target (Step 8), but recorded here so Steps 7-9 don't rediscover it
 * from scratch.
 */
bool agx_win_open_device(struct agx_device *dev, void *hdev);

/*
 * honeykrisp-h1-execution.md SS5.4: DeviceIoControl transport shared by
 * every op body in agx_device_win.c and by agx_sync_win.c's
 * util_sync_provider_ntagx (kept non-static here, unlike the plan's
 * illustrative skeleton, precisely so the sync provider -- "keep with the
 * winsys so it can use ntagx_ioctl", plan SS3.1 -- doesn't need its own
 * copy).
 *
 * `drm_cmd` is one of the DRM_IOCTL_ASAHI_* pinned literals (PART 1) or one
 * of kmd_uapi.h's NTASI_KMD_UAPI_IOCTL_* Windows-extension literals
 * (PART 2, syncobj family + GEM_CLOSE/GEM_MMAP/GEM_MUNMAP) -- anything
 * ntasi_kmd_uapi_cmd_to_ctl() recognizes. `req` is the same wire struct
 * pointer every agx_device_ops_t body already builds for the Linux/virtio
 * backends (struct drm_asahi_* or struct ntasi_kmd_uapi_* as appropriate);
 * its `in_size`/`out_size` are derived generically from `drm_cmd`'s own
 * dir+size bits, so callers never pass sizes explicitly.
 *
 * Returns 0 on success; on failure returns -1 (unrecognized drm_cmd) or
 * -1 with `errno`/GetLastError() context left for the caller to log, same
 * calling convention as drmIoctl().
 */
int ntagx_ioctl(void *hdev, uint32_t drm_cmd, void *req);

/*
 * NTAGX_FENCE_LOG=1 -- the completion-path counter readout.
 *
 * WHY IT IS ITS OWN SWITCH, AND NOT NTAGX_BO_LOG. The measurement is fence
 * LATENCY, and NTAGX_BO_LOG's submit verifier polls the KMD with Sleep(2) for
 * up to 250 ms inside vkQueueSubmit. Sharing a switch would mean the only way
 * to read the counters is with a perturbing sleep on the path whose timing is
 * the evidence. Read straight from getenv() rather than through dev->debug,
 * for the reason this series has already paid for once: when the symptom is
 * "nothing happened", a flag that was never parsed and a thing that never
 * happened look identical.
 */
#if AGX_GPU_DIAG >= 2
bool ntagx_fence_log_enabled(void);
#else
#define ntagx_fence_log_enabled() false
#endif

/*
 * Sample the KMD's cumulative completion-path counters and print them with
 * the delta since the previous sample, labelled with `when`.
 *
 * CALL IT AFTER THE THING YOU WANT TO MEASURE, NEVER BEFORE. The counters are
 * cumulative, so a sample taken when the submit ioctl returns describes the
 * PREVIOUS submission's completion, not this one's -- submission is
 * asynchronous and the fence has not been signalled yet. The pair of samples
 * that answers the question is "after submit" and "after fence wait"; the
 * delta between them is what ran while the fence was outstanding.
 *
 * Read-only in the kernel and free of side effects here beyond stderr.
 */
#if AGX_GPU_DIAG >= 2
void ntagx_completion_counters_report(void *hdev, const char *when);
#else
#define ntagx_completion_counters_report(...) ((void)0)
#endif

/*
 * honeykrisp-h1-execution.md SS3.1: the Windows util_sync_provider
 * (agx_sync_win.c), the counterpart of util_sync_provider_drm
 * (src/util/u_sync_provider.h). Declared here rather than in the shared,
 * driver-agnostic u_sync_provider.h -- unlike util_sync_provider_drm, this
 * constructor is asahi/Windows-specific (it talks kmd-uapi-core's ioctl
 * surface, not generic libdrm), so it belongs with the winsys it is part
 * of, matching where the plan keeps the .c file itself ("keep with the
 * winsys so it can use ntagx_ioctl", plan SS3.1).
 */
struct util_sync_provider *util_sync_provider_ntagx(void *hdev);

/*
 * Copyright 2024 Valve Corporation
 * Copyright 2024 Alyssa Rosenzweig
 * Copyright 2022-2023 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "asahi/lib/agx_device.h"
#include <sys/types.h>
#include "asahi/lib/agx_d3dkmt.h"
#include "hk_private.h"
#include "vk_physical_device.h"
#include "vk_sync.h"
#include "wsi_common.h"

struct hk_instance;
struct hk_physical_device;

struct hk_queue_family {
   VkQueueFlags queue_flags;
   uint32_t queue_count;
};

struct hk_memory_heap {
   uint64_t size;
   uint64_t used;
   VkMemoryHeapFlags flags;
};

struct hk_physical_device {
   struct vk_physical_device vk;
   dev_t render_dev; /* unused on Windows -- see device_path below */
   int master_fd;

   /*
    * honeykrisp-h1-execution.md SS2.2 row P1: the KMD device-interface
    * path this physical device was opened from (CreateFileA target),
    * stashed so hk_device.c's per-logical-device re-open (SS2.2 row P4)
    * doesn't need a DRM render-node round trip to rediscover it. Sized
    * like Win32 MAX_PATH without pulling in <windows.h> here.
    */
   char device_path[260];

   /*
    * The D3DKMT adapter this physical device was opened from, when it was
    * opened that way (d3dkmt == true). This is the identity Vulkan is
    * supposed to expose as VkPhysicalDeviceIDProperties::deviceLUID and the
    * one a D3D application matches an IDXGIAdapter against -- which a raw NT
    * file handle on a private ioctl node could never provide.
    */
   bool d3dkmt;
   struct agx_kmt_luid adapter_luid;

   /* Only used for VK_EXT_memory_budget */
   struct agx_device dev;

   struct wsi_device wsi_device;

   uint8_t device_uuid[VK_UUID_SIZE];

   // TODO: add mapable VRAM heap if possible
   struct hk_memory_heap mem_heaps[3];
   VkMemoryType mem_types[3];
   uint8_t mem_heap_count;
   uint8_t mem_type_count;
   uint64_t sysmem;

   struct hk_queue_family queue_families[3];
   uint8_t queue_family_count;

   struct vk_sync_type syncobj_sync_type;
   const struct vk_sync_type *sync_types[2];

   simple_mtx_t debug_compile_lock;
};

VK_DEFINE_HANDLE_CASTS(hk_physical_device, vk.base, VkPhysicalDevice,
                       VK_OBJECT_TYPE_PHYSICAL_DEVICE)

static inline struct hk_instance *
hk_physical_device_instance(struct hk_physical_device *pdev)
{
   return (struct hk_instance *)pdev->vk.instance;
}

VkResult hk_create_drm_physical_device(struct vk_instance *vk_instance,
                                       struct _drmDevice *drm_device,
                                       struct vk_physical_device **pdev_out);

#ifdef _WIN32
/* honeykrisp-h1-execution.md SS2.2 row P1 (hk_physical_device.c) / SS4 item
 * 4 (hk_physical_device_win.c). */
/*
 * Exactly one of `path` and `luid` must be non-NULL: `path` opens the legacy
 * private \\.\AGXKMD ioctl node, `luid` opens a WDDM adapter through
 * D3DKMT. The legacy path is retained only because patch 0009's user-mode
 * shim has no WDDM miniport to talk to.
 */
VkResult hk_create_win_physical_device(struct vk_instance *vk_instance,
                                       const char *path,
                                       const struct agx_kmt_luid *luid,
                                       struct vk_physical_device **pdev_out);
VkResult hk_enumerate_physical_devices_win(struct vk_instance *vk_instance);
#endif

void hk_physical_device_destroy(struct vk_physical_device *vk_device);

/*
 * honeykrisp-h1-execution.md SS4 item 1: extend the WSI-platform guard with
 * VK_USE_PLATFORM_WIN32_KHR -- turns on KHR_swapchain/present extensions
 * (hk_physical_device.c, all #ifdef HK_USE_WSI_PLATFORM) and the instance
 * surface extensions (hk_instance.c) for the GDI/CPU present path
 * (wsi_common_win32.cpp, upstream-shipped already).
 */
#if defined(VK_USE_PLATFORM_WAYLAND_KHR) ||                                    \
   defined(VK_USE_PLATFORM_XCB_KHR) || defined(VK_USE_PLATFORM_XLIB_KHR) ||    \
   defined(VK_USE_PLATFORM_DISPLAY_KHR) || defined(VK_USE_PLATFORM_WIN32_KHR)
#define HK_USE_WSI_PLATFORM
#endif

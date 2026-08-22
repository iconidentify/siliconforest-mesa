/*
 * Copyright 2024 Valve Corporation
 * Copyright 2024 Alyssa Rosenzweig
 * Copyright 2022-2023 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */
#include "hk_wsi.h"
#include "hk_instance.h"
#include "wsi_common.h"

#ifndef _WIN32
#include <xf86drm.h>
#endif

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
hk_wsi_proc_addr(VkPhysicalDevice physicalDevice, const char *pName)
{
   VK_FROM_HANDLE(hk_physical_device, pdev, physicalDevice);
   return vk_instance_get_proc_addr_unchecked(pdev->vk.instance, pName);
}

/*
 * honeykrisp-h1-execution.md SS4 item 3 / SS2.2 row P6: drmGetDevice2-based
 * on-device presentation probe, Linux-only. Not compiled on Windows --
 * hk_init_wsi() below leaves wsi_device.can_present_on_device unset there
 * instead (supports_modifiers also goes false, SS4 item 3: the GDI/CPU
 * present path advertises no modifiers).
 */
#ifndef _WIN32
static bool
hk_wsi_can_present_on_device(VkPhysicalDevice physicalDevice, int fd)
{
   VK_FROM_HANDLE(hk_physical_device, pdev, physicalDevice);
   if (pdev->dev.is_virtio) {
      return false;
   }

   drmDevicePtr device;
   if (drmGetDevice2(fd, 0, &device) != 0) {
      return false;
   }

   /* Allow on-device presentation for all non-virtio devices with bus type
    * PLATFORM */
   bool match = device->bustype == DRM_BUS_PLATFORM;

   drmFreeDevice(&device);

   return match;
}
#endif

VkResult
hk_init_wsi(struct hk_physical_device *pdev)
{
   VkResult result;

#ifdef _WIN32
   /* Honeykrisp has no D3D12 resource/queue interop.  Use Mesa's Win32
    * GDI copy-present backend, which is also valid in headless sessions. */
   struct wsi_device_options wsi_options = {.sw_device = true};
#else
   struct wsi_device_options wsi_options = {.sw_device = false};
#endif
   result = wsi_device_init(
      &pdev->wsi_device, hk_physical_device_to_handle(pdev), hk_wsi_proc_addr,
      &pdev->vk.instance->alloc, pdev->master_fd,
      &hk_physical_device_instance(pdev)->drirc.options, &wsi_options);
   if (result != VK_SUCCESS)
      return result;

   pdev->wsi_device.supports_scanout = false;
#ifdef _WIN32
   /* honeykrisp-h1-execution.md SS4 item 3: the GDI/CPU present path
    * (wsi_common_win32.cpp) advertises no modifiers, and has no drm-node
    * on-device probe to hook. */
   pdev->wsi_device.supports_modifiers = false;
#else
   pdev->wsi_device.supports_modifiers = true;
   pdev->wsi_device.can_present_on_device = hk_wsi_can_present_on_device;
#endif

   pdev->vk.wsi_device = &pdev->wsi_device;

   return result;
}

void
hk_finish_wsi(struct hk_physical_device *pdev)
{
   pdev->vk.wsi_device = NULL;
   wsi_device_finish(&pdev->wsi_device, &pdev->vk.instance->alloc);
}

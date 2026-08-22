/*
 * Copyright 2026 the Asahi Linux contributors
 * SPDX-License-Identifier: MIT
 */

/*
 * honeykrisp-h1-execution.md SS4 item 4 / SS5.2 item 2: the Windows
 * `enumerate` callback (vk_instance.c's enumerate_physical_devices_locked
 * consults instance->physical_devices.enumerate before its drm-node walk,
 * VERIFIED vk_instance.c:445-457 -- hk_instance.c installs this instead of
 * try_create_for_drm on Windows, SS4 item 4). Kept as its own TU (rather
 * than folded into hk_physical_device.c) because it only needs
 * hk_create_win_physical_device (declared in hk_physical_device.h), not any
 * of that file's static helpers.
 *
 * Enumeration is now WDDM-shaped: D3DKMTEnumAdapters2 lists every adapter in
 * the system and each one's KMTQAITYPE_UMDRIVERPRIVATE blob says whether it
 * is an AGX GPU, so multi-adapter works for free and a foreign adapter is
 * skipped rather than mistakenly claimed. That also replaces the
 * GUID_DEVINTERFACE_AGXKMD / CM_Get_Device_Interface_List walk this file used
 * to describe as future work -- there is no device interface to walk when the
 * driver is a WDDM miniport.
 *
 * NTAGX_DEVICE_PATH still selects the legacy private-ioctl backend, because
 * patch 0009's user-mode shim is a \\.\pipe-style endpoint with no WDDM
 * miniport behind it. Setting it opts out of D3DKMT entirely; leaving it
 * unset is the production path.
 */

#include "hk_instance.h"
#include "hk_physical_device.h"

#include "asahi/lib/agx_device_d3dkmt.h"

#include "util/list.h"
#include "util/macros.h"
#include "util/os_misc.h"
#include "util/u_math.h"

/* Enough for any Mac; the loop below simply ignores anything past it. */
#define HK_MAX_D3DKMT_ADAPTERS 8

VkResult
hk_enumerate_physical_devices_win(struct vk_instance *_instance)
{
   struct hk_instance *instance = (struct hk_instance *)_instance;
   const char *path = os_get_option("NTAGX_DEVICE_PATH");
   struct vk_physical_device *pdevice;
   VkResult result;

   if (path != NULL) {
      /*
       * Legacy private-ioctl backend, for patch 0009's user-mode shim only,
       * and only on an explicit opt-in -- there is deliberately no silent
       * fallback to it. An empty NTAGX_DEVICE_PATH selects the KMD's fixed
       * symbolic link; any other value is used verbatim.
       */
      if (path[0] == '\0')
         path = "\\\\.\\AGXKMD";

      result = hk_create_win_physical_device(_instance, path, NULL, &pdevice);
      if (result == VK_ERROR_INCOMPATIBLE_DRIVER)
         return VK_SUCCESS;
      if (result != VK_SUCCESS)
         return result;

      list_addtail(&pdevice->link, &instance->vk.physical_devices.list);
      return VK_SUCCESS;
   }

   struct agx_kmt_luid luids[HK_MAX_D3DKMT_ADAPTERS];
   uint32_t count = 0;

   if (!agx_d3dkmt_enumerate_adapters(luids, ARRAY_SIZE(luids), &count))
      return VK_SUCCESS; /* no D3DKMT at all: no devices, not an error */

   count = MIN2(count, (uint32_t)ARRAY_SIZE(luids));

   for (uint32_t i = 0; i < count; ++i) {
      result = hk_create_win_physical_device(_instance, NULL, &luids[i],
                                             &pdevice);
      if (result == VK_ERROR_INCOMPATIBLE_DRIVER)
         continue;
      if (result != VK_SUCCESS)
         return result;

      list_addtail(&pdevice->link, &instance->vk.physical_devices.list);
   }

   return VK_SUCCESS;
}

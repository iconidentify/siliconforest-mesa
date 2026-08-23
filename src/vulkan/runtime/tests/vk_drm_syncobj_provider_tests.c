/*
 * Copyright 2026 the Asahi Linux contributors
 * SPDX-License-Identifier: MIT
 */

#include "vk_drm_syncobj.h"

#include "util/u_sync_provider.h"

#include <stdbool.h>
#include <stdio.h>

static int
fake_timeline_wait(struct util_sync_provider *provider, uint32_t *handles,
                   uint64_t *points, unsigned handle_count,
                   int64_t timeout_ns, unsigned flags,
                   uint32_t *first_signaled)
{
   (void)provider;
   (void)handles;
   (void)points;
   (void)handle_count;
   (void)timeout_ns;
   (void)flags;
   (void)first_signaled;
   return 0;
}

static bool
has_feature(enum vk_sync_features features, enum vk_sync_features feature)
{
   return (features & feature) != 0;
}

static bool
check_type(enum util_sync_provider_features provider_features,
           bool expose_timeline,
           bool expect_wait_before_signal)
{
   struct util_sync_provider provider = {
      .features = provider_features,
      .timeline_wait = expose_timeline ? fake_timeline_wait : NULL,
   };

   const enum vk_sync_features features =
      vk_drm_syncobj_provider_features(&provider);
   const enum vk_sync_features required_base =
      VK_SYNC_FEATURE_BINARY |
      VK_SYNC_FEATURE_GPU_WAIT |
      VK_SYNC_FEATURE_GPU_MULTI_WAIT |
      VK_SYNC_FEATURE_CPU_RESET |
      VK_SYNC_FEATURE_CPU_SIGNAL |
      VK_SYNC_FEATURE_WAIT_PENDING;
   const bool has_timeline =
      has_feature(features, VK_SYNC_FEATURE_TIMELINE);
   const bool has_wait_before_signal =
      has_feature(features, VK_SYNC_FEATURE_WAIT_BEFORE_SIGNAL);

   return (features & required_base) == required_base &&
          has_timeline == expose_timeline &&
          has_wait_before_signal == expect_wait_before_signal;
}

int
main(void)
{
   if (!check_type(0, false, false)) {
      fprintf(stderr, "binary provider unexpectedly exposed a timeline\n");
      return 1;
   }

   if (!check_type(0, true, false)) {
      fprintf(stderr, "provider without capability changed timeline mode\n");
      return 1;
   }

   if (!check_type(UTIL_SYNC_PROVIDER_WAIT_BEFORE_SIGNAL, true, true)) {
      fprintf(stderr, "wait-before-signal capability was not propagated\n");
      return 1;
   }

   return 0;
}

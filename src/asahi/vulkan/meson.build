# Copyright © 2022 Collabora Ltd. and Red Hat Inc.
# SPDX-License-Identifier: MIT
hk_files = files(
  'hk_buffer.c',
  'hk_buffer.h',
  'hk_buffer_view.c',
  'hk_buffer_view.h',
  'hk_cmd_buffer.c',
  'hk_cmd_buffer.h',
  'hk_cmd_clear.c',
  'hk_cmd_dispatch.c',
  'hk_cmd_draw.c',
  'hk_cmd_meta.c',
  'hk_cmd_pool.c',
  'hk_cmd_pool.h',
  'hk_descriptor_set.h',
  'hk_descriptor_set.c',
  'hk_descriptor_set_layout.c',
  'hk_descriptor_set_layout.h',
  'hk_descriptor_table.c',
  'hk_descriptor_table.h',
  'hk_device.c',
  'hk_device.h',
  'hk_device_memory.c',
  'hk_device_memory.h',
  'hk_event.c',
  'hk_event.h',
  'hk_format.c',
  'hk_image.c',
  'hk_image.h',
  'hk_image_view.c',
  'hk_image_view.h',
  'hk_instance.c',
  'hk_instance.h',
  'hk_nir_lower_descriptors.c',
  'hk_physical_device.c',
  'hk_physical_device.h',
  'hk_private.h',
  'hk_query_pool.c',
  'hk_query_pool.h',
  'hk_queue.c',
  'hk_queue.h',
  'hk_sampler.c',
  'hk_sampler.h',
  'hk_shader.c',
  'hk_shader.h',
  'hk_wsi.c',
  'hk_wsi.h'
)

# honeykrisp-h1-execution.md SS5.2 item 2: hk_physical_device_win.c
# (SS4 item 4) is the Windows `enumerate` callback -- only reachable/needed
# there, the Linux drm-node walk (hk_create_drm_physical_device,
# hk_physical_device.c) has no counterpart file.
if host_machine.system() == 'windows'
  hk_files += files('hk_physical_device_win.c')
endif

hk_drirc = custom_target(
  'hk_drirc',
  input  : [files('hk_drirc_gen.py'), drirc_gen, '00-hk-defaults.conf'],
  output : ['hk_drirc.c', 'hk_drirc.h'],
  command : [
    prog_python, '@INPUT0@',
    '--import-path', join_paths(dir_source_root, 'src/util'),
    '--drirc-src', '@OUTPUT0@',
    '--drirc-hdr', '@OUTPUT1@',
    '--validate', '@INPUT2@',
  ],
)

hk_entrypoints = custom_target(
  input : [vk_entrypoints_gen, vk_api_xml],
  output : ['hk_entrypoints.h', 'hk_entrypoints.c'],
  command : [
    prog_python, '@INPUT0@', '--xml', '@INPUT1@', '--proto', '--weak',
    '--out-h', '@OUTPUT0@', '--out-c', '@OUTPUT1@', '--prefix', 'hk',
    '--beta', with_vulkan_beta.to_string(),
  ],
  depend_files : vk_entrypoints_gen_depend_files,
)

# honeykrisp-h1-execution.md SS5.2 item 2: dep_libdrm is null_dep on
# Windows anyway (root meson.build: system_has_kms_drm is false there, so
# dep_libdrm is never even a real dependency() call) -- only add it when
# found, matching the "upstream-shaped, not forked" argument (SS7) for why
# this edit is small enough to plausibly upstream.
hk_deps = [
  idep_nir,
  idep_vulkan_runtime,
  idep_vulkan_util,
  idep_vulkan_wsi,
  idep_vulkan_wsi_headers,
  idep_agx_pack,
  idep_libagx,
  idep_libagx_shaders_h,
]
if dep_libdrm.found()
  hk_deps += dep_libdrm
endif

# honeykrisp-h1-execution.md SS5.2 item 2: no virtio-gpu include dirs on
# Windows (same rationale as libasahi_lib, src/asahi/lib/meson.build); the
# overlay drm-uapi shim + kmd-uapi-core/kmd-syncobj-core headers hk_queue.c
# (SS3.2 -- "drm-uapi/drm.h" for DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT) and
# hk_physical_device_win.c/hk_device.c need are supplied via -Dc_args at
# `meson setup`, same as src/asahi/lib/meson.build's libasahi_lib.
libhk = static_library(
  'hk',
  [
    hk_entrypoints,
    hk_drirc,
    hk_files,
    sha1_h,
  ],
  include_directories : host_machine.system() == 'windows'
    ? [inc_gallium, inc_gallium_aux, inc_include, inc_src, inc_asahi]
    : [
        inc_gallium,
        inc_gallium_aux,
        inc_include,
        inc_src,
        inc_asahi,
        inc_virtio_gpu,
        inc_virtio_vdrm,
      ],
  link_with : [libasahi_lib, libasahi_layout, libasahi_compiler],
  c_args : ['-Wno-c2x-extensions', hk_win_c_args],
  dependencies : [hk_deps],
  gnu_symbol_visibility : 'hidden',
)

libvulkan_asahi = shared_library(
  'vulkan_asahi',
  link_whole : [libhk],
  link_args: [ld_args_build_id],
  gnu_symbol_visibility : 'hidden',
  # honeykrisp Windows port: the driver is built with hidden visibility, so the
  # loader-facing ICD entry points (vk_icdGetInstanceProcAddr,
  # vk_icdNegotiateLoaderICDInterfaceVersion) are NOT auto-exported by lld on
  # PE targets -- verified: the .dll linked with 381 exports, all of them
  # SPIRV-Tools C++ symbols, and neither ICD symbol present. Mesa's other
  # Windows-capable Vulkan drivers (dzn src/microsoft/vulkan/meson.build:78,
  # radv, lavapipe) all pass the generated vulkan_api.def here; do the same.
  vs_module_defs : vulkan_api_def,
  # Windows DLLs carry no 'lib' prefix. meson's mingw default produced
  # libvulkan_asahi.dll while the generated ICD manifest points at
  # vulkan_asahi.dll (icd_file_name uses libname_prefix, empty on Windows) --
  # the loader would never find the driver. VERIFIED by auditing the built
  # artifact against asahi_icd.aarch64.json.
  name_prefix : '',
  install : true,
)

icd_file_name = libname_prefix + 'vulkan_asahi.' + libname_suffix

asahi_icd = custom_target(
  input : [vk_icd_gen, vk_api_xml],
  output : 'asahi_icd.' + vulkan_manifest_suffix,
  command : [
    prog_python, '@INPUT0@',
    '--api-version', '1.4', '--xml', '@INPUT1@',
    '--sizeof-pointer', sizeof_pointer,
    '--icd-lib-path', vulkan_icd_lib_path,
    '--icd-filename', icd_file_name,
    '--out', '@OUTPUT@',
  ],
  build_by_default : true,
  install_dir : with_vulkan_icd_dir,
  install_tag : 'runtime',
  install : true,
)

_dev_icdname = 'asahi_devenv_icd.@0@.json'.format(host_machine.cpu())
_dev_icd = custom_target(
  input : [vk_icd_gen, vk_api_xml],
  output : _dev_icdname,
  command : [
    prog_python, '@INPUT0@',
    '--api-version', '1.4', '--xml', '@INPUT1@',
    '--sizeof-pointer', sizeof_pointer,
    '--icd-lib-path', meson.current_build_dir(),
    '--icd-filename', icd_file_name,
    '--out', '@OUTPUT@',
  ],
  build_by_default : true,
)

devenv.append('VK_DRIVER_FILES', _dev_icd.full_path())

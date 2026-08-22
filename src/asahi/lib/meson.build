# Copyright 2018 Rob Clark
# Copyright 2019 Collabora, Ltd.
# Copyright 2017 Intel Corporation
# SPDX-License-Identifier: MIT

libasahi_lib_files = files(
  'agx_bo.c',
  'agx_border.c',
  'agx_chip.c',
  'agx_device.c',
  'agx_linker.c',
  'agx_bg_eot.c',
  'agx_tilebuffer.c',
  'agx_nir_lower_msaa.c',
  'agx_nir_lower_sample_intrinsics.c',
  'agx_nir_lower_tilebuffer.c',
  'agx_nir_lower_uvs.c',
  'agx_nir_lower_vbo.c',
  'agx_nir_prolog_epilog.c',
  'agx_ppp.h',
  'agx_scratch.c',
  'agx_va.c',
  'pool.c',
)

# honeykrisp-h1-execution.md SS5.2 item 1: the winsys backend is
# OS-specific -- agx_device_virtio.c is Linux/virtio-gpu-only (needs
# libdrm's vdrm.h, HAVE_LIBDRM always set when it's reachable, see root
# meson.build's system_has_kms_drm gating of dep_libdrm); the Windows
# equivalent (agx_device_win.c, SS1) talks DeviceIoControl instead. Also
# swap decode.c (POSIX-only, <sys/mman.h>) for decode_stub.c (SS2.2 row D3)
# and add the ntagx sync provider (SS3.1) on Windows.
if host_machine.system() == 'windows'
  libasahi_lib_files += files(
    'agx_device_win.c',
    'agx_device_win.h',
    'decode_stub.c',
    'agx_sync_win.c',
  )
  # The D3DKMT winsys: the same agx_device_ops_t surface, but through
  # dxgkrnl and a WDDM miniport instead of a private ioctl node, so that
  # Vulkan and D3D share one allocator, one GPU VA regime and one handle
  # namespace. Built alongside the AGXKMD backend rather than replacing it:
  # the two are selected at runtime by hk_enumerate_physical_devices_win()
  # (NTAGX_DEVICE_PATH forces the legacy path, which patch 0009's user-mode
  # shim still needs), so this patch adds a capability instead of removing
  # one while the miniport does not yet exist.
  #
  # agx_d3dkmt_core.c has no Mesa, Windows or generated-header dependency on
  # purpose -- gpu/honeykrisp-port/tests/run-host-tests.sh compiles and runs
  # it natively. Nothing else in this list can be tested off-target.
  libasahi_lib_files += files(
    'agx_d3dkmt.c',
    'agx_d3dkmt.h',
    'agx_d3dkmt_core.c',
    'agx_d3dkmt_core.h',
    'agx_d3dkmt_priv.h',
    'agx_device_d3dkmt.c',
    'agx_device_d3dkmt.h',
    'agx_sync_d3dkmt.c',
    'agx_d3dkmt_bridge.c',
    'agx_d3dkmt_bridge.h',
    'agxw_bridge_ddi.h',
    'agxw_bridge_core.c',
    'agxw_bridge_core.h',
    'agx_bridge_vma.c',
    'agx_bridge_vma.h',
    'agx_bridge_vma_core.c',
    'agx_bridge_vma_core.h',
  )
  # BUILD-STATUS.md stage-2: agx_device_win.c calls ntasi_kmd_uapi_cmd_to_ctl
  # (kmd-uapi-core/kmd_uapi.c); until now that tree was only ever put on the
  # include path (-Dc_args' -I, for kmd_uapi.h), never actually compiled in
  # -- link failed with "undefined symbol: ntasi_kmd_uapi_cmd_to_ctl".
  # kmd-uapi-core/kmd-syncobj-core live outside this Mesa tree entirely (in
  # the windows-on-apple-silicon repo, passed in via -Dc_args at `meson
  # setup`, same as the honeykrisp-port/overlay directory) with no fixed
  # relative path from here, so this can't be a plain files('../../relative')
  # reference; meson's `files()` does accept an absolute path string (it
  # only special-cases and resolves *relative* strings against
  # current_source_dir(), an absolute string passes through unchanged) --
  # VERIFIED by this successfully compiling. kmd_syncobj.c (kmd-syncobj-core)
  # is NOT added here: nothing under src/asahi currently calls a
  # kmd_syncobj_* symbol (grep, stage-2), only kmd_uapi.h types/macros are
  # used so far by agx_sync_win.c.
  kmd_uapi_core_dir = meson.get_external_property('ntagx_kmd_uapi_core')
  libasahi_lib_files += files(kmd_uapi_core_dir / 'kmd_uapi.c')

  # The WDDM miniport's kernel<->userspace contract
  # (drivers/AppleAgxWddm/core). agxw_contract.h is INCLUDED by the D3DKMT
  # winsys rather than transcribed, and agxw_contract.c is COMPILED IN so the
  # UMD runs the same validators the kernel runs -- a rejection here means
  # exactly what a rejection in the miniport would mean. That is only possible
  # because that core is deliberately WDK-free.
  #
  # Located from its own cross-file property when the build sets one, else
  # derived from the AppleAgxGpu core path, which is a fixed sibling in the
  # same repository. Deriving keeps this patch applicable without editing
  # honeykrisp-port/cross-*.txt; setting the property explicitly is the
  # tidier long-term form.
  agxw_contract_dir = meson.get_external_property(
    'ntagx_wddm_contract_core',
    kmd_uapi_core_dir / '..' / '..' / '..' / 'AppleAgxWddm' / 'core')
  libasahi_lib_files += files(agxw_contract_dir / 'agxw_contract.c')
  libasahi_lib_inc_win = include_directories(agxw_contract_dir)
else
  libasahi_lib_files += files('agx_device_virtio.c')
  libasahi_lib_inc_win = []
endif

# honeykrisp-h1-execution.md SS2.2 row P9: "is virtio available" define,
# guards the two dev->dev.is_virtio blocks in hk_queue.c and the mirror
# bookkeeping in hk_device_memory.c that name `struct asahi_ccmd_submit_res`
# by value (needs the complete type, which agx_device.h only
# forward-declares when !HAVE_LIBDRM, SS2.1 row D2). Not consumed in this
# file (agx_device_virtio.c's own file-list gating just above doesn't need
# it) -- exported here as a variable so src/asahi/vulkan/meson.build (where
# hk_queue.c/hk_device_memory.c actually compile, Step 8/9) can add it to
# libhk's c_args without duplicating the host_machine.system() check.
# add_project_arguments() has already run by the time subdir('src') gets
# here (root meson.build), so appending to the global pre_args list from a
# subdir meson.build would silently do nothing -- pass it as per-target
# c_args instead.
hk_win_c_args = host_machine.system() == 'windows' ? [] : ['-DHAVE_VDRM']

libasahi_decode_files = files(
  'decode.c',
)

libasahi_decode = static_library(
  'asahi_decode',
  [libasahi_decode_files, agx_pack],
  dependencies : [dep_valgrind, idep_mesautil],
  c_args : [no_override_init_args, '-Wno-c2x-extensions'],
  gnu_symbol_visibility : 'hidden',
  link_with : [libagx2_disasm],
  build_by_default : false,
)

libasahi_decode_shared = shared_library(
  'asahi_decode',
  [libasahi_decode_files, agx_pack],
  dependencies : [dep_valgrind, idep_mesautil],
  c_args : [no_override_init_args, '-Wno-c2x-extensions'],
  build_by_default : with_tools.contains('asahi'),
  link_with : [libagx2_disasm],
)

libagx_shaders = custom_target(
  input : libagx_spv,
  output : ['libagx_shaders.h', 'libagx_shaders.c'],
  command : [prog_asahi_clc, libagx_spv, '@OUTPUT0@', '@OUTPUT1@'],
)

idep_libagx_shaders_h = declare_dependency(
  sources : [libagx_shaders],
  include_directories : include_directories('.'),
)

# honeykrisp-h1-execution.md SS5.2 item 1: on Windows this target has no
# virtio-gpu (inc_virtio_gpu/inc_virtio_vdrm), no libvdrm to link (its
# sources are libdrm-only and never even attempted -- see the top of this
# file), and decode_stub.c (already in libasahi_lib_files above) replaces
# libasahi_decode entirely rather than linking it. The overlay drm-uapi
# shim + kmd-uapi-core/kmd-syncobj-core headers this target's Windows files
# need (agx_device_win.c's "drm-uapi/asahi_drm.h" + "kmd_uapi.h",
# decode_stub.c/agx_sync_win.c) are supplied via -Dc_args at `meson setup`
# (-I ordering: overlay dir first, then Mesa's include/, then
# kmd-uapi-core/kmd-syncobj-core) rather than baked in here, since those
# paths live outside the Mesa tree (windows-on-apple-silicon/) at a
# location relative to wherever this tree was checked out, not to this
# meson.build.
# honeykrisp-h1-execution.md SS5.2: `not with_vdrm` (not just
# `host_machine.system() == 'windows'`) guards the inc_virtio_gpu/
# inc_virtio_vdrm/libvdrm references below -- inc_virtio_vdrm is only
# DECLARED at all when with_vdrm triggers `subdir('virtio/vdrm')`
# (src/meson.build), which is unconditionally true whenever with_asahi_vk
# is (root meson.build with_vdrm list) -- i.e. true on our real Windows
# cross build too. The narrower-looking `not with_vdrm` clause exists so
# this file also configures cleanly for the H1 native-tool-only bring-up
# build (`-Dtools=asahi` without `-Dvulkan-drivers=asahi`, used to build
# a native mesa_clc/asahi_clc pair ahead of the Windows cross build,
# BUILD-STATUS.md) where with_vdrm is false and inc_virtio_vdrm would
# otherwise be an unknown variable.
_asahi_lib_no_virtio = host_machine.system() == 'windows' or not with_vdrm

libasahi_lib = static_library(
  'asahi_lib',
  [libasahi_lib_files, libagx_shaders, agx_pack, sha1_h],
  include_directories : _asahi_lib_no_virtio
    ? [inc_asahi, libasahi_lib_inc_win]
    : [inc_asahi, inc_virtio_gpu, inc_virtio_vdrm],
  c_args : [no_override_init_args, '-Wno-c2x-extensions'],
  gnu_symbol_visibility : 'hidden',
  link_with: _asahi_lib_no_virtio
    ? [libpoly_nir]
    : [libasahi_decode, libvdrm, libpoly_nir],
  dependencies: _asahi_lib_no_virtio
    ? [dep_valgrind, idep_nir, idep_mesautil, idep_libagx, idep_libpoly]
    : [dep_libdrm, dep_valgrind, idep_nir, idep_mesautil, idep_libagx, idep_libpoly],
  build_by_default : false,
)

if with_tests
  test(
    'libasahi_tests',
    executable(
      'libasahi_tests',
      files(
        'tests/test-device.cpp',
        'tests/test-packing.cpp',
        'tests/test-tilebuffer.cpp',
      ),
      c_args : [c_msvc_compat_args, no_override_init_args],
      gnu_symbol_visibility : 'hidden',
      dependencies: [idep_gtest, idep_agx_pack, idep_mesautil],
      link_with : [libasahi_lib, libasahi_layout],
    ),
    suite : ['asahi'],
    protocol : 'gtest',
  )
endif

if dep_iokit.found()
  libasahi_wrap = shared_library(
    'wrap',
    'wrap.c',
    include_directories : [inc_src, inc_asahi],
    dependencies : [idep_mesautil, dep_iokit],
    c_args : [no_override_init_args, '-Wno-missing-prototypes'],
    gnu_symbol_visibility : 'hidden',
    build_by_default : with_tools.contains('asahi'),
    link_with: libasahi_decode,
  )
endif

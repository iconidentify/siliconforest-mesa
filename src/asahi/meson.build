# Copyright 2018 Rob Clark
# Copyright 2019 Collabora, Ltd.
# SPDX-License-Identifier: MIT

# This is used only for the r/e tooling on macOS
dep_iokit = dependency('IOKit', required : false)

inc_asahi = include_directories([
   '.', 'layout', 'lib', 'genxml', 'compiler'
])

subdir('isa')

_asahi_genxml_entered = with_gallium_asahi or with_asahi_vk or with_tools.contains('asahi')
if _asahi_genxml_entered
   subdir('genxml')
   subdir('libagx')
   subdir('compiler')
   subdir('clc')
endif

if with_gallium_asahi or with_asahi_vk
   subdir('layout')
   subdir('lib')
elif dep_iokit.found()
   # Just build enough for libwrap.dylib.
   #
   # honeykrisp-h1-execution.md SS5.2 (host tool bring-up for the Windows
   # cross-build's `-Dmesa-clc=system -Dprecomp-compiler=system` stage):
   # `subdir('genxml')` was already entered above whenever
   # with_tools.contains('asahi') (e.g. building asahi_clc/mesa_clc
   # natively on macOS without also enabling with_asahi_vk/
   # with_gallium_asahi) -- re-entering it here hits meson's "directory
   # already visited" error. Guard it (**bugfix**, not port-specific: this
   # combination -- tools=asahi without gallium/vulkan asahi, on a host
   # with IOKit -- was reachable before this port too, just apparently
   # never exercised).
   if not _asahi_genxml_entered
      subdir('genxml')
   endif
   subdir('lib')
endif

if with_asahi_vk
   subdir('vulkan')
endif

if with_tools.contains('drm-shim')
  subdir('drm-shim')
endif

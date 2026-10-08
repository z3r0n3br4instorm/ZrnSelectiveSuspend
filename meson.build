# SPDX-License-Identifier: GPL-2.0-only
project('zrn-selective-suspend', 'c',
  version: '0.1.0',
  license: 'GPL-2.0-only',
  license_files: ['LICENSE'],
  meson_version: '>=1.1',
  default_options: ['c_std=gnu11', 'warning_level=2', 'buildtype=debugoptimized'])

cc = meson.get_compiler('c')
add_project_arguments('-D_GNU_SOURCE', '-Wno-unused-parameter', language: 'c')

threads = dependency('threads')
dl = cc.find_library('dl', required: false)
# Headers only: the system package is optional, the wrap is the fallback.
vulkan_headers = dependency('VulkanHeaders', fallback: ['vulkan-headers', 'vulkan_headers_dep'])
# The layer needs only the type definitions of the window systems.
wsi_headers = [
  dependency('xcb').partial_dependency(compile_args: true, includes: true),
  dependency('x11').partial_dependency(compile_args: true, includes: true),
  dependency('wayland-client').partial_dependency(compile_args: true, includes: true),
]

python = import('python').find_installation('python3')

subdir('src/common')
subdir('tools')
subdir('src/layer')
subdir('src/daemon')
subdir('src/zssctl')
subdir('tests')

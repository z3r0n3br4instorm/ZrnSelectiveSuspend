# SPDX-License-Identifier: GPL-2.0-only
layer_lib = shared_library('zss_airlock',
  'icd.c', 'gate.c', 'device.c', 'cmd.c', 'swapchain.c', 'format.c', 'migrate.c', 'control.c', 'retain.c',
  'vk11.c', 'profile.c', 'lower.c', layer_tables,
  dependencies: [common_dep, vulkan_headers, wsi_headers, threads, dl],
  c_args: ['-fvisibility=hidden'],
  # The layer runs a thread of its own and keeps the application's objects. A
  # program that unloads its Vulkan driver between probes (Chromium does)
  # would pull the code out from under that thread, so the library stays.
  link_args: ['-Wl,-z,nodelete'],
  install: true)

# The loader finds the layer through this manifest. zss-run points it here.
manifest_conf = configuration_data()
manifest_conf.set('LIB', layer_lib.full_path())
layer_manifest = configure_file(input: 'zss_icd.json.in', output: 'zss_icd.json',
  configuration: manifest_conf)

run_conf = configuration_data()
run_conf.set('MANIFEST', meson.current_build_dir() / 'zss_icd.json')
configure_file(input: 'zss-run.in', output: 'zss-run', configuration: run_conf)

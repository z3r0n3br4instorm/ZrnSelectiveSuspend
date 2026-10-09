# SPDX-License-Identifier: GPL-2.0-only
# Generators that read the Vulkan registry, and what they produce.

survey_src = custom_target('zss-survey-source',
  output: 'zss_survey_gen.c',
  input: ['gen_survey.py', 'zss_registry.py'],
  depend_files: vk_registry,
  command: [python, '@INPUT0@', '@OUTPUT@', '--registry', vk_registry])

# An ordinary Vulkan layer that only watches; see tools/zss-survey.
survey_lib = shared_library('VkLayer_zss_survey', survey_src,
  dependencies: [vulkan_headers, wsi_headers, threads],
  c_args: ['-Wno-unused-function'],
  # It runs a thread that writes the report. A program that unloads its Vulkan
  # layers between probes (Chromium) would pull the code out from under it.
  link_args: ['-Wl,-z,nodelete'],
  name_prefix: 'lib')

survey_conf = configuration_data()
survey_conf.set('LIB', survey_lib.full_path())
configure_file(input: 'zss_survey.json.in', output: 'VkLayer_zss_survey.json', configuration: survey_conf)

# Tables for the layer itself (see src/layer/vk11.c).
layer_tables = custom_target('zss-layer-tables',
  output: 'zss_feats_gen.h',
  input: ['gen_layer_tables.py', 'zss_registry.py'],
  depend_files: vk_registry,
  command: [python, '@INPUT0@', '@OUTPUT@', '--registry', vk_registry])

# SPDX-License-Identifier: GPL-2.0-only
common_inc = include_directories('.')
common_lib = static_library('zss_common', 'zss_json.c', 'zss_ipc.c', 'zss_hash.c', pic: true)
common_dep = declare_dependency(link_with: common_lib, include_directories: common_inc)

# SPDX-License-Identifier: GPL-2.0-only
glslang = find_program('glslangValidator')
vulkan_loader = dependency('vulkan')

shader_headers = []
foreach stage : ['vert', 'frag']
  shader_headers += custom_target('quad_' + stage,
    input: 'testapp/quad.' + stage,
    output: 'quad_' + stage + '.h',
    command: [glslang, '-V', '--quiet', '--vn', 'quad_' + stage + '_spv', '-o', '@OUTPUT@', '@INPUT@'])
endforeach

testapp = executable('zss-testapp', 'testapp/zss-testapp.c', shader_headers,
  dependencies: [vulkan_headers, vulkan_loader])

test_json = executable('test_json', 'test_json.c', dependencies: [common_dep])
test('json', test_json)

test_retain = executable('test_retain', 'test_retain.c', '../src/layer/retain.c',
  include_directories: include_directories('../src/layer'), dependencies: [common_dep, threads])
test('retain', test_retain)

# Not a test: prints hashing and store throughput for docs/device-loss.md.
bench_retain = executable('bench_retain', 'bench_retain.c', '../src/layer/retain.c',
  include_directories: include_directories('../src/layer'), dependencies: [common_dep, threads])

python = find_program('python3')
# A small OpenGL program for the OpenGL-through-Zink tests; built only where OpenGL and Xlib are to be had.
gl_dep = dependency('gl', required: false)
x11_dep = dependency('x11', required: false)
gl_apps = []
if gl_dep.found() and x11_dep.found()
  gl_apps += executable('zss-glapp', 'testapp/zss-glapp.c', dependencies: [gl_dep, x11_dep])
endif

env = environment()
env.set('ZSS_BUILD', meson.project_build_root())
test('protocol', python, args: [files('test_protocol.py')], env: env, timeout: 60)
test('track-a', python, args: [files('track_a.py')], env: env, timeout: 900, is_parallel: false, depends: gl_apps)
test_rules = executable('test_rules', 'test_rules.c', '../src/daemon/power.c', '../src/daemon/sysfs.c',
  include_directories: include_directories('../src/daemon'), dependencies: [common_dep])
test('rules', test_rules)
test('packaging', python, args: [files('test_packaging.py')], env: env, timeout: 300)
test('tester-report', python, args: [files('test_report.py')], env: env, timeout: 600)
test('track-off', python, args: [files('track_off.py')], env: env, timeout: 300, is_parallel: false)
test('track-loss', python, args: [files('track_loss.py')], env: env, timeout: 600, is_parallel: false)
# Boots a QEMU guest; skipped (exit 77) where QEMU or KVM is unavailable.
test('track-b', python, args: [files('track_b.py')], env: env, timeout: 600, is_parallel: false)
# The kernel module, built for the running kernel and loaded only inside the guest.
test('track-k', python, args: [files('track_k.py')], env: env, timeout: 600, is_parallel: false)
# Lending a card to a virtual machine, in the same guest with an emulated IOMMU.
test('track-v', python, args: [files('track_v.py')], env: env, timeout: 600, is_parallel: false)

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

python = find_program('python3')
env = environment()
env.set('ZSS_BUILD', meson.project_build_root())
test('protocol', python, args: [files('test_protocol.py')], env: env, timeout: 60)
test('track-a', python, args: [files('track_a.py')], env: env, timeout: 600, is_parallel: false)
# Boots a QEMU guest; skipped (exit 77) where QEMU or KVM is unavailable.
test('track-b', python, args: [files('track_b.py')], env: env, timeout: 600, is_parallel: false)

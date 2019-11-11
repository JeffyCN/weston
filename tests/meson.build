plugin_test_shell_desktop = shared_library(
	'weston-test-desktop-shell',
	'weston-test-desktop-shell.c',
	include_directories: common_inc,
	dependencies: [ dep_lib_desktop, dep_libweston_public ],
	name_prefix: '',
	install: false
)
env_modmap += 'weston-test-desktop-shell.so=@0@;'.format(plugin_test_shell_desktop.full_path())

lib_test_runner = static_library(
	'test-runner',
	'weston-test-runner.c',
	dependencies: [
		dep_libweston_private_h_deps,
		dep_wayland_client,
	],
	include_directories: common_inc,
	install: false,
)
dep_test_runner = declare_dependency(
	dependencies: dep_wayland_client,
	link_with: lib_test_runner
)

lib_test_client = static_library(
	'test-client',
	[
		'weston-test-client-helper.c',
		'weston-test-fixture-compositor.c',
		weston_test_client_protocol_h,
		weston_test_protocol_c,
	],
	include_directories: common_inc,
	dependencies: [
		dep_libshared,
		dep_wayland_client,
		dep_libexec_weston,
		dep_pixman,
		dependency('cairo'),
	],
	install: false,
)
dep_test_client = declare_dependency(
	link_with: lib_test_client,
	dependencies: [
		dep_wayland_client,
		dep_test_runner,
		dep_pixman,
	]
)

exe_plugin_test = shared_library(
	'test-plugin',
	'weston-test.c',
	weston_test_server_protocol_h,
	weston_test_protocol_c,
	include_directories: common_inc,
	dependencies: [
		dep_libexec_weston,
		dep_libweston_private,
		dep_threads
	],
	name_prefix: '',
	install: false,
)

deps_zuc = [ dep_libshared ]
if get_option('test-junit-xml')
	d = dependency('libxml-2.0', version: '>= 2.6', required: false)
	if not d.found()
		error('JUnit XML support requires libxml-2.0 >= 2.6 which was not found. Or, you can use \'-Dtest-junit-xml=false\'.')
	endif
	deps_zuc += d
	config_h.set('ENABLE_JUNIT_XML', '1')
endif

lib_zuc = static_library(
	'zunitc',
	'../tools/zunitc/inc/zunitc/zunitc.h',
	'../tools/zunitc/inc/zunitc/zunitc_impl.h',
	'../tools/zunitc/src/zuc_base_logger.c',
	'../tools/zunitc/src/zuc_base_logger.h',
	'../tools/zunitc/src/zuc_collector.c',
	'../tools/zunitc/src/zuc_collector.h',
	'../tools/zunitc/src/zuc_context.h',
	'../tools/zunitc/src/zuc_event.h',
	'../tools/zunitc/src/zuc_event_listener.h',
	'../tools/zunitc/src/zuc_junit_reporter.c',
	'../tools/zunitc/src/zuc_junit_reporter.h',
	'../tools/zunitc/src/zuc_types.h',
	'../tools/zunitc/src/zunitc_impl.c',
	include_directories: [ common_inc, include_directories('../tools/zunitc/inc') ],
	dependencies: deps_zuc,
)
dep_zuc = declare_dependency(
	link_with: lib_zuc,
	dependencies: dep_libshared,
	include_directories: include_directories('../tools/zunitc/inc')
)

lib_zucmain = static_library(
	'zunitcmain',
	'../tools/zunitc/src/main.c',
	include_directories: [ common_inc, include_directories('../tools/zunitc/inc') ],
	dependencies: [ dep_libshared, dep_zuc ],
)
dep_zucmain = declare_dependency(
	link_with: lib_zucmain,
	dependencies: dep_zuc
)

tests = [
	{	'name': 'plugin-registry', },
	{	'name': 'roles', },
	{	'name': 'string', },
	{	'name': 'subsurface-shot', },
	{
		'name': 'vertex-clip',
		'dep_objs': dep_vertex_clipping,
	},
]

tests_standalone = [
	['config-parser', [], [ dep_zucmain ]],
	['matrix', [], [ dep_libm, dep_matrix_c ]],
	['timespec', [], [ dep_zucmain ]],
	['zuc',
		[
			'../tools/zunitc/test/fixtures_test.c',
			'../tools/zunitc/test/zunitc_test.c'
		],
		[ dep_zucmain ]
	],
]

tests_weston = [
	['bad-buffer'],
	['devices'],
	['event'],
	[
		'keyboard',
		[
			'input-timestamps-helper.c',
			input_timestamps_unstable_v1_client_protocol_h,
			input_timestamps_unstable_v1_protocol_c,
		]
	],
	[
		'linux-explicit-synchronization',
		[
			linux_explicit_synchronization_unstable_v1_client_protocol_h,
			linux_explicit_synchronization_unstable_v1_protocol_c,
		]
	],
	['internal-screenshot'],
	[
		'presentation',
		[
			presentation_time_client_protocol_h,
			presentation_time_protocol_c,
		]
	],
	[
		'pointer',
		[
			'input-timestamps-helper.c',
			input_timestamps_unstable_v1_client_protocol_h,
			input_timestamps_unstable_v1_protocol_c,
		]
	],
	['subsurface'],
	[
		'text',
		[
			text_input_unstable_v1_client_protocol_h,
			text_input_unstable_v1_protocol_c,
		]
	],
	[
		'touch',
		[
			'input-timestamps-helper.c',
			input_timestamps_unstable_v1_client_protocol_h,
			input_timestamps_unstable_v1_protocol_c,
		]
	],
	[
		'viewporter',
		[
			viewporter_client_protocol_h,
			viewporter_protocol_c,
		]
	],
]

if get_option('xwayland')
	d = dependency('x11', required: false)
	if not d.found()
		error('Xwayland tests require libX11 which was not found. Or, you can use \'-Dxwayland=false\'.')
	endif
	tests_weston += [ [ 'xwayland', [], d ] ]
endif

tests_weston_plugin = [
	['surface'],
	['surface-global'],
	['surface-screenshot', 'surface-screenshot-test.c', dep_libshared],
]

if get_option('shell-ivi')
	tests_weston += [
		[
			'ivi-shell-app',
			[
				ivi_application_client_protocol_h,
				ivi_application_protocol_c,
			]
		]
	]
	tests_weston_plugin += [
		['ivi-layout-internal'],
	]

	ivi_layout_test_plugin = shared_library(
		'test-ivi-layout',
		[
			'ivi-layout-test-plugin.c',
			weston_test_server_protocol_h,
			weston_test_protocol_c,
		],
		include_directories: common_inc,
		dependencies: [ dep_libweston_private, dep_libexec_weston ],
		name_prefix: '',
		install: false,
	)
	env_modmap += 'test-ivi-layout.so=' + ivi_layout_test_plugin.full_path() + ';'

	tests += [
		{
			'name': 'ivi-layout-client',
			'sources': [
				'ivi-layout-test-client.c',
				ivi_application_client_protocol_h,
				ivi_application_protocol_c,
			],
			'test_deps': [ ivi_layout_test_plugin ],
		},
	]
endif

env_test_weston = [
	'WESTON_TEST_REFERENCE_PATH=@0@/reference'.format(meson.current_source_dir()),
	'WESTON_MODULE_MAP=@0@'.format(env_modmap),
	'WESTON_DATA_DIR=' + join_paths(meson.current_source_dir(), '..', 'data'),
]

test_config_h = configuration_data()
test_config_h.set_quoted('WESTON_TEST_REFERENCE_PATH', meson.current_source_dir() + '/reference')
test_config_h.set_quoted('WESTON_MODULE_MAP', env_modmap)
test_config_h.set_quoted('WESTON_DATA_DIR', join_paths(meson.current_source_dir(), '..', 'data'))
test_config_h.set_quoted('TESTSUITE_PLUGIN_PATH', exe_plugin_test.full_path())
configure_file(output: 'test-config.h', configuration: test_config_h)

foreach t : tests
	t_name = 'test-' + t.get('name')
	t_sources = t.get('sources', [t.get('name') + '-test.c'])
	t_sources += weston_test_client_protocol_h

	t_deps = [ dep_test_client, dep_libweston_private_h ]
	t_deps += t.get('dep_objs', [])

	t_exe = executable(
		t_name,
		t_sources,
		c_args: [
			'-DUNIT_TEST',
			'-DTHIS_TEST_NAME="' + t_name + '"',
		],
		build_by_default: true,
		include_directories: common_inc,
		dependencies: t_deps,
		install: false,
	)

	test(t.get('name'), t_exe, depends: t.get('test_deps', []))
endforeach

# FIXME: the multiple loops is lame. rethink this.
foreach t : tests_standalone
	if t[0] != 'zuc'
		srcs_t = [
			'@0@-test.c'.format(t.get(0)),
			weston_test_client_protocol_h,
		]
	else
		srcs_t = []
	endif

	if t.length() > 1
		srcs_t += t.get(1)
	endif

	if t.length() > 2
		deps_t = t[2]
	else
		deps_t = [ dep_test_client ]
	endif

	exe_t = executable(
		'test-@0@'.format(t.get(0)),
		srcs_t,
		c_args: [ '-DUNIT_TEST' ],
		build_by_default: true,
		include_directories: common_inc,
		dependencies: deps_t,
		install: false,
	)

	# matrix-test is a manual test
	if t[0] != 'matrix'
		test(t.get(0), exe_t)
	endif
endforeach

foreach t : tests_weston
	srcs_t = [
		'@0@-test.c'.format(t.get(0)),
		weston_test_client_protocol_h,
	]
	if t.length() > 1
		srcs_t += t.get(1)
	endif

	deps_t = [
		dep_test_client
	]
	if t.length() > 2
		deps_t += t.get(2)
	endif

	args_t = [
		'--backend=headless-backend.so',
		'--socket=test-@0@'.format(t.get(0)),
		'--modules=@0@'.format(exe_plugin_test.full_path()),
		'--width=320',
		'--height=240',
	]

	if t.get(0) == 'xwayland'
		args_t += '--xwayland'
	endif

	# FIXME: Get this from the array ... ?
	if t.get(0) == 'internal-screenshot'
		args_t += [ '--config=@0@/internal-screenshot.ini'.format(meson.current_source_dir()) ]
		args_t += [ '--use-pixman' ]
		args_t += [ '--shell=desktop-shell.so' ]
	elif t[0] == 'subsurface-shot'
		args_t += [ '--no-config' ]
		args_t += [ '--use-pixman' ]
		args_t += [ '--width=320' ]
		args_t += [ '--height=240' ]
		args_t += [ '--shell=weston-test-desktop-shell.so' ]
	elif t.get(0) == 'linux-explicit-synchronization'
		args_t += [ '--use-pixman' ]
	elif t.get(0).startswith('ivi-')
		args_t += [ '--config=@0@/../ivi-shell/weston-ivi-test.ini'.format(meson.current_build_dir()) ]
		args_t += [ '--shell=ivi-shell.so' ]
	else
		args_t += [ '--no-config' ]
		args_t += [ '--shell=desktop-shell.so' ]
	endif

	exe_t = executable(
		'test-@0@'.format(t.get(0)),
		srcs_t,
		c_args: [ '-DUNIT_TEST' ],
		include_directories: common_inc,
		dependencies: deps_t,
		install: false,
	)

	env_t = [
		'WESTON_TEST_CLIENT_PATH=@0@'.format(exe_t.full_path())
	]
	env_t += env_test_weston

	test(t.get(0), exe_weston, env: env_t, args: args_t)
endforeach

foreach t : tests_weston_plugin
	srcs_t = []

	if t.length() > 1
		srcs_t += t.get(1)
	else
		srcs_t += '@0@-test.c'.format(t.get(0))
	endif

	deps_t = [
		dep_libweston_private,
	]
	if t.length() > 2
		deps_t += t.get(2)
	endif

	exe_t = shared_library(
		'test-@0@'.format(t.get(0)),
		srcs_t,
		include_directories: common_inc,
		dependencies: deps_t,
		name_prefix: '',
		install: false,
	)

	args_t = [
		'--backend=headless-backend.so',
		'--socket=test-@0@'.format(t.get(0)),
	]

	# FIXME: Get this from the array ... ?
	if t.get(0).startswith('ivi-')
		args_t += [ '--no-config' ]
		args_t += [ '--modules=@1@,@0@'.format(exe_plugin_test.full_path(),exe_t.full_path()) ]
		args_t += [ '--shell=ivi-shell.so' ]
	else
		args_t += [ '--no-config' ]
		args_t += [ '--shell=desktop-shell.so' ]
		args_t += [ '--modules=@0@'.format(exe_t.full_path()) ]
	endif

	# surface-screenshot is a manual test
	if t[0] != 'surface-screenshot'
		test(t.get(0), exe_weston, env: env_test_weston, args: args_t)
	endif
endforeach

if get_option('backend-drm')
	executable(
		'setbacklight',
		'setbacklight.c',
		dependencies: [
			dep_backlight,
			dep_libdrm,
			dependency('libudev')
		],
		include_directories: common_inc,
		install: false
	)
endif

if not get_option('dbus')
	dep_dbus = declare_dependency()
	subdir_done()
endif

dbus_dep = dependency('dbus-1', version: '>= 1.6', required: false)
if not dbus_dep.found()
	error('dbus support requires dbus-1 >= 1.6 which was not found. Or, you can use \'-Ddbus=false\'')
endif

config_h.set('HAVE_DBUS', '1')

srcs_dbus = [
	'dbus.c',
]

deps_dbus = [
	dep_libweston_private,
	dbus_dep,
]

lib_dbus = static_library(
	'dbus',
	srcs_dbus,
	include_directories: common_inc,
	dependencies: deps_dbus,
	name_prefix: '',
	install: true,
	install_dir: dir_module_libweston
)

dep_dbus = declare_dependency(
	link_with: lib_dbus,
	dependencies: deps_dbus,
	include_directories: include_directories('.')
)

install_headers(dbus_h, subdir: dir_include_libweston_install)

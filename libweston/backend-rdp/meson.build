if not get_option('backend-rdp')
	subdir_done()
endif

config_h.set('BUILD_RDP_COMPOSITOR', '1')

if get_option('rdp-thread-check')
        config_h.set('ENABLE_RDP_THREAD_CHECK', '1')
endif

dep_frdp = dependency('freerdp2', version: '>= 2.2.0', required: false)
if not dep_frdp.found()
	error('RDP-backend requires freerdp >= 2.2.0 which was not found. Or, you can use \'-Dbackend-rdp=false\'.')
endif

dep_wpr = dependency('winpr2', version: '>= 2.2.0', required: false)
if not dep_wpr.found()
	error('RDP-backend requires winpr >= 2.2.0 which was not found. Or, you can use \'-Dbackend-rdp=false\'.')
endif

deps_rdp = [
	dep_libweston_private,
	dep_frdp,
	dep_wpr,
]
srcs_rdp = [
        'rdp.c',
        'rdputil.c',
]

plugin_rdp = shared_library(
	'rdp-backend',
	srcs_rdp,
	include_directories: common_inc,
	dependencies: deps_rdp,
	name_prefix: '',
	install: true,
	install_dir: dir_module_libweston
)
env_modmap += 'rdp-backend.so=@0@;'.format(plugin_rdp.full_path())
install_headers(backend_rdp_h, subdir: dir_include_libweston_install)

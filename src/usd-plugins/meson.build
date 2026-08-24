# Minimal RemixParticleSystem.dll — only registers TfType for HasAPI() support.
# Property definitions come from the codeless schema (generatedSchema.usda + plugInfo.json)
# deployed alongside the DLL by scripts-common/install_usd_plugins.bat.

usd_schema_plugins_cpp_compiler = meson.get_compiler('cpp')
if usd_schema_plugins_cpp_compiler.get_id() != 'msvc'
  error('USD-SCHEMA-PLUGINS requires the MSVC compiler.')
endif

usd_schema_plugins_isdebug = get_option('buildtype') == 'debug'
usd_schema_plugins_is_woa = target_machine.cpu() == 'armv8'
dependencies_file = not usd_schema_plugins_is_woa ? 'packman-external.xml' : 'packman-external-arm64.xml'

packman_result = run_command(
  join_paths(meson.current_source_dir(), '../../scripts-common/packman/packman.cmd'),
  'pull',
  join_paths(meson.current_source_dir(), dependencies_file),
  '-p', 'windows-x86_64',
  check: true,
)
fs.read(join_paths(meson.current_source_dir(), dependencies_file))

usd_schema_plugins_nvusd_dir = usd_schema_plugins_isdebug ? '_external/usd_stock_x64/debug' : '_external/usd_stock_x64/release'
if usd_schema_plugins_is_woa
  usd_schema_plugins_nvusd_dir = usd_schema_plugins_isdebug ? '_external/usd_stock_arm64/debug' : '_external/usd_stock_arm64/release'
endif
usd_schema_plugins_nvusd_libdir = join_paths(meson.current_source_dir(), usd_schema_plugins_nvusd_dir, 'lib')

# This USD is the Python-enabled stock build, so its headers reach for Python's own -
# pxr/external/boost/python/detail/wrap_python.hpp includes pyconfig.h - and the plugins
# link the Python-backed USD libraries alongside it.
usd_schema_plugins_python_dir = '_external/python311_x64'
usd_schema_plugins_python_libdir = join_paths(meson.current_source_dir(), usd_schema_plugins_python_dir, 'libs')
usd_schema_plugins_includes = include_directories(
  usd_schema_plugins_nvusd_dir + '/include',
  usd_schema_plugins_python_dir + '/include',
)
if usd_schema_plugins_is_woa
  # The stock Arm64 USD package already carries the Python headers and libraries.
  usd_schema_plugins_python_libdir = usd_schema_plugins_nvusd_libdir
  usd_schema_plugins_includes = include_directories(usd_schema_plugins_nvusd_dir + '/include')
endif

usd_schema_plugins_deplibs = [
  'usd_arch',
  'usd_tf',
  'usd_usd',
  'usd_sdf',
  'usd_plug',
  'usd_python',
  get_option('buildtype') == 'debug' ? 'tbb12_debug' : 'tbb12',
]
usd_schema_plugins_linklibs = []
foreach ulib : usd_schema_plugins_deplibs
  usd_schema_plugins_linklibs += usd_schema_plugins_cpp_compiler.find_library(ulib, dirs: usd_schema_plugins_nvusd_libdir)
endforeach
usd_schema_plugins_linklibs += usd_schema_plugins_cpp_compiler.find_library('python311', dirs: usd_schema_plugins_python_libdir)

# Each plugin lives in a subdir; install_usd_plugins.bat walks those subdirs at install time.
subdir('RemixParticleSystem')
subdir('RemixCategories')

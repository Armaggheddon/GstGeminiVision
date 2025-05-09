project('gst-gemini-vision', 'c',
  version : '0.1.0',
  meson_version : '>=0.50.0',
  default_options : ['warning_level=1', 'c_std=c11'])

gst_version = '>=1.16.0' # Minimum GStreamer version

# Dependencies
glib_dep = dependency('glib-2.0', version : '>=2.56')
gobject_dep = dependency('gobject-2.0')
gst_dep = dependency('gstreamer-1.0', version : gst_version)
gstvideo_dep = dependency('gstreamer-video-1.0', version : gst_version)
gstbase_dep = dependency('gstreamer-base-1.0', version : gst_version)
curl_dep = dependency('libcurl', required : true)
jsonc_dep = dependency('json-c', required : true)
libjpeg_dep = dependency('libjpeg', required : false)

# Add GObject Introspection dependency
gir_dep = dependency('gobject-introspection-1.0', required : false)
build_gir = gir_dep.found()

plugin_name = 'geminivision'
plugin_so_name = 'gst' + plugin_name # This will be 'gstgeminivision'

plugin_sources = [
  'src/plugin.c',
  'src/gstgeminivision.c',
]

# Define the shared module with plugin_so_name as its Meson target name.
# This will produce libgstgeminivision.so.
# g-ir-scanner will use 'gstgeminivision' for its --library argument.
gst_geminivision_lib = shared_module(plugin_so_name,
  plugin_sources,
  dependencies : [glib_dep, gobject_dep, gst_dep, gstbase_dep, gstvideo_dep, curl_dep, jsonc_dep, libjpeg_dep],
  install : true,
  install_dir : join_paths(get_option('libdir'), 'gstreamer-1.0'),
  # name_prefix is not needed as 'gst' is part of plugin_so_name
)

# Optional: generate GObject Introspection data (for language bindings)
if build_gir
  gnome = import('gnome')
  gir_headers = ['src/gstgeminivision.h']
  gir_sources = plugin_sources
  
  # C flags for g-ir-scanner's compiler.
  # GST_PLUGIN_BUILD_STATIC is often defined for GStreamer plugins
  # to ensure G_DEFINE_TYPE and other macros correctly export symbols
  # for introspection.
  gir_scanner_cflags = ['-DGST_PLUGIN_BUILD_STATIC']

  gnome.generate_gir(gst_geminivision_lib, # Pass the correctly named library target
    sources : gir_sources + gir_headers,
    nsversion : '1.0',
    namespace : 'GstGeminiVision',
    symbol_prefix : 'gst_gemini_vision',
    identifier_prefix : 'GstGeminiVision',
    export_packages : [plugin_so_name], # Pkg-config name for export is 'gstgeminivision'
    includes : ['Gst-1.0', 'GstBase-1.0', 'GstVideo-1.0'],
    install : true,
    extra_args : [ # Arguments for g-ir-scanner itself
      '--warn-all',
      '--library-path=' + meson.current_build_dir(), # Path to find the .so
      # '--no-libtool', # Optional, often not needed if paths are clear
    ] + gir_scanner_cflags # Add C flags directly to extra_args
  )
else
  message('GObject introspection not found, not generating bindings')
endif

# Generate pkg-config file for our plugin
pkg = import('pkgconfig')
pkg.generate(
  name : plugin_name, # Human-readable package name (e.g., geminivision)
  filebase : plugin_so_name,  # Basename for .pc file (e.g., gstgeminivision.pc)
  description : 'GStreamer plugin for using Google Gemini API for vision processing',
  # Link with the correct library name, including the 'gst' prefix.
  libraries_private : ['-L${pluginsdir}', '-l' + plugin_so_name],
  variables : [
    'pluginsdir=' + join_paths('${libdir}', 'gstreamer-1.0')
  ],
  requires : [
    'gstreamer-1.0',
    'gstreamer-base-1.0',
    'gstreamer-video-1.0',
  ],
)
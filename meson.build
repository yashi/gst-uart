project('gst-uart', 'c',
        version : '0.1',
       default_options : 'warning_level=2')

glib = dependency('glib-2.0')
gst = dependency('gstreamer-1.0', version : '>1.0')
base = dependency('gstreamer-base-1.0', version : '>1.0')

subdir('src')

uart = library('gstuart',
               src,
               dependencies : base,
	       install : true,
	       install_dir : gst.get_variable('pluginsdir'))

cdata = configuration_data()
cdata.set_quoted('PACKAGE', meson.project_name())
cdata.set_quoted('VERSION', meson.project_version())
configure_file(output : 'config.h', configuration : cdata)

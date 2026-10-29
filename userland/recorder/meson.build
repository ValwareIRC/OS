project('os-record', 'c', version: '0.1', default_options: ['warning_level=2', 'c_std=gnu17'])
gst = dependency('gstreamer-1.0')
gstapp = dependency('gstreamer-app-1.0')
# The shell's screen recorder: frames in through shared memory, WebM out
executable('os-record', 'os-record.c', dependencies: [gst, gstapp], install: true,
           install_dir: get_option('libexecdir'))

libunwind_dep = dependency('libunwind-generic', required: false)

libsysprof_memory_preload_deps = [
  libsysprof_capture_dep,
  cc.find_library('dl', required: false),
  libunwind_dep,
]

config_h.set10('ENABLE_LIBUNWIND', libunwind_dep.found())

libsysprof_memory_preload = shared_library('sysprof-memory-collector',
  ['sysprof-memory-collector.c'],
  dependencies: libsysprof_memory_preload_deps,
)


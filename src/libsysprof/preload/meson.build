libsysprof_memory_preload_deps = [
  libsysprof_capture_dep,
  cc.find_library('dl', required: false),
]

libsysprof_memory_preload = shared_library('sysprof-memory-collector',
  ['sysprof-memory-collector.c'],
  dependencies: libsysprof_memory_preload_deps,
)


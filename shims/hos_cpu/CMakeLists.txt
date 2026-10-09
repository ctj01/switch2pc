# libhos_cpu.so: supervisor calls, threads, synchronisation, memory map, IPC transport.
add_library(hos_cpu SHARED
    cpu_init.c
    cpu_thread.c
    handles.c
    hipc.c
    ipc.c
    ipc_sm.c
    mem_regions.c
    svc_memory.c
    svc_misc.c
    svc_shmem.c
    svc_sleep.c
    svc_table.c
    sync.c
    trap.c
    trap_aarch64.S
)
set_target_properties(hos_cpu PROPERTIES
    OUTPUT_NAME hos_cpu
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
target_include_directories(hos_cpu
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(hos_cpu PRIVATE s2pc_common pthread)

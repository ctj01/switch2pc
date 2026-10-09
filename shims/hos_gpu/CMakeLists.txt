# libhos_gpu.so: nvdrv (nvmap, nvhost-ctrl) and vi with a null compositor that
# can dump frames. A real GPU backend is a later phase.
add_library(hos_gpu SHARED
    gpu_init.c
    nvdrv.c
    parcel.c
    swizzle.c
    vi.c
)
set_target_properties(hos_gpu PROPERTIES
    OUTPUT_NAME hos_gpu
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
target_link_libraries(hos_gpu PRIVATE hos_cpu s2pc_common pthread)

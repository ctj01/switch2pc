# libhos_gpu.so: nvdrv (nvmap, nvhost-ctrl) and vi with a null compositor that
# can dump frames. A real GPU backend is a later phase.
add_library(hos_gpu SHARED
    gpu_init.c
    nvdrv.c
    parcel.c
    swizzle.c
    vi.c
    display_x11.c
)
set_target_properties(hos_gpu PROPERTIES
    OUTPUT_NAME hos_gpu
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
# The window (#25) needs libX11 for the target: on an x86-64 host that is
# libx11-dev:arm64 from ports.ubuntu.com (see scripts/setup_wsl_arm64_libs.sh).
find_package(X11)
if(X11_FOUND)
    target_link_libraries(hos_gpu PRIVATE X11::X11)
    target_compile_definitions(hos_gpu PRIVATE HAVE_X11)
    message(STATUS "hos_gpu: X11 window enabled (${X11_X11_LIB})")
else()
    message(STATUS "hos_gpu: no libX11 for the target; HOS_WINDOW is unavailable")
endif()
target_link_libraries(hos_gpu PRIVATE hos_cpu s2pc_common pthread)

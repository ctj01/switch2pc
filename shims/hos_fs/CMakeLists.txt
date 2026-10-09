# libhos_fs.so: fsp-srv (sdmc, save data, romfs data storage) over the host filesystem.
add_library(hos_fs SHARED
    fs_init.c
    fsp.c
    romfs_build.c
)
set_target_properties(hos_fs PROPERTIES
    OUTPUT_NAME hos_fs
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
target_link_libraries(hos_fs PRIVATE hos_cpu s2pc_common)

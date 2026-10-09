# libhos_net.so: nifm (offline); bsd:u sockets later.
add_library(hos_net SHARED
    net_init.c
    nifm.c
)
set_target_properties(hos_net PROPERTIES
    OUTPUT_NAME hos_net
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
target_link_libraries(hos_net PRIVATE hos_cpu s2pc_common)

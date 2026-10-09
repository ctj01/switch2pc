# libhos_input.so: hid service; input backends to follow.
add_library(hos_input SHARED
    input_init.c
    hid.c
    input_state.c
)
set_target_properties(hos_input PROPERTIES
    OUTPUT_NAME hos_input
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
target_link_libraries(hos_input PRIVATE hos_cpu s2pc_common pthread)

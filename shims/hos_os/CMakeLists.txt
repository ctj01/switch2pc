# libhos_os.so: OS-level services (lm, set, set:sys, time, applet, apm, account, psm, pl).
add_library(hos_os SHARED
    os_init.c
    lm.c
    setsys.c
    time.c
    applet.c
    apm.c
    account.c
    psm.c
    pl.c
)
set_target_properties(hos_os PROPERTIES
    OUTPUT_NAME hos_os
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
target_link_libraries(hos_os PRIVATE hos_cpu s2pc_common)

# Common: logging, results, file IO. Linked statically into everything.
add_library(s2pc_common STATIC
    common/hos_log.c
    common/hos_file.c
    common/sha256.c
)
target_include_directories(s2pc_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Core: NSO and NRO parsing, LZ4, MOD0 relocation, instruction patching, ELF reading.
add_library(s2pc_core STATIC
    nso/lz4.c
    nso/nso.c
    nso/nro.c
    nso/mod0.c
    nso/elf2nso.c
    patch/patch.c
)
target_link_libraries(s2pc_core PUBLIC s2pc_common)

# switch2pc: the host-side inspection and packaging tool.
add_executable(switch2pc cli/main.c)
target_link_libraries(switch2pc PRIVATE s2pc_core)

# hosrun: the native ARM64 loader. The game binary runs inside this process.
if(S2PC_ARM64)
    add_executable(hosrun loader/main.c)
    target_link_libraries(hosrun PRIVATE s2pc_core hos_cpu
        -Wl,--no-as-needed ${S2PC_SERVICE_LIBS} -Wl,--as-needed)
    set(rpath "\$ORIGIN;\$ORIGIN/../shims/hos_cpu")
    foreach(lib ${S2PC_SERVICE_LIBS})
        set(rpath "${rpath};\$ORIGIN/../shims/${lib}")
    endforeach()
    set_target_properties(hosrun PROPERTIES
        BUILD_RPATH "${rpath}"
        INSTALL_RPATH "\$ORIGIN")
endif()


# Example modules. Built with the cross toolchain, packaged into NSO files by
# the host switch2pc tool. The tool must run on the build machine: pass
# -DS2PC_HOST_TOOL=<native switch2pc> when cross-compiling from x86-64.
if(S2PC_HOST_TOOL)
    set(S2PC_TOOL ${S2PC_HOST_TOOL})
    set(S2PC_TOOL_DEP "")
else()
    set(S2PC_TOOL $<TARGET_FILE:switch2pc>)
    set(S2PC_TOOL_DEP switch2pc)
endif()

set(S2PC_MODULE_CFLAGS
    -std=c11 -O2 -g -Wall -Wextra -Werror -ffreestanding -fPIE
    -fno-stack-protector -fno-asynchronous-unwind-tables -fno-builtin
    -mno-outline-atomics -nostdlib -nostartfiles)
set(S2PC_MODULE_LDFLAGS
    -Wl,-pie -Wl,--no-dynamic-linker -Wl,-z,max-page-size=0x1000 -Wl,-z,norelro
    -Wl,--build-id=none -Wl,--hash-style=sysv -Wl,--export-dynamic
    -Wl,--unresolved-symbols=ignore-all)
set(S2PC_MODULE_LD ${CMAKE_CURRENT_SOURCE_DIR}/kernel_test/link.ld)

# Raw assembly module: .text only, no MOD0.
function(s2pc_add_asm_example name)
    set(src ${CMAKE_CURRENT_SOURCE_DIR}/${name}/${name}.S)
    set(dir ${CMAKE_CURRENT_BINARY_DIR}/${name})
    file(MAKE_DIRECTORY ${dir})
    add_custom_command(
        OUTPUT ${dir}/${name}.elf
        COMMAND ${CMAKE_C_COMPILER} -nostdlib -static -Wl,-Ttext=0 -Wl,--build-id=none
                -o ${dir}/${name}.elf ${src}
        DEPENDS ${src}
        COMMENT "Assembling ${name}.elf")
    add_custom_command(
        OUTPUT ${dir}/${name}.bin
        COMMAND ${CMAKE_OBJCOPY} -O binary -j .text ${dir}/${name}.elf ${dir}/${name}.bin
        DEPENDS ${dir}/${name}.elf
        COMMENT "Extracting ${name}.bin")
    add_custom_command(
        OUTPUT ${dir}/${name}.nso
        COMMAND ${S2PC_TOOL} mknso --text ${dir}/${name}.bin --name ${name} -o ${dir}/${name}.nso
        DEPENDS ${dir}/${name}.bin ${S2PC_TOOL_DEP}
        COMMENT "Packaging ${name}.nso")
    add_custom_target(${name}_nso ALL DEPENDS ${dir}/${name}.nso)
endfunction()

# Freestanding C module: static PIE with MOD0, converted with elf2nso. Set
# S2PC_MODULE_NSO_FLAGS to override the packaging flags (default: --compress).
#   s2pc_add_c_module(<target> <output dir> <nso name> <sources...>)
set(S2PC_MODULE_NSO_FLAGS --compress)
function(s2pc_add_c_module target dir nso_name)
    file(MAKE_DIRECTORY ${dir})
    set(headers ${CMAKE_CURRENT_SOURCE_DIR}/kernel_test/hos.h
                ${CMAKE_CURRENT_SOURCE_DIR}/kernel_test/ipc_client.h)
    add_custom_command(
        OUTPUT ${dir}/${nso_name}.elf
        COMMAND ${CMAKE_C_COMPILER} ${S2PC_MODULE_CFLAGS} ${S2PC_MODULE_LDFLAGS}
                ${S2PC_MODULE_EXTRA_LDFLAGS}
                -Wl,-T,${S2PC_MODULE_LD} -o ${dir}/${nso_name}.elf ${ARGN}
        DEPENDS ${ARGN} ${S2PC_MODULE_LD} ${headers}
        COMMENT "Linking ${nso_name}.elf")
    add_custom_command(
        OUTPUT ${dir}/${nso_name}.nso
        COMMAND ${S2PC_TOOL} elf2nso ${dir}/${nso_name}.elf --name ${nso_name}
                ${S2PC_MODULE_NSO_FLAGS} -o ${dir}/${nso_name}.nso
        DEPENDS ${dir}/${nso_name}.elf ${S2PC_TOOL_DEP}
        COMMENT "Packaging ${nso_name}.nso")
    add_custom_target(${target} ALL DEPENDS ${dir}/${nso_name}.nso)
endfunction()

s2pc_add_asm_example(hello)

set(kt ${CMAKE_CURRENT_SOURCE_DIR}/kernel_test)
s2pc_add_c_module(kernel_test_nso ${CMAKE_CURRENT_BINARY_DIR}/kernel_test kernel_test
    ${kt}/crt0.S ${kt}/main.c)

# The homebrew ABI test: its crt0 leaves room for the NRO header, so
# tools/nso2nro.py wraps it in place; nso2nro takes uncompressed NSOs only.
set(hb ${CMAKE_CURRENT_SOURCE_DIR}/hbabi_test)
set(S2PC_MODULE_NSO_FLAGS)
s2pc_add_c_module(hbabi_test_nso ${CMAKE_CURRENT_BINARY_DIR}/hbabi_test hbabi_test
    ${hb}/crt0.S ${hb}/main.c)
set(S2PC_MODULE_NSO_FLAGS --compress)

# Two modules linked at load time: main imports from sdk.
set(mm ${CMAKE_CURRENT_SOURCE_DIR}/multi_module)
s2pc_add_c_module(multi_module_main_nso ${CMAKE_CURRENT_BINARY_DIR}/multi_module main
    ${kt}/crt0.S ${mm}/main.c)
# sdk has a constructor and, when ld.lld is available (GNU ld 2.42 ignores
# -z pack-relative-relocs on aarch64), packs its relative relocations as
# DT_RELR: with no rtld, hosrun must apply the RELR (#22) and run the
# initialiser (#21).
find_program(S2PC_LLD ld.lld)
set(lld_relr OFF)
if(S2PC_LLD)
    # pack-relative-relocs arrived in LLD 15; older ones ignore it with a warning.
    execute_process(COMMAND ${S2PC_LLD} --version OUTPUT_VARIABLE lld_version ERROR_QUIET)
    if(lld_version MATCHES "LLD ([0-9]+)\\." AND CMAKE_MATCH_1 GREATER_EQUAL 15)
        set(lld_relr ON)
    else()
        message(STATUS "switch2pc: ${S2PC_LLD} predates DT_RELR (LLD 15); the sdk module keeps RELA")
    endif()
endif()
if(lld_relr)
    # The cross gcc does not look for ld.lld itself; hand it a directory whose "ld" is lld.
    set(lld_dir ${CMAKE_CURRENT_BINARY_DIR}/lld-bin)
    file(MAKE_DIRECTORY ${lld_dir})
    file(CREATE_LINK ${S2PC_LLD} ${lld_dir}/ld SYMBOLIC)
    set(S2PC_MODULE_EXTRA_LDFLAGS -B${lld_dir} -Wl,-z,pack-relative-relocs)
    set(S2PC_SDK_RELR ON CACHE INTERNAL "sdk example uses DT_RELR")
else()
    set(S2PC_SDK_RELR OFF CACHE INTERNAL "sdk example uses DT_RELR")
endif()
s2pc_add_c_module(multi_module_sdk_nso ${CMAKE_CURRENT_BINARY_DIR}/multi_module sdk
    ${mm}/crt0_lib.S ${mm}/sdk.c)
set(S2PC_MODULE_EXTRA_LDFLAGS)

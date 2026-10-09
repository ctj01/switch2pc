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

# Freestanding C module: static PIE with MOD0, converted with elf2nso.
#   s2pc_add_c_module(<target> <output dir> <nso name> <sources...>)
function(s2pc_add_c_module target dir nso_name)
    file(MAKE_DIRECTORY ${dir})
    set(headers ${CMAKE_CURRENT_SOURCE_DIR}/kernel_test/hos.h
                ${CMAKE_CURRENT_SOURCE_DIR}/kernel_test/ipc_client.h)
    add_custom_command(
        OUTPUT ${dir}/${nso_name}.elf
        COMMAND ${CMAKE_C_COMPILER} ${S2PC_MODULE_CFLAGS} ${S2PC_MODULE_LDFLAGS}
                -Wl,-T,${S2PC_MODULE_LD} -o ${dir}/${nso_name}.elf ${ARGN}
        DEPENDS ${ARGN} ${S2PC_MODULE_LD} ${headers}
        COMMENT "Linking ${nso_name}.elf")
    add_custom_command(
        OUTPUT ${dir}/${nso_name}.nso
        COMMAND ${S2PC_TOOL} elf2nso ${dir}/${nso_name}.elf --name ${nso_name} --compress
                -o ${dir}/${nso_name}.nso
        DEPENDS ${dir}/${nso_name}.elf ${S2PC_TOOL_DEP}
        COMMENT "Packaging ${nso_name}.nso")
    add_custom_target(${target} ALL DEPENDS ${dir}/${nso_name}.nso)
endfunction()

s2pc_add_asm_example(hello)

set(kt ${CMAKE_CURRENT_SOURCE_DIR}/kernel_test)
s2pc_add_c_module(kernel_test_nso ${CMAKE_CURRENT_BINARY_DIR}/kernel_test kernel_test
    ${kt}/crt0.S ${kt}/main.c)

# Two modules linked at load time: main imports from sdk.
set(mm ${CMAKE_CURRENT_SOURCE_DIR}/multi_module)
s2pc_add_c_module(multi_module_main_nso ${CMAKE_CURRENT_BINARY_DIR}/multi_module main
    ${kt}/crt0.S ${mm}/main.c)
s2pc_add_c_module(multi_module_sdk_nso ${CMAKE_CURRENT_BINARY_DIR}/multi_module sdk
    ${mm}/crt0_lib.S ${mm}/sdk.c)

# libFuzzer harnesses (#11) for the three parsers that read untrusted bytes:
# the NSO loader, the MOD0 relocator and the HIPC message decoder. Host-only,
# clang-only, behind -DS2PC_FUZZ=ON; scripts/fuzz.sh configures, builds, seeds
# and runs them. The parser sources are compiled into each harness so the
# sanitizers instrument them too.
if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "S2PC_FUZZ needs clang (libFuzzer); got ${CMAKE_C_COMPILER_ID}")
endif()

set(S2PC_FUZZ_FLAGS -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all
    -fno-omit-frame-pointer -g -O1)
set(S2PC_FUZZ_COMMON
    ${CMAKE_SOURCE_DIR}/src/common/hos_log.c
    ${CMAKE_SOURCE_DIR}/src/common/sha256.c)

function(s2pc_add_fuzzer name)
    add_executable(${name} ${name}.c ${S2PC_FUZZ_COMMON} ${ARGN})
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/src
                                               ${CMAKE_SOURCE_DIR}/shims/hos_cpu)
    target_compile_options(${name} PRIVATE ${S2PC_FUZZ_FLAGS})
    target_link_options(${name} PRIVATE ${S2PC_FUZZ_FLAGS})
endfunction()

s2pc_add_fuzzer(fuzz_nso_load
    ${CMAKE_SOURCE_DIR}/src/nso/nso.c ${CMAKE_SOURCE_DIR}/src/nso/lz4.c)
s2pc_add_fuzzer(fuzz_mod0_relocate ${CMAKE_SOURCE_DIR}/src/nso/mod0.c)
s2pc_add_fuzzer(fuzz_hipc_parse ${CMAKE_SOURCE_DIR}/shims/hos_cpu/hipc.c)

# The seed corpus is generated, not committed: realistic inputs from the same
# writers the tests use (nso_write, the test suite's MOD0 image, ipc_client.h).
add_executable(fuzz_seeds seeds.c ${S2PC_FUZZ_COMMON}
    ${CMAKE_SOURCE_DIR}/src/nso/nso.c ${CMAKE_SOURCE_DIR}/src/nso/lz4.c)
target_include_directories(fuzz_seeds PRIVATE ${CMAKE_SOURCE_DIR}/src
                                              ${CMAKE_SOURCE_DIR}/examples/kernel_test)

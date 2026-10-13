cmake_minimum_required(VERSION 3.16)
project(switch2pc VERSION 0.1.0 LANGUAGES C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options(-Wall -Wextra -Werror -Wshadow -Wvla -Wformat=2 -Wundef)
add_compile_definitions(_GNU_SOURCE)

option(S2PC_BUILD_TESTS "Build the test suite" ON)
option(S2PC_FUZZ "Build the libFuzzer harnesses (clang only; see scripts/fuzz.sh)" OFF)

# Host tool used to build example NSOs. Defaults to the in-tree target, which only
# runs on the build machine if it is ARM64 or has qemu binfmt registered. On a plain
# x86-64 host pass -DS2PC_HOST_TOOL=/path/to/native/switch2pc when cross-compiling.
set(S2PC_HOST_TOOL "" CACHE FILEPATH "Native switch2pc binary used for example packaging")

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(S2PC_ARM64 ON)
else()
    set(S2PC_ARM64 OFF)
endif()
message(STATUS "switch2pc: ARM64 runtime targets ${S2PC_ARM64} (processor ${CMAKE_SYSTEM_PROCESSOR})")

# Service libraries hosrun and the runtime tests link with --no-as-needed so
# they stay in DT_NEEDED although nothing references their symbols directly.
set(S2PC_SERVICE_LIBS hos_os hos_input hos_fs hos_gpu hos_audio hos_net)

add_subdirectory(src)
if(S2PC_ARM64)
    add_subdirectory(shims)
    add_subdirectory(examples)
endif()
if(S2PC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
if(S2PC_FUZZ)
    add_subdirectory(fuzz)
endif()

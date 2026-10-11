# Cross toolchain: build the ARM64 Linux targets (hosrun, libhos_*.so, tests)
# on an x86-64 host. Tests run through qemu-user when not on real ARM64.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_ASM_COMPILER aarch64-linux-gnu-gcc)

# Cross libc lives in /usr/aarch64-linux-gnu; optional libraries for the
# target (libX11, libpulse, libasound) come from Ubuntu's arm64 multiarch
# packages in /usr/lib/aarch64-linux-gnu, so search "/" with that arch too.
set(CMAKE_LIBRARY_ARCHITECTURE aarch64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu /)
set(S2PC_MULTIARCH_LIBDIR /usr/lib/aarch64-linux-gnu)
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-Wl,-rpath-link,${S2PC_MULTIARCH_LIBDIR}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-Wl,-rpath-link,${S2PC_MULTIARCH_LIBDIR}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

if(NOT DEFINED CMAKE_CROSSCOMPILING_EMULATOR)
    set(CMAKE_CROSSCOMPILING_EMULATOR "qemu-aarch64;-L;/usr/aarch64-linux-gnu")
endif()

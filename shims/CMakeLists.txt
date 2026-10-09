# Each HOS subsystem is its own shared library. hos_cpu is the kernel; the
# others are IPC services that register with it from their constructors.
add_subdirectory(hos_cpu)
add_subdirectory(hos_os)
add_subdirectory(hos_input)
add_subdirectory(hos_fs)
add_subdirectory(hos_gpu)
add_subdirectory(hos_audio)
add_subdirectory(hos_net)

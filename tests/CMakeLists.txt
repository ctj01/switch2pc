add_library(test_framework STATIC framework/test.c)
target_include_directories(test_framework PUBLIC framework)

function(s2pc_add_test name)
    add_executable(${name} ${name}.c)
    target_link_libraries(${name} PRIVATE test_framework ${ARGN})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

# Host-side units: build and run on any architecture.
s2pc_add_test(test_lz4 s2pc_core)
s2pc_add_test(test_nso s2pc_core)
s2pc_add_test(test_patch s2pc_core)
s2pc_add_test(test_sha256 s2pc_common)
s2pc_add_test(test_mme s2pc_common)
target_sources(test_mme PRIVATE ${CMAKE_SOURCE_DIR}/shims/hos_gpu/gpu_mme.c)
target_include_directories(test_mme PRIVATE ${CMAKE_SOURCE_DIR}/shims/hos_gpu)
s2pc_add_test(test_shader s2pc_common m)
target_sources(test_shader PRIVATE ${CMAKE_SOURCE_DIR}/shims/hos_gpu/gpu_shader.c)
target_include_directories(test_shader PRIVATE ${CMAKE_SOURCE_DIR}/shims/hos_gpu)

# Runtime: ARM64 only (qemu-user through CMAKE_CROSSCOMPILING_EMULATOR when cross-building).
if(S2PC_ARM64)
    set(runtime_libs hos_cpu -Wl,--no-as-needed ${S2PC_SERVICE_LIBS} -Wl,--as-needed s2pc_common)
    set(runtime_rpath "${CMAKE_BINARY_DIR}/shims/hos_cpu")
    foreach(lib ${S2PC_SERVICE_LIBS})
        set(runtime_rpath "${runtime_rpath};${CMAKE_BINARY_DIR}/shims/${lib}")
    endforeach()
    s2pc_add_test(test_svc ${runtime_libs})
    s2pc_add_test(test_ipc ${runtime_libs})
    target_include_directories(test_ipc PRIVATE ${CMAKE_SOURCE_DIR}/examples/kernel_test)
    set_target_properties(test_svc test_ipc PROPERTIES BUILD_RPATH "${runtime_rpath}")

    set(hello_nso ${CMAKE_BINARY_DIR}/examples/hello/hello.nso)
    add_test(NAME smoke_hello COMMAND hosrun ${hello_nso})
    set_tests_properties(smoke_hello PROPERTIES
        PASS_REGULAR_EXPRESSION "hello from HOS.*svcBreak User \\[notification\\].*svcExitProcess"
        FAIL_REGULAR_EXPRESSION "fatal|unimplemented svc|svcBreak Panic")

    add_test(NAME smoke_hello_info COMMAND switch2pc info ${hello_nso})
    set_tests_properties(smoke_hello_info PROPERTIES
        PASS_REGULAR_EXPRESSION "patch sites   7 total, 7 svc->brk")

    # Fixtures for the service tests: an SD card directory and a fake romfs image.
    set(fixture_dir ${CMAKE_BINARY_DIR}/tests/fixtures)
    file(MAKE_DIRECTORY ${fixture_dir}/sdmc)
    file(WRITE ${fixture_dir}/romfs.bin "ROMFS_FIXTURE: not a real romfs image, 64 bytes....")

    set(kernel_test_nso ${CMAKE_BINARY_DIR}/examples/kernel_test/kernel_test.nso)
    add_test(NAME smoke_kernel_test COMMAND hosrun --sdmc ${fixture_dir}/sdmc
             --romfs ${fixture_dir}/romfs.bin ${kernel_test_nso})
    set_tests_properties(smoke_kernel_test PROPERTIES
        TIMEOUT 120
        PASS_REGULAR_EXPRESSION "hello via lm \\(session\\).*hello via lm \\(domain\\).*kernel_test done"
        FAIL_REGULAR_EXPRESSION "FAIL |fatal|svcBreak Panic")

    # Two modules: main imports from sdk, resolved at load time. The directory form
    # exercises the exefs walk (main + sdk found by name).
    set(mm_dir ${CMAKE_BINARY_DIR}/examples/multi_module)
    # With ld.lld the sdk module carries DT_RELR, which hosrun must apply itself.
    if(S2PC_SDK_RELR)
        set(mm_relr_re "[1-9][0-9]* relr applied.*")
    else()
        set(mm_relr_re "")
    endif()
    add_test(NAME smoke_multi_module COMMAND hosrun ${mm_dir}/main.nso ${mm_dir}/sdk.nso)
    set_tests_properties(smoke_multi_module PROPERTIES
        PASS_REGULAR_EXPRESSION "sdk(\\.nso)?: 1 initialiser.*${mm_relr_re}sdk_add\\(2,3\\) = 0x5.*sdk_tick advances.*message: sdk module says hi.*sdk init ran.*multi_module done"
        FAIL_REGULAR_EXPRESSION "FAIL|fatal|unresolved symbols")

    file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/tests/fixtures/exefs)
    add_test(NAME smoke_multi_module_exefs
             COMMAND ${CMAKE_COMMAND} -E copy_if_different ${mm_dir}/main.nso
                     ${CMAKE_BINARY_DIR}/tests/fixtures/exefs/main)
    add_test(NAME smoke_multi_module_exefs_sdk
             COMMAND ${CMAKE_COMMAND} -E copy_if_different ${mm_dir}/sdk.nso
                     ${CMAKE_BINARY_DIR}/tests/fixtures/exefs/sdk)
    add_test(NAME smoke_multi_module_exefs_run
             COMMAND hosrun ${CMAKE_BINARY_DIR}/tests/fixtures/exefs)
    set_tests_properties(smoke_multi_module_exefs_run PROPERTIES
        DEPENDS "smoke_multi_module_exefs;smoke_multi_module_exefs_sdk"
        PASS_REGULAR_EXPRESSION "multi_module done"
        FAIL_REGULAR_EXPRESSION "FAIL|fatal|unresolved symbols")

    # A real libnx binary, when scripts/build_libnx_hello.sh has produced it. Its own
    # crt0 and __appInit (sm, set:sys, applet, apm, hid, fsp-srv) run before main.
    set(libnx_nso ${CMAKE_SOURCE_DIR}/examples/libnx_hello/build/hello.nso)
    if(EXISTS ${libnx_nso})
        add_test(NAME smoke_libnx_hello COMMAND hosrun --sdmc ${fixture_dir}/sdmc ${libnx_nso})
        set_tests_properties(smoke_libnx_hello PROPERTIES
            TIMEOUT 120
            PASS_REGULAR_EXPRESSION "main reached.*sdmc read back.*thread counter 5000 \\(expect 5000\\).*libnx hello: done"
            FAIL_REGULAR_EXPRESSION "FAIL |fatal|svcBreak Panic")
    else()
        message(STATUS "switch2pc: examples/libnx_hello/build/hello.nso not found; libnx smoke test skipped")
    endif()

    # libnx console: vi + nvdrv + bufferqueue; frames are dumped as PPM and must contain text.
    set(libnx_console_nso ${CMAKE_SOURCE_DIR}/examples/libnx_console/build/console.nso)
    if(EXISTS ${libnx_console_nso})
        file(MAKE_DIRECTORY ${fixture_dir}/frames)
        add_test(NAME smoke_libnx_console
                 COMMAND hosrun --sdmc ${fixture_dir}/sdmc ${libnx_console_nso})
        set_tests_properties(smoke_libnx_console PROPERTIES
            TIMEOUT 120
            ENVIRONMENT "HOS_FRAMES=${fixture_dir}/frames"
            PASS_REGULAR_EXPRESSION "frame 3: 1280x720 format [0-9], [1-9][0-9]* non-black pixels.*libnx console: done"
            FAIL_REGULAR_EXPRESSION "FAIL |fatal|svcBreak Panic")
    endif()

    # libnx services: audout null sink and nifm offline, through the libnx client code.
    set(libnx_services_nso ${CMAKE_SOURCE_DIR}/examples/libnx_services/build/services.nso)
    if(EXISTS ${libnx_services_nso})
        add_test(NAME smoke_libnx_services
                 COMMAND hosrun --sdmc ${fixture_dir}/sdmc ${libnx_services_nso})
        set_tests_properties(smoke_libnx_services PROPERTIES
            TIMEOUT 120
            ENVIRONMENT "HOS_AUDIO_DUMP=${fixture_dir}/audio.pcm"
            PASS_REGULAR_EXPRESSION "audio released 1 buffers rc=0 \\(same buffer: 1\\).*internet: offline.*account: 1 user\\(s\\), listed 1 rc=0.*account: nickname \"[^\"]+\" rc=0.*battery: 100%, charger type 1.*libnx services: done"
            FAIL_REGULAR_EXPRESSION "FAIL |fatal|svcBreak Panic")
    endif()

    # libnx romfs: the data storage is a romfs image built from a directory.
    file(MAKE_DIRECTORY ${fixture_dir}/romfs_dir/sub/dir)
    file(WRITE ${fixture_dir}/romfs_dir/hello.txt "hello romfs\n")
    file(WRITE ${fixture_dir}/romfs_dir/sub/a.txt "a\n")
    file(WRITE ${fixture_dir}/romfs_dir/sub/b.txt "bb\n")
    string(ASCII 1 2 3 4 5 6 7 8 9 nested_bytes)
    file(WRITE ${fixture_dir}/romfs_dir/sub/dir/nested.bin "${nested_bytes}")
    set(libnx_romfs_nso ${CMAKE_SOURCE_DIR}/examples/libnx_romfs/build/romfs.nso)
    if(EXISTS ${libnx_romfs_nso})
        add_test(NAME smoke_libnx_romfs
                 COMMAND hosrun --romfs ${fixture_dir}/romfs_dir ${libnx_romfs_nso})
        set_tests_properties(smoke_libnx_romfs PROPERTIES
            TIMEOUT 120
            PASS_REGULAR_EXPRESSION "romfs read: \"hello romfs\".*nested: 8 bytes read, first 01 02 03, size 9.*romfs:/sub has 5 entries.*libnx romfs: done"
            FAIL_REGULAR_EXPRESSION "FAIL |fatal|svcBreak Panic")
    endif()

    # libnx input: scripted presses land in the hid shared memory; A at 300 ms, Plus at 800 ms.
    file(WRITE ${fixture_dir}/input.txt "300 1 20000 -20000 0 0\n500 0\n800 400\n")
    set(libnx_input_nso ${CMAKE_SOURCE_DIR}/examples/libnx_input/build/input.nso)
    if(EXISTS ${libnx_input_nso})
        add_test(NAME smoke_libnx_input COMMAND hosrun --sdmc ${fixture_dir}/sdmc ${libnx_input_nso})
        set_tests_properties(smoke_libnx_input PROPERTIES
            TIMEOUT 120
            ENVIRONMENT "HOS_INPUT_SCRIPT=${fixture_dir}/input.txt"
            PASS_REGULAR_EXPRESSION "kDown 0x1, left stick 20000 -20000.*kDown 0x400.*Plus pressed.*libnx input: done"
            FAIL_REGULAR_EXPRESSION "FAIL |fatal|svcBreak Panic")
    endif()

    # libnx sockets: bsd:u over host sockets (TCP and UDP on loopback, poll, O_NONBLOCK).
    set(libnx_sockets_nso ${CMAKE_SOURCE_DIR}/examples/libnx_sockets/build/sockets.nso)
    if(EXISTS ${libnx_sockets_nso})
        add_test(NAME smoke_libnx_sockets COMMAND hosrun --sdmc ${fixture_dir}/sdmc ${libnx_sockets_nso})
        set_tests_properties(smoke_libnx_sockets PROPERTIES
            TIMEOUT 120
            PASS_REGULAR_EXPRESSION "socketInitializeDefault rc=0.*tcp: accepted 1 from 127.0.0.1.*received 4 \"ping\".*would-block 1.*after peer close recv 0.*udp: received 8 \"datagram\".*libnx sockets: done"
            FAIL_REGULAR_EXPRESSION "FAIL |fatal|svcBreak Panic")
    endif()

    # GPU milestones M0 and M1 (docs/gpu.md): a deko3d program creates a device,
    # swapchain and queue, clears and presents 60 frames (through a 3D macro), then
    # checks an inline upload, a DMA buffer copy and an image round trip byte for byte.
    set(libnx_deko3d_nso ${CMAKE_SOURCE_DIR}/examples/libnx_deko3d/build/deko3d.nso)
    if(EXISTS ${libnx_deko3d_nso})
        add_test(NAME smoke_deko3d COMMAND hosrun --sdmc ${fixture_dir}/sdmc ${libnx_deko3d_nso})
        set_tests_properties(smoke_deko3d PROPERTIES
            TIMEOUT 120
            PASS_REGULAR_EXPRESSION "deko3d: device created.*swapchain created.*queue created.*clears.*frame 40 presented.*push data ok.*copy buffer ok.*image round trip ok.*deko3d: done"
            FAIL_REGULAR_EXPRESSION "FAIL|fatal|Panic|does not parse|not mapped|deko3d: .*: result [1-9]")
    endif()

    # GPU milestone M3a: deko_basic's triangle (shaders compiled by uam, vertex shader
    # indexing a constant buffer by gl_VertexID, perspective-correct colour), rendered
    # on the CPU and compared pixel by pixel with an independent reference.
    set(tri_dir ${CMAKE_SOURCE_DIR}/examples/libnx_deko3d_triangle)
    find_package(Python3 COMPONENTS Interpreter)
    if(EXISTS ${tri_dir}/build/deko3d_triangle.nso AND EXISTS ${tri_dir}/romfs/shaders)
        file(MAKE_DIRECTORY ${fixture_dir}/triangle)
        add_test(NAME smoke_deko3d_triangle
                 COMMAND hosrun --romfs ${tri_dir}/romfs --sdmc ${fixture_dir}/sdmc
                         ${tri_dir}/build/deko3d_triangle.nso)
        set_tests_properties(smoke_deko3d_triangle PROPERTIES
            TIMEOUT 300
            ENVIRONMENT "HOS_FRAMES=${fixture_dir}/triangle"
            PASS_REGULAR_EXPRESSION "triangle scene ready.*1 draws, 1 triangles, 460800 pixels.*deko3d triangle: done"
            FAIL_REGULAR_EXPRESSION "FAIL|fatal|Panic|shader stopped|does not parse|not mapped")
        if(Python3_FOUND)
            add_test(NAME check_deko3d_triangle
                     COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/check_triangle.py
                             ${fixture_dir}/triangle/frame_0002.ppm)
            set_tests_properties(check_deko3d_triangle PROPERTIES
                DEPENDS smoke_deko3d_triangle
                PASS_REGULAR_EXPRESSION "0 differ by more than 1")
        endif()
    endif()

    # GPU milestone M3b: a 3D cube from a vertex buffer, transformed by matrices in a
    # uniform buffer loaded through the command stream (LOAD_CONSTANT_BUFFER), drawn as
    # quads with back-face culling, compared with an independent double-precision
    # reference.
    set(cube_dir ${CMAKE_SOURCE_DIR}/examples/libnx_deko3d_cube)
    if(EXISTS ${cube_dir}/build/deko3d_cube.nso AND EXISTS ${cube_dir}/romfs/shaders)
        file(MAKE_DIRECTORY ${fixture_dir}/cube)
        add_test(NAME smoke_deko3d_cube
                 COMMAND hosrun --romfs ${cube_dir}/romfs --sdmc ${fixture_dir}/sdmc
                         ${cube_dir}/build/deko3d_cube.nso)
        set_tests_properties(smoke_deko3d_cube PROPERTIES
            TIMEOUT 300
            ENVIRONMENT "HOS_FRAMES=${fixture_dir}/cube"
            PASS_REGULAR_EXPRESSION "cube scene ready.*1 draws, 6 triangles.*deko3d cube: done"
            FAIL_REGULAR_EXPRESSION "FAIL|fatal|Panic|shader stopped|does not parse|not mapped|result [1-9]")
        if(Python3_FOUND)
            add_test(NAME check_deko3d_cube
                     COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/check_cube.py
                             ${fixture_dir}/cube/frame_0002.ppm)
            set_tests_properties(check_deko3d_cube PROPERTIES
                DEPENDS smoke_deko3d_cube
                PASS_REGULAR_EXPRESSION "6 front triangles.*0 differ by more than 2")
        endif()
    endif()

    # GPU milestone M3c: a Z24S8 depth buffer (test LESS, write on/off per draw) and
    # alpha blending, four quads with their own state, against a reference simulation.
    set(db_dir ${CMAKE_SOURCE_DIR}/examples/libnx_deko3d_depthblend)
    if(EXISTS ${db_dir}/build/deko3d_depthblend.nso AND EXISTS ${db_dir}/romfs/shaders)
        file(MAKE_DIRECTORY ${fixture_dir}/depthblend)
        add_test(NAME smoke_deko3d_depthblend
                 COMMAND hosrun --romfs ${db_dir}/romfs --sdmc ${fixture_dir}/sdmc
                         ${db_dir}/build/deko3d_depthblend.nso)
        set_tests_properties(smoke_deko3d_depthblend PROPERTIES
            TIMEOUT 300
            ENVIRONMENT "HOS_FRAMES=${fixture_dir}/depthblend"
            PASS_REGULAR_EXPRESSION "depth/blend scene ready.*4 draws, 8 triangles.*deko3d depth/blend: done"
            FAIL_REGULAR_EXPRESSION "FAIL|fatal|Panic|shader stopped|does not parse|not mapped|not supported|result [1-9]")
        if(Python3_FOUND)
            add_test(NAME check_deko3d_depthblend
                     COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/check_depthblend.py
                             ${fixture_dir}/depthblend/frame_0002.ppm)
            set_tests_properties(check_deko3d_depthblend PROPERTIES
                DEPENDS smoke_deko3d_depthblend
                PASS_REGULAR_EXPRESSION "0 differ by more than 1")
        endif()
    endif()

    # DOOM (doomgeneric + shareware IWAD): a complete game through framebuffer, input,
    # romfs and timing. 400 frames headless; frames are not dumped to keep it fast.
    set(doom_nso ${CMAKE_SOURCE_DIR}/examples/doom/build/doom_test.nso)
    set(doom_romfs ${CMAKE_SOURCE_DIR}/examples/doom/romfs)
    if(EXISTS ${doom_nso} AND EXISTS ${doom_romfs}/doom1.wad)
        file(WRITE ${fixture_dir}/doom_input.txt "2500 2000\n4500 2001\n6000 4000\n8000 0\n")
        add_test(NAME smoke_doom
                 COMMAND hosrun --romfs ${doom_romfs} --sdmc ${fixture_dir}/sdmc ${doom_nso})
        set_tests_properties(smoke_doom PROPERTIES
            TIMEOUT 900
            ENVIRONMENT "HOS_INPUT_SCRIPT=${fixture_dir}/doom_input.txt"
            PASS_REGULAR_EXPRESSION "doom: DOOM Shareware.*doom: frame 400.*doom: done after 400 frames"
            FAIL_REGULAR_EXPRESSION "FAIL |fatal|svcBreak Panic")
    endif()

    # Quake (quakegeneric + shareware pak): 3D software renderer, 18 MiB romfs, demo loop.
    set(quake_nso ${CMAKE_SOURCE_DIR}/examples/quake/build/quake_test.nso)
    set(quake_romfs ${CMAKE_SOURCE_DIR}/examples/quake/romfs)
    if(EXISTS ${quake_nso} AND EXISTS ${quake_romfs}/id1/pak0.pak)
        add_test(NAME smoke_quake
                 COMMAND hosrun --romfs ${quake_romfs} --sdmc ${fixture_dir}/sdmc ${quake_nso})
        set_tests_properties(smoke_quake PROPERTIES
            TIMEOUT 900
            PASS_REGULAR_EXPRESSION "quake: frame 300.*quake: done after 300 frames"
            FAIL_REGULAR_EXPRESSION "FAIL |fatal|svcBreak Panic")
    endif()

    # SDL2 (ports/sdl2 software driver on devkitPro's SDL 2.28): window + software renderer,
    # queued audio through audout, controller events from the hid shared memory.
    set(sdl_test_nso ${CMAKE_SOURCE_DIR}/examples/sdl_test/build/sdl_test.nso)
    if(EXISTS ${sdl_test_nso})
        file(WRITE ${fixture_dir}/sdl_input.txt "400 1
600 0
1200 2
1400 0
")
        add_test(NAME smoke_sdl_test COMMAND hosrun --sdmc ${fixture_dir}/sdmc ${sdl_test_nso})
        set_tests_properties(smoke_sdl_test PROPERTIES
            TIMEOUT 300
            ENVIRONMENT "HOS_INPUT_SCRIPT=${fixture_dir}/sdl_input.txt"
            PASS_REGULAR_EXPRESSION "sdl: video driver Switch.*sdl: renderer software.*sdl: audio device.*controller button [01] down.*controller button [01] down.*sdl_test: done"
            FAIL_REGULAR_EXPRESSION "FAIL |fatal|svcBreak Panic")
    endif()

    # OpenTyrian: an unmodified SDL2 game (scripts/build_opentyrian.sh + the freeware
    # Tyrian 2.1 data). Skips the intro, opens a menu, then leaves on the applet exit
    # request the script sends at 9 s, like closing a title from the HOME menu.
    set(tyrian_nso ${CMAKE_SOURCE_DIR}/examples/tyrian/build/tyrian.nso)
    set(tyrian_romfs ${CMAKE_SOURCE_DIR}/examples/tyrian/romfs)
    if(EXISTS ${tyrian_nso} AND EXISTS ${tyrian_romfs}/data/tyrian.shp)
        file(WRITE ${fixture_dir}/tyrian_input.txt "500 0
3000 8
4500 1
6000 1
9000 exit
")
        add_test(NAME smoke_tyrian
                 COMMAND hosrun --romfs ${tyrian_romfs} --sdmc ${fixture_dir}/sdmc ${tyrian_nso})
        set_tests_properties(smoke_tyrian PROPERTIES
            TIMEOUT 300
            ENVIRONMENT "HOS_INPUT_SCRIPT=${fixture_dir}/tyrian_input.txt"
            PASS_REGULAR_EXPRESSION "Welcome to... >> OpenTyrian.*frame [0-9]+: 640x400.*exit requested; delivering AppletMessage_ExitRequest.*ISelfController::Exit"
            FAIL_REGULAR_EXPRESSION "ERROR: |FAIL |fatal|svcBreak Panic")
    endif()

    add_test(NAME smoke_kernel_test_info COMMAND switch2pc info ${kernel_test_nso})
    set_tests_properties(smoke_kernel_test_info PROPERTIES
        PASS_REGULAR_EXPRESSION "MOD0          at.*relocations   [1-9][0-9]* applied, 0 unresolved, 0 unsupported")
endif()

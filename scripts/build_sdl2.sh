#!/usr/bin/env bash
# Build SDL2 for the switch2pc runtime: devkitPro's Switch fork of SDL 2.28
# (branch switch-sdl-2.28-audout: audio over audout, joystick over hid, libnx
# threads/timers/filesystem) with its EGL video driver replaced by our
# software framebuffer driver from ports/sdl2/. SDL_CPUINFO is forced on: the
# Switch default turns it off, and without it SDL picks no audio format
# converters on ARMv8 (no scalar fallback), so any rate/format change fails.
# Static library, installed to
# third_party/sdl2-install. Run from Git Bash on Windows with devkitPro at
# C:\devkitPro.
#
#   git clone --depth 1 -b switch-sdl-2.28-audout https://github.com/devkitPro/SDL third_party/SDL
#   scripts/build_sdl2.sh
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
sdl="$root/third_party/SDL"
build="$root/third_party/SDL/build-switch2pc"
prefix="$root/third_party/sdl2-install"

dkp=${DEVKITPRO:-/c/devkitPro}
case "$(uname -s)" in
MINGW* | MSYS*) dkp_w=$(cygpath -m "$dkp") ;;
*) dkp_w=$dkp ;;
esac
export DEVKITPRO="$dkp_w"
cmake="$dkp/msys2/usr/bin/cmake"
make="$dkp/msys2/usr/bin/make"

if [ ! -d "$sdl/src/video/switch" ]; then
    echo "third_party/SDL is missing; clone branch switch-sdl-2.28-audout of devkitPro/SDL" >&2
    exit 1
fi

# Overlay our driver: replace the EGL video driver, drop the GLES glue.
cp "$root/ports/sdl2/SDL_switchvideo.c" "$sdl/src/video/switch/SDL_switchvideo.c"
cp "$root/ports/sdl2/SDL_switchvideo.h" "$sdl/src/video/switch/SDL_switchvideo.h"
rm -f "$sdl/src/video/switch/SDL_switchopengles.c" "$sdl/src/video/switch/SDL_switchopengles.h"

# No GL, no EGL, no nouveau in the NINTENDO_SWITCH section of CMakeLists.txt.
if ! grep -q "switch2pc-patched" "$sdl/CMakeLists.txt"; then
    sed -i \
        -e 's|^\(\s*\)set(SDL_VIDEO_OPENGL_EGL 1)|\1# switch2pc-patched: software video only|' \
        -e '/^\s*set(HAVE_VIDEO_OPENGLES TRUE)/d' \
        -e '/^\s*set(SDL_VIDEO_OPENGL_ES2 1)/d' \
        -e '/^\s*set(SDL_VIDEO_RENDER_OGL_ES2 1)/d' \
        -e '/^\s*set(SDL_VIDEO_OPENGL 1)/d' \
        -e '/^\s*set(HAVE_VIDEO_OPENGL TRUE)/d' \
        -e 's|list(APPEND EXTRA_LIBS EGL glapi drm_nouveau nx m)|list(APPEND EXTRA_LIBS nx m)|' \
        -e '/add_definitions(-DSDL_VIDEO_STATIC_ANGLE)/d' \
        "$sdl/CMakeLists.txt"
fi

# devkitPro's CMake lives in its own msys2 and wants POSIX paths there:
# /opt/devkitpro is C:\devkitPro, and /c/... is the Windows drive.
msys_bash="$dkp/msys2/usr/bin/bash.exe"
"$msys_bash" -lc "
    export DEVKITPRO=/opt/devkitpro
    cmake -S '$sdl' -B '$build' -G 'Unix Makefiles' \
        -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake \
        -DNINTENDO_SWITCH=ON -DSDL_PTHREADS=ON \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX='$prefix' \
        -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF -DSDL_TESTS=OFF \
        -DSDL_OPENGL=OFF -DSDL_OPENGLES=OFF -DSDL_RENDER_D3D=OFF -DSDL_VULKAN=OFF \
        -DSDL_OFFSCREEN=OFF -DSDL_DUMMYVIDEO=OFF -DSDL_DUMMYAUDIO=OFF \
        -DSDL_STATIC_PIC=ON -DSDL_CPUINFO=ON &&
    cmake --build '$build' -j \$(nproc) &&
    cmake --install '$build'
"
echo "installed SDL2 to $prefix"
ls "$prefix/lib"

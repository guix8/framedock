# Cross build for the Steam Frame (aarch64 SteamOS) on a PC: clang + lld against a sysroot copied from the headset
# (contrib/cross/README.md says how to make one). Use:
#   cmake -S . -B build-cross -G Ninja -DCMAKE_TOOLCHAIN_FILE=contrib/cross/aarch64-sysroot.cmake \
#         -DFRAME_SYSROOT=$HOME/frame-sysroot
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

if(NOT FRAME_SYSROOT)
  set(FRAME_SYSROOT "$ENV{FRAME_SYSROOT}")
endif()
if(NOT FRAME_SYSROOT OR NOT EXISTS "${FRAME_SYSROOT}/usr/include")
  message(FATAL_ERROR "FRAME_SYSROOT must point at a sysroot copied from the headset (see contrib/cross/README.md)")
endif()
set(FRAME_SYSROOT "${FRAME_SYSROOT}" CACHE PATH "Sysroot copied from the headset")
# try_compile projects read this file again without the cache: hand them the path
set(ENV{FRAME_SYSROOT} "${FRAME_SYSROOT}")
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES FRAME_SYSROOT)

set(CMAKE_SYSROOT "${FRAME_SYSROOT}")
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
# The headset's own triple, so clang finds its GCC (crt files, libstdc++ headers) under usr/lib/gcc in the sysroot
set(CMAKE_C_COMPILER_TARGET aarch64-unknown-linux-gnu)
set(CMAKE_CXX_COMPILER_TARGET aarch64-unknown-linux-gnu)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-fuse-ld=lld")

set(CMAKE_FIND_ROOT_PATH "${FRAME_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(ENV{PKG_CONFIG_SYSROOT_DIR} "${FRAME_SYSROOT}")
set(ENV{PKG_CONFIG_LIBDIR} "${FRAME_SYSROOT}/usr/lib/pkgconfig:${FRAME_SYSROOT}/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")
set(OPENVR_LIB_DIR "${FRAME_SYSROOT}/opt/steamvr/bin/linuxarm64" CACHE PATH "libopenvr_api.so in the sysroot")

# Tests run on the PC through qemu-user (apt install qemu-user); the sysroot also needs lib -> usr/lib (README)
find_program(FRAME_QEMU qemu-aarch64)
if(FRAME_QEMU)
  set(CMAKE_CROSSCOMPILING_EMULATOR "${FRAME_QEMU};-L;${FRAME_SYSROOT}")
endif()

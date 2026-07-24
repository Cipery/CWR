set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)
# LGPL: openal-soft must be dynamically linked so users can replace the implementation
if(PORT STREQUAL "openal-soft")
    set(VCPKG_LIBRARY_LINKAGE dynamic)
endif()

set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE
    "${CMAKE_CURRENT_LIST_DIR}/../toolchains/macos-arm64-clang.cmake"
)

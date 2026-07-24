file(GLOB_RECURSE POSEIDON_METAL_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

set(POSEIDON_METAL_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/EngineMetal.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/FrameRing.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/MetalContext.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/MetalFwd.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Private/MetalCppFirst.hpp
)

# These switches must be defined before MetalCppFirst.hpp is parsed, so keep
# them source-local instead of placing preprocessor directives above line 1.
set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/MetalCppImpl.cpp
    PROPERTIES COMPILE_DEFINITIONS
        "NS_PRIVATE_IMPLEMENTATION;CA_PRIVATE_IMPLEMENTATION;MTL_PRIVATE_IMPLEMENTATION"
)

add_library(PoseidonMetal STATIC
    ${POSEIDON_METAL_SOURCES}
    ${POSEIDON_METAL_HEADERS}
)

target_include_directories(PoseidonMetal
    PUBLIC
        ${CMAKE_SOURCE_DIR}/engine
    PRIVATE
        ${CMAKE_SOURCE_DIR}/thirdparty/metal-cpp
)

target_link_libraries(PoseidonMetal PUBLIC
    Poseidon
    "-framework Metal"
    "-framework QuartzCore"
    "-framework Foundation"
)

# metal-cpp's std::function completion-handler bridge is implemented with
# Clang blocks, while every source file remains ordinary C++17.
target_compile_options(PoseidonMetal PRIVATE -fblocks -Wno-error)
target_compile_definitions(PoseidonMetal PRIVATE
    $<$<CONFIG:Debug>:_DEBUG>
    METALCPP_SYMBOL_VISIBILITY_HIDDEN
)

set_target_properties(PoseidonMetal PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    OUTPUT_NAME "PoseidonMetal"
)

option(CWR_METAL_RUNTIME_SHADERS
    "Compile pre-expanded Metal source at runtime instead of staging a metallib"
    OFF
)

execute_process(
    COMMAND xcrun -sdk macosx metal --version
    RESULT_VARIABLE CWR_METAL_TOOLCHAIN_RESULT
    OUTPUT_VARIABLE CWR_METAL_TOOLCHAIN_OUTPUT
    ERROR_VARIABLE CWR_METAL_TOOLCHAIN_ERROR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE
)

set(_metal_shader_source "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/PoseidonShaders.metal")
set(_metal_shader_types "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/PoseidonShaderTypes.h")
set(_metal_shader_build_dir "${CMAKE_CURRENT_BINARY_DIR}/Shaders")
set(_metal_air "${_metal_shader_build_dir}/PoseidonShaders.air")
set(_metal_metallib "${_metal_shader_build_dir}/PoseidonShaders.metallib")
set(_staged_metallib "${DIST_DIR}/PoseidonShaders.metallib")
set(_staged_source "${DIST_DIR}/PoseidonShaders.metal")

if(CWR_METAL_RUNTIME_SHADERS)
    set(CWR_METAL_OFFLINE_SHADERS OFF CACHE INTERNAL
        "Whether PoseidonMetal stages an offline-compiled metallib" FORCE
    )
    target_compile_definitions(PoseidonMetal PRIVATE CWR_METAL_OFFLINE_SHADERS=0)
    message(WARNING
        "CWR_METAL_RUNTIME_SHADERS=ON: PoseidonMetal will compile staged MSL source at runtime"
    )

    add_custom_command(
        OUTPUT "${_staged_source}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_DIR}"
        COMMAND ${CMAKE_COMMAND} -E rm -f "${_staged_metallib}"
        COMMAND ${CMAKE_COMMAND}
            "-DSHADER_TYPES=${_metal_shader_types}"
            "-DSHADER_SOURCE=${_metal_shader_source}"
            "-DSTAGED_SOURCE=${_staged_source}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/ExpandRuntimeShaders.cmake"
        DEPENDS
            "${_metal_shader_source}"
            "${_metal_shader_types}"
            "${CMAKE_CURRENT_SOURCE_DIR}/ExpandRuntimeShaders.cmake"
        COMMENT "Staging self-contained Poseidon Metal source for runtime compilation"
        VERBATIM
    )
    add_custom_target(PoseidonMetalShaders ALL DEPENDS "${_staged_source}")
elseif(CWR_METAL_TOOLCHAIN_RESULT EQUAL 0)
    set(CWR_METAL_OFFLINE_SHADERS ON CACHE INTERNAL
        "Whether PoseidonMetal stages an offline-compiled metallib" FORCE
    )
    target_compile_definitions(PoseidonMetal PRIVATE CWR_METAL_OFFLINE_SHADERS=1)
    message(STATUS "Metal shader toolchain found: ${CWR_METAL_TOOLCHAIN_OUTPUT}")

    add_custom_command(
        OUTPUT "${_staged_metallib}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_metal_shader_build_dir}" "${DIST_DIR}"
        COMMAND xcrun -sdk macosx metal
            -std=metal3.2
            "$<$<CONFIG:Debug>:-frecord-sources>"
            "$<$<CONFIG:Debug>:-gline-tables-only>"
            -I "${CMAKE_CURRENT_SOURCE_DIR}/Shaders"
            -o "${_metal_air}"
            -c "${_metal_shader_source}"
        COMMAND xcrun -sdk macosx metallib
            "${_metal_air}"
            -o "${_metal_metallib}"
        COMMAND ${CMAKE_COMMAND} -E rm -f "${_staged_source}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_metal_metallib}"
            "${_staged_metallib}"
        DEPENDS "${_metal_shader_source}" "${_metal_shader_types}"
        COMMENT "Compiling and staging PoseidonShaders.metallib"
        COMMAND_EXPAND_LISTS
        VERBATIM
    )
    add_custom_target(PoseidonMetalShaders ALL DEPENDS "${_staged_metallib}")
else()
    message(FATAL_ERROR
        "The Metal shader toolchain is unavailable (${CWR_METAL_TOOLCHAIN_ERROR}). "
        "Install it with `xcodebuild -downloadComponent MetalToolchain`, or configure "
        "with -DCWR_METAL_RUNTIME_SHADERS=ON to use runtime shader compilation."
    )
endif()

unset(_metal_shader_source)
unset(_metal_shader_types)
unset(_metal_shader_build_dir)
unset(_metal_air)
unset(_metal_metallib)
unset(_staged_metallib)
unset(_staged_source)

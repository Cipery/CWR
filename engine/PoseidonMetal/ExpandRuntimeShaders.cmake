if(NOT DEFINED SHADER_TYPES OR NOT DEFINED SHADER_SOURCE OR NOT DEFINED STAGED_SOURCE)
    message(FATAL_ERROR "SHADER_TYPES, SHADER_SOURCE, and STAGED_SOURCE are required")
endif()

file(READ "${SHADER_TYPES}" _shader_types)
file(READ "${SHADER_SOURCE}" _shader_source)

set(_include_marker "#include \"PoseidonShaderTypes.h\"")
string(FIND "${_shader_source}" "${_include_marker}" _include_offset)
if(_include_offset EQUAL -1)
    message(FATAL_ERROR "${SHADER_SOURCE} does not contain ${_include_marker}")
endif()

string(REPLACE "${_include_marker}" "${_shader_types}" _expanded_source "${_shader_source}")
get_filename_component(_staged_dir "${STAGED_SOURCE}" DIRECTORY)
file(MAKE_DIRECTORY "${_staged_dir}")
file(WRITE "${STAGED_SOURCE}" "${_expanded_source}")

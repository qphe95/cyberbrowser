# EmbedShaders.cmake
# Convert SPIR-V binary files into a C source file with embedded arrays.
#
# Usage:
#   embed_shaders(
#       OUTPUT_HEADER <path>
#       OUTPUT_SOURCE <path>
#       SHADERS <file1> <file2> ...
#   )

function(embed_shaders)
    set(options)
    set(oneValueArgs OUTPUT_HEADER OUTPUT_SOURCE)
    set(multiValueArgs SHADERS)
    cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT ARG_OUTPUT_HEADER OR NOT ARG_OUTPUT_SOURCE OR NOT ARG_SHADERS)
        message(FATAL_ERROR "embed_shaders: missing required arguments")
    endif()

    set(header_content
"#ifndef EMBEDDED_SHADERS_H
#define EMBEDDED_SHADERS_H

#include <stddef.h>
#include <stdint.h>

")

    set(source_content
"#include \"embedded_shaders.h\"

")

    foreach(spv_path ${ARG_SHADERS})
        if(NOT EXISTS ${spv_path})
            message(FATAL_ERROR "embed_shaders: file not found: ${spv_path}")
        endif()

        get_filename_component(basename ${spv_path} NAME)
        # Sanitize to a valid C identifier
        string(REGEX REPLACE "[^a-zA-Z0-9]" "_" c_name ${basename})
        set(array_name "${c_name}")
        set(len_name "${c_name}_len")

        string(APPEND header_content
"extern const uint8_t ${array_name}[];
extern const size_t ${len_name};

")

        # Read file as raw hex string
        file(READ ${spv_path} hex_content HEX)
        string(LENGTH "${hex_content}" hex_len)
        math(EXPR byte_count "${hex_len} / 2")
        math(EXPR last_byte_idx "${byte_count} - 1")

        string(APPEND source_content "const uint8_t ${array_name}[] = {\n")

        # Break into lines of up to 12 bytes (24 hex chars)
        set(line_count 0)
        set(current_line "    ")
        foreach(byte_idx RANGE 0 ${last_byte_idx})
            math(EXPR hex_idx "${byte_idx} * 2")
            string(SUBSTRING "${hex_content}" ${hex_idx} 2 hex_byte)
            string(TOLOWER "${hex_byte}" hex_byte)

            if(line_count EQUAL 0)
                string(APPEND current_line "0x${hex_byte}")
            else()
                string(APPEND current_line ", 0x${hex_byte}")
            endif()
            math(EXPR line_count "${line_count} + 1")

            if(line_count EQUAL 12)
                string(APPEND source_content "${current_line},\n")
                set(current_line "    ")
                set(line_count 0)
            endif()
        endforeach()

        if(line_count GREATER 0)
            string(APPEND source_content "${current_line},\n")
        endif()

        string(APPEND source_content
"};
const size_t ${len_name} = sizeof(${array_name});

")
    endforeach()

    string(APPEND header_content
"#endif /* EMBEDDED_SHADERS_H */
")

    file(WRITE ${ARG_OUTPUT_HEADER} "${header_content}")
    file(WRITE ${ARG_OUTPUT_SOURCE} "${source_content}")
endfunction()

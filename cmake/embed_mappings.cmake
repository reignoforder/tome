# SPDX-License-Identifier: GPL-3.0-or-later
# tome_embed_mappings(<output.cpp> <dir>): writes a source file defining
# tome::mappings::kEmbeddedFiles with the text of every <dir>/*.json.
# CMake re-runs when a file is added, removed or edited.
function(tome_embed_mappings output dir)
    file(GLOB files CONFIGURE_DEPENDS "${dir}/*.json")
    list(SORT files)
    # The glob only notices added or removed files; this makes edits re-run CMake too.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${files})
    set(entries "")
    foreach(path IN LISTS files)
        get_filename_component(name "${path}" NAME)
        file(READ "${path}" text)
        string(FIND "${text}" ")tome_json\"" clash)
        if(NOT clash EQUAL -1)
            message(FATAL_ERROR "${path} contains the raw-string delimiter )tome_json\"")
        endif()
        string(APPEND entries "    {\"${name}\", R\"tome_json(${text})tome_json\"},\n")
    endforeach()
    list(LENGTH files count)
    if(count EQUAL 0)
        message(FATAL_ERROR "No mapping files in ${dir}")
    endif()
    # Built with APPEND: a list would split the text at every ';'.
    set(content "// Generated from ${dir} by cmake/embed_mappings.cmake. Do not edit.\n")
    string(APPEND content "#include \"core/embedded_mappings.h\"\n\nnamespace tome::mappings {\n\n")
    string(APPEND content "extern const EmbeddedFile kEmbeddedFiles[] = {\n${entries}};\n")
    string(APPEND content "extern const size_t kEmbeddedFileCount = ${count};\n\n} // namespace tome::mappings\n")
    # configure_file only touches the output when it changes, so unrelated
    # reconfigures don't rebuild anything.
    file(WRITE "${output}.tmp" "${content}")
    configure_file("${output}.tmp" "${output}" COPYONLY)
endfunction()

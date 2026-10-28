tome_add_mod(hello
    SOURCES hello.c
    METADATA tome.mod.json
    FILES ui/hello.fdf config.schema.json
)

# Its icon (tome.mod.json "icon"): a generated texture, since image files
# aren't kept in the repository. Without Python the mod just has no icon.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
    add_custom_command(TARGET hello POST_BUILD
        COMMAND ${Python3_EXECUTABLE} -I "${CMAKE_SOURCE_DIR}/tests/e2e/make_blp.py"
            "${CMAKE_BINARY_DIR}/mods/hello/icon.blp" 32 3070d0
    )
    set(hello_icon icon.blp)
endif()

# The same mod as one file: <build>/packages/hello.tome.
tome_package(hello FILES ${hello_icon})

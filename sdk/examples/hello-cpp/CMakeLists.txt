# SPDX-License-Identifier: MIT
tome_add_mod(hello_cpp
    SOURCES hello_cpp.cpp
    METADATA tome.mod.json
    FOLDER hello-cpp
    FILES ui/hello_cpp.fdf config.schema.json
)
set_target_properties(hello_cpp PROPERTIES OUTPUT_NAME hello-cpp)
# tome.hpp needs C++17; built as that, so the header stays usable there.
set_target_properties(hello_cpp PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(hello_cpp PRIVATE -Wall -Wextra -Wpedantic)

tome_package(hello_cpp)

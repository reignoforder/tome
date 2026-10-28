# The MinGW toolchain lives in the SDK, so mods built outside this repo use
# the same one: sdk/cmake/toolchain-i686-mingw.cmake.
include("${CMAKE_CURRENT_LIST_DIR}/../sdk/cmake/toolchain-i686-mingw.cmake")

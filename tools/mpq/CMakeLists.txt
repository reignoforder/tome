# tome-mpq: a small MPQ utility for tests and development (list, extract,
# replace files in maps). Built on StormLib (MIT), fetched at configure time.
include(FetchContent)

FetchContent_Declare(stormlib
    GIT_REPOSITORY https://github.com/ladislav-zezula/StormLib.git
    GIT_TAG 86f9b99ffe4d3417dad16d00541cf6f2e3d7bf79  # v9.30
)
set(STORM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(stormlib)

add_executable(tome-mpq tome_mpq.cpp)
target_link_libraries(tome-mpq PRIVATE storm)
target_link_options(tome-mpq PRIVATE -static)

# SPDX-License-Identifier: GPL-3.0-or-later
# Packs the player download (the dist target in loader/CMakeLists.txt):
# cmake -DMIX=<tome.mix> -DOUT=<zip> -DVERSION=<version> -DSOURCE=<Tome's source> -P dist.cmake
#
# Laid out as it goes into the game folder: tome.mix next to war3.exe, and
# tome/ with the readme (loader/dist/README.txt), DISCLAIMER.txt, licenses/
# (the GPL, loader/third_party's texts) and an empty mods/. Nothing in the
# zip's top level but those two, so unpacking it into the game folder
# overwrites none of the game's files. Text files get Windows line endings,
# for Notepad before Windows 10.
foreach(var MIX OUT VERSION SOURCE)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "dist.cmake: -D${var} is missing")
    endif()
endforeach()
get_filename_component(stage "${OUT}" DIRECTORY)
set(stage "${stage}/dist")
file(REMOVE_RECURSE "${stage}")
file(MAKE_DIRECTORY "${stage}/tome/mods")
file(COPY "${MIX}" DESTINATION "${stage}")

# <from> to tome/<to> with CRLF line endings (and @TOME_VERSION@ filled in).
function(text from to)
    file(READ "${from}" content)
    string(REPLACE "@TOME_VERSION@" "${VERSION}" content "${content}")
    string(REPLACE "\r\n" "\n" content "${content}")
    string(REPLACE "\n" "\r\n" content "${content}")
    file(WRITE "${stage}/tome/${to}" "${content}")
endfunction()
text("${SOURCE}/loader/dist/README.txt" README.txt)
text("${SOURCE}/DISCLAIMER.md" DISCLAIMER.txt)
text("${SOURCE}/LICENSE" licenses/GPL-3.0.txt)
include("${SOURCE}/loader/third_party/licenses.cmake")
set(texts "")
tome_copy_licenses("${stage}/tome/licenses" texts)
foreach(t IN LISTS texts)
    text("${stage}/tome/licenses/${t}" "licenses/${t}")
endforeach()

include("${CMAKE_CURRENT_LIST_DIR}/repro_zip.cmake")

reproducible_zip("${OUT}" "${stage}" tome.mix tome)
file(SHA256 "${OUT}" hash)
get_filename_component(name "${OUT}" NAME)
file(WRITE "${OUT}.sha256" "${hash}  ${name}\n")
message(STATUS "Player download: ${OUT} (SHA-256 ${hash})")

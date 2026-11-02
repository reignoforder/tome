# SPDX-License-Identifier: GPL-3.0-or-later
# Runs a command and checks its output holds every expected text, in any
# order (a single PASS_REGULAR_EXPRESSION with all of them backtracks badly).
# cmake "-DCOMMAND=<program;args>" "-DEXPECT=<text;text...>" -P expect_output.cmake
execute_process(COMMAND ${COMMAND} OUTPUT_VARIABLE out ERROR_VARIABLE err)
set(all "${out}${err}")
foreach(text IN LISTS EXPECT)
    string(FIND "${all}" "${text}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "no \"${text}\" in the output:\n${all}")
    endif()
endforeach()

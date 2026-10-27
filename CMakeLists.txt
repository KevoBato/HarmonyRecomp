# harmonyrecomp - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(harmonyrecomp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(HARMONYRECOMP_SOURCES
    src/main.cpp
    src/graphics_menu.cpp
    src/crash_log.cpp
    src/dlc.cpp
    src/game_fixes.cpp
    src/netplay/gamesock.cpp
    src/netplay/hooks.cpp
    src/netplay/lobby.cpp
    src/netplay/netplay.cpp
    src/netplay/sessions.cpp
    src/netplay/udp.cpp
)

if(WIN32)
    add_executable(harmonyrecomp WIN32 ${HARMONYRECOMP_SOURCES})
else()
    add_executable(harmonyrecomp ${HARMONYRECOMP_SOURCES})
endif()

rexglue_setup_target(harmonyrecomp)

# Netplay lobby service (src/netplay/udp.cpp): Winsock and adapter enumeration.
if(WIN32)
    target_link_libraries(harmonyrecomp PRIVATE ws2_32 iphlpapi)
    # Symbol map, to resolve the offsets in logs/crash_*.txt.
    target_link_options(harmonyrecomp PRIVATE "LINKER:/MAP:harmonyrecomp.map")
endif()

# Move the main module's indirect-call table out of the way of the player DLLs.
# By default it sits right after the image (0x82960000) and is ~13 MB, which
# would cover 0x83000000-0x835F0000 where the game loads dllAlucard.dll and the
# other characters. 0x8C000000-0x8CD5E990 is free in the 0x80000000 guest heap.
# Needs the table_base SDK change applied by scripts/build.py.
target_compile_definitions(harmonyrecomp PRIVATE REX_FUNC_TABLE_BASE=0x8C000000ull)

# Give every player DLL its own indirect-call table slot as well. By default a
# DLL's table sits right after its image, which overlaps the next character's
# fixed load address (Alucard 0x83000000, Soma 0x83090000, ...). Slots are
# 1 MB apart from 0x8D000000; each table is about 0x60000 bytes. The six base
# characters keep fixed slots; DLC characters (see dlc/ in the README) follow
# in the order codegen lists them.
# Needs SDK change 5 from scripts/build.py (ReXModule_TableBase).
set(_harmony_dll_modules Alucard Soma Jonathan Charlotte Shanoa AxeArmor)
set(_harmony_dll_targets_file "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/dll_targets.cmake")
if(EXISTS "${_harmony_dll_targets_file}")
    file(STRINGS "${_harmony_dll_targets_file}" _harmony_dll_lines REGEX "^# DLL module: dll")
    foreach(_harmony_line ${_harmony_dll_lines})
        string(REGEX REPLACE "^# DLL module: dll" "" _harmony_name "${_harmony_line}")
        string(STRIP "${_harmony_name}" _harmony_name)
        list(FIND _harmony_dll_modules "${_harmony_name}" _harmony_index)
        if(_harmony_index EQUAL -1)
            list(APPEND _harmony_dll_modules "${_harmony_name}")
        endif()
    endforeach()
endif()
set(_harmony_dll_slot 0)
foreach(_harmony_dll ${_harmony_dll_modules})
    if(TARGET harmonyrecomp_dll${_harmony_dll})
        math(EXPR _harmony_table "0x8D000000 + ${_harmony_dll_slot} * 0x100000"
             OUTPUT_FORMAT HEXADECIMAL)
        target_compile_definitions(harmonyrecomp_dll${_harmony_dll} PRIVATE
            REX_FUNC_TABLE_BASE=${_harmony_table}ull)
    endif()
    math(EXPR _harmony_dll_slot "${_harmony_dll_slot} + 1")
endforeach()

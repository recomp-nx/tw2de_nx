# tw2de (2026-11-09): makes one DLL module's partially linked object fit into the executable beside the
# entrypoint and the other modules (cmake -P, from dll_targets.cmake).
#
# A game's DLLs are recompiled like its executable: the same function names (sub_<address>, which repeat between
# modules that share a base address), the same PPCImageConfig / PPCFuncMappings tables. Every symbol the module
# defines for itself (strong: code T, data D/B/R/G/S; and the generated functions, which are weak W) becomes local to it; the entry points get the module's suffix
# (ReXModule_Register_<symbol>, ReXModule_GetImageInfo_<symbol>, which the generated module registry calls). Vague
# linkage (C++ inline functions and variables, templates: mangled W, V, u) stays global, so those merge with the executable's own
# copies and share their state.
#
# Variables: NM, OBJCOPY, IN_OBJ, OUT_OBJ, SYMBOL.
foreach(_v NM OBJCOPY IN_OBJ OUT_OBJ SYMBOL)
  if(NOT DEFINED ${_v})
    message(FATAL_ERROR "rexglue_static_module: ${_v} not set")
  endif()
endforeach()
execute_process(COMMAND "${NM}" --defined-only -g "${IN_OBJ}" OUTPUT_VARIABLE _nm RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "rexglue_static_module: ${NM} failed on ${IN_OBJ}")
endif()
string(REPLACE "\n" ";" _lines "${_nm}")
set(_local "")
set(_count 0)
foreach(_l IN LISTS _lines)
  if(_l MATCHES "^[0-9a-fA-F]* ([TDBRGSW]) (.+)$")
    set(_type "${CMAKE_MATCH_1}")
    set(_name "${CMAKE_MATCH_2}")
    # The generated functions are weak (sub_<address>, __imp__sub_..., the __savegprlr_ / __restfpr_ helpers, so the
    # app's hooks can replace them): those are the module's own too. A weak C++ name (_Z..., inline functions and
    # templates) stays global.
    if(_type STREQUAL "W" AND (_name MATCHES "^_Z" OR _name MATCHES "^DW\\.ref\\."))
      continue()
    endif()
    if(_name STREQUAL "ReXModule_Register" OR _name STREQUAL "ReXModule_GetImageInfo")
      continue()
    endif()
    string(APPEND _local "${_name}\n")
    math(EXPR _count "${_count} + 1")
  endif()
endforeach()
file(WRITE "${OUT_OBJ}.local.txt" "${_local}")
execute_process(COMMAND "${OBJCOPY}" "--localize-symbols=${OUT_OBJ}.local.txt"
                        --redefine-sym "ReXModule_Register=ReXModule_Register_${SYMBOL}"
                        --redefine-sym "ReXModule_GetImageInfo=ReXModule_GetImageInfo_${SYMBOL}"
                        "${IN_OBJ}" "${OUT_OBJ}" RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "rexglue_static_module: ${OBJCOPY} failed on ${IN_OBJ}")
endif()
message(STATUS "rexglue_static_module: ${SYMBOL}: ${_count} symbols made local")

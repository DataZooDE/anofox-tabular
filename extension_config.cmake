# This file is included by DuckDB's build system. It specifies which extension to load

# GCC 14 + DuckDB v1.5.2: plan_serializer and other DuckDB tools that link both
# duckdb_static (non-COMDAT constexpr defs for C++11 compat) and our extension objects
# (COMDAT constexpr defs from C++17 headers) hit a "multiple definition" error.
# Setting DUCKDB_EXTRA_LINK_FLAGS here propagates to all DuckDB tool link commands
# (plan_serializer, shell, etc.) which include this variable.
if(UNIX AND NOT APPLE)
    list(APPEND DUCKDB_EXTRA_LINK_FLAGS -Wl,--allow-multiple-definition)
    # CMAKE_SHARED_LINKER_FLAGS / CMAKE_EXE_LINKER_FLAGS propagate to all shared
    # libraries (libduckdb.so) and executables (test/unittest) that link both
    # duckdb_static (non-COMDAT constexpr defs, C++11) and our extension archive
    # (COMDAT defs, C++17), triggering a GCC 14 "multiple definition" error.
    # DUCKDB_EXTRA_LINK_FLAGS only reaches plan_serializer and the DuckDB shell.
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--allow-multiple-definition")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--allow-multiple-definition")
endif()

duckdb_extension_load(anofox_tabular
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS)

duckdb_extension_load(json)

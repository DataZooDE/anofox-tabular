# This file is included by DuckDB's build system. It specifies which extension to load

# GCC 14 + DuckDB v1.5.2: any target that links both duckdb_static (non-COMDAT
# constexpr defs compiled under C++11 compat mode) and our extension objects
# (COMDAT constexpr defs from C++17 headers) hits a "multiple definition" linker
# error.  We must suppress it for every artifact DuckDB produces:
#
#   CMAKE_SHARED_LINKER_FLAGS  → libduckdb.so
#   CMAKE_EXE_LINKER_FLAGS     → test/unittest, duckdb CLI
#   DUCKDB_EXTRA_LINK_FLAGS    → plan_serializer, shell (DuckDB-specific variable)
#
# This file is include()-d from DuckDB's root CMakeLists.txt before
# add_subdirectory(src/test/tools), so the flag reaches all targets.
if(UNIX AND NOT APPLE)
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--allow-multiple-definition")
    set(CMAKE_EXE_LINKER_FLAGS   "${CMAKE_EXE_LINKER_FLAGS}    -Wl,--allow-multiple-definition")
    list(APPEND DUCKDB_EXTRA_LINK_FLAGS -Wl,--allow-multiple-definition)
endif()

duckdb_extension_load(anofox_tabular
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS)

duckdb_extension_load(json)

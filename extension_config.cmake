# This file is included by DuckDB's build system. It specifies which extension to load

# GCC 14 + DuckDB v1.5.x: plan_serializer and other DuckDB tools that link both
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

# Extension version: the release tag (vYYYY.MM.DD) when the commit has one, else the commit hash.
# DuckDB derives it the same way, but v1.5.6 quotes the tag pattern literally
# (--match "'v*.*.*'"), finds no tag and always stamps the hash; DuckDB 2.0 is not affected.
set(ANOFOX_TABULAR_EXT_VERSION "")
find_package(Git QUIET)
if(Git_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --always --match "v*.*.*"
        WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
        RESULT_VARIABLE ANOFOX_GIT_RESULT
        OUTPUT_VARIABLE ANOFOX_GIT_DESCRIBE
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(ANOFOX_GIT_RESULT EQUAL 0 AND ANOFOX_GIT_DESCRIBE MATCHES "^v[0-9]+\\.[0-9]+\\.[0-9]+(-[a-zA-Z0-9\\.]+)?$")
        set(ANOFOX_TABULAR_EXT_VERSION "${ANOFOX_GIT_DESCRIBE}")
    endif()
endif()

if(ANOFOX_TABULAR_EXT_VERSION STREQUAL "")
    # No release tag on this commit: DuckDB stamps the commit hash
    duckdb_extension_load(anofox_tabular
        SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
        LOAD_TESTS)
else()
    message(STATUS "anofox_tabular extension version ${ANOFOX_TABULAR_EXT_VERSION}")
    duckdb_extension_load(anofox_tabular
        SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
        EXTENSION_VERSION ${ANOFOX_TABULAR_EXT_VERSION}
        LOAD_TESTS)
endif()

duckdb_extension_load(json)

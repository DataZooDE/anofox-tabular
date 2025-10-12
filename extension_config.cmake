# This file is included by DuckDB's build system. It specifies which extension to load

duckdb_extension_load(anofox_tabular
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS)

duckdb_extension_load(json)
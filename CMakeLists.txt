cmake_minimum_required(VERSION 3.10...3.29)

set(TARGET_NAME anofox_tabular)
set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME} LANGUAGES C CXX)
include_directories(src/include)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Set CMake policy to suppress FindBoost warning
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()

# --- Dependencies ---

# Lightweight deps via vcpkg
include(CMakeFindDependencyMacro)
find_package(spdlog CONFIG REQUIRED)
find_package(c-ares CONFIG REQUIRED)

# Libphonenumber: Custom implementation (no external dependency)

# Libpostal via pkg-config (system-installed, CI preferred)
find_package(PkgConfig REQUIRED)

# First, check for Alpine/system package installation (most common in CI)
# Alpine libpostal-dev package contents:
#   /usr/include/libpostal/libpostal.h
#   /usr/lib/libpostal.a
#   /usr/lib/libpostal.so -> libpostal.so.1
#   /usr/lib/pkgconfig/libpostal.pc
# Prefer static library (.a) to avoid runtime dependency issues in Docker CI
if(EXISTS "/usr/lib/libpostal.a" AND EXISTS "/usr/include/libpostal/libpostal.h")
    message(STATUS "Found libpostal via Alpine/system package (static library)")
    set(LIBPOSTAL_LDFLAGS "/usr/lib/libpostal.a")
    set(LIBPOSTAL_INCLUDE_DIRS "/usr/include")
    set(LIBPOSTAL_FOUND TRUE)
# Check for source build in /usr/local (Ubuntu CI builds from source)
elseif(EXISTS "/usr/local/lib/libpostal.a" AND EXISTS "/usr/local/include/libpostal/libpostal.h")
    message(STATUS "Found libpostal in /usr/local (static library)")
    set(LIBPOSTAL_LDFLAGS "/usr/local/lib/libpostal.a")
    set(LIBPOSTAL_INCLUDE_DIRS "/usr/local/include")
    set(LIBPOSTAL_FOUND TRUE)
# Check for local build (from configure_ci)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libpostal-install/lib/libpostal.a")
    message(STATUS "Found libpostal in local install (static library)")
    set(LIBPOSTAL_LDFLAGS "${CMAKE_CURRENT_SOURCE_DIR}/libpostal-install/lib/libpostal.a")
    set(LIBPOSTAL_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/libpostal-install/include")
    set(LIBPOSTAL_FOUND TRUE)
else()
    # Try pkg-config as last resort
    pkg_check_modules(LIBPOSTAL libpostal)

    if(LIBPOSTAL_FOUND)
        message(STATUS "Found libpostal via pkg-config")
    else()
        # Last resort: try to install libpostal using the install script
        message(STATUS "libpostal not found, attempting to install via script...")
        execute_process(
            COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/scripts/install-deps-ci.sh
            RESULT_VARIABLE INSTALL_RESULT
            OUTPUT_VARIABLE INSTALL_OUTPUT
            ERROR_VARIABLE INSTALL_ERROR
        )

        if(INSTALL_RESULT EQUAL 0)
            message(STATUS "Installation script completed, rechecking for libpostal...")
            # Check again after installation (prefer static library)
            if(EXISTS "/usr/lib/libpostal.a" AND EXISTS "/usr/include/libpostal/libpostal.h")
                message(STATUS "Found libpostal after installation (Alpine, static library)")
                set(LIBPOSTAL_LDFLAGS "/usr/lib/libpostal.a")
                set(LIBPOSTAL_INCLUDE_DIRS "/usr/include")
                set(LIBPOSTAL_FOUND TRUE)
            elseif(EXISTS "/usr/local/lib/libpostal.a" AND EXISTS "/usr/local/include/libpostal/libpostal.h")
                message(STATUS "Found libpostal after installation (/usr/local, static library)")
                set(LIBPOSTAL_LDFLAGS "/usr/local/lib/libpostal.a")
                set(LIBPOSTAL_INCLUDE_DIRS "/usr/local/include")
                set(LIBPOSTAL_FOUND TRUE)
            else()
                message(FATAL_ERROR "libpostal installation script succeeded but library still not found")
            endif()
        else()
            message(FATAL_ERROR "libpostal not found and installation script failed: ${INSTALL_ERROR}")
        endif()
    endif()
endif()


# --- Build ---

set(EXTENSION_SOURCES
        src/anofox_tabular_extension.cpp
        src/anofox_diff.cpp
        src/anofox_postal.cpp
        src/anofox_phonenumber.cpp
        src/anofox_phonenumber_metadata.cpp
        src/anofox_email.cpp
        src/anofox_email_dns.cpp
        src/anofox_email_smtp.cpp
        src/anofox_metric.cpp
        src/anofox_isolation_forest.cpp
        src/anofox_dbscan.cpp
        src/anofox_money.cpp
        src/anofox_money_currency.cpp
        src/anofox_vat.cpp
        src/anofox_vat_country.cpp
)

add_library(anofox_trace_obj OBJECT src/anofox_trace.cpp)
set_property(TARGET anofox_trace_obj PROPERTY INCLUDE_DIRECTORIES "")
target_include_directories(anofox_trace_obj PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/include)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

target_sources(${EXTENSION_NAME} PRIVATE $<TARGET_OBJECTS:anofox_trace_obj>)
target_sources(${LOADABLE_EXTENSION_NAME} PRIVATE $<TARGET_OBJECTS:anofox_trace_obj>)


# --- Linking ---

# Link dependencies
target_link_libraries(${EXTENSION_NAME}
    c-ares::cares
    spdlog::spdlog_header_only
    ${LIBPOSTAL_LDFLAGS}
)

# Add include directories from dependencies
target_include_directories(${EXTENSION_NAME} PRIVATE
    ${LIBPOSTAL_INCLUDE_DIRS}
)
# spdlog configuration is handled by the header-only target
# No need to manually define SPDLOG_* flags; spdlog_header_only sets them correctly
get_target_property(SPDLOG_INTERFACE_INCLUDES spdlog::spdlog_header_only INTERFACE_INCLUDE_DIRECTORIES)
if(SPDLOG_INTERFACE_INCLUDES)
    list(GET SPDLOG_INTERFACE_INCLUDES 0 SPDLOG_PRIMARY_INCLUDE)
    if(SPDLOG_PRIMARY_INCLUDE)
        target_include_directories(${EXTENSION_NAME} BEFORE PRIVATE "${SPDLOG_PRIMARY_INCLUDE}/spdlog/fmt/bundled")
        target_include_directories(${LOADABLE_EXTENSION_NAME} BEFORE PRIVATE "${SPDLOG_PRIMARY_INCLUDE}/spdlog/fmt/bundled")
        target_include_directories(anofox_trace_obj PRIVATE "${SPDLOG_PRIMARY_INCLUDE}" "${SPDLOG_PRIMARY_INCLUDE}/spdlog/fmt/bundled" ${CMAKE_CURRENT_SOURCE_DIR}/src/include)
    endif()
endif()
target_link_libraries(${LOADABLE_EXTENSION_NAME}
    c-ares::cares
    spdlog::spdlog_header_only
    ${LIBPOSTAL_LDFLAGS}
)

# Add include directories from dependencies
target_include_directories(${LOADABLE_EXTENSION_NAME} PRIVATE
    ${LIBPOSTAL_INCLUDE_DIRS}
)
# --- Installation ---

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

install(
  TARGETS ${LOADABLE_EXTENSION_NAME}
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

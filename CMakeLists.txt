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
find_package(CURL CONFIG REQUIRED)

# Libphonenumber: Custom implementation (no external dependency)

# Libpostal via pkg-config (system-installed, CI preferred)
find_package(PkgConfig REQUIRED)

# Libpostal is installed by configure_ci (Makefile) before CMake runs.
# The install script (scripts/install-deps-ci.sh) builds from source with -fPIC
# and installs to /usr/local for both Alpine (musl) and Ubuntu (glibc).
#
# In multi-container CI workflows, configure_ci may run in one container and
# CMake in another, so we fallback to running the install script from CMake.

# Check for source build in /usr/local (standard install location)
if(EXISTS "/usr/local/lib/libpostal.a" AND EXISTS "/usr/local/include/libpostal/libpostal.h")
    message(STATUS "Found libpostal in /usr/local (static library with -fPIC)")
    set(LIBPOSTAL_LDFLAGS "/usr/local/lib/libpostal.a")
    set(LIBPOSTAL_INCLUDE_DIRS "/usr/local/include")
    set(LIBPOSTAL_FOUND TRUE)
# Fallback: Check Alpine system package location (if install script changes)
elseif(EXISTS "/usr/lib/libpostal.a" AND EXISTS "/usr/include/libpostal/libpostal.h")
    message(STATUS "Found libpostal in /usr/lib (static library)")
    set(LIBPOSTAL_LDFLAGS "/usr/lib/libpostal.a")
    set(LIBPOSTAL_INCLUDE_DIRS "/usr/include")
    set(LIBPOSTAL_FOUND TRUE)
else()
    # Try pkg-config
    pkg_check_modules(LIBPOSTAL libpostal)
    if(LIBPOSTAL_FOUND)
        message(STATUS "Found libpostal via pkg-config")
    else()
        # Last resort: run the install script from CMake
        # This handles multi-container CI where configure_ci runs in different container
        message(STATUS "libpostal not found, running install script from CMake...")
        execute_process(
            COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/scripts/install-deps-ci.sh
            RESULT_VARIABLE INSTALL_RESULT
            OUTPUT_VARIABLE INSTALL_OUTPUT
            ERROR_VARIABLE INSTALL_ERROR
        )

        if(INSTALL_RESULT EQUAL 0)
            message(STATUS "Install script completed, rechecking for libpostal...")
            # Check again after installation
            if(EXISTS "/usr/local/lib/libpostal.a" AND EXISTS "/usr/local/include/libpostal/libpostal.h")
                message(STATUS "Found libpostal in /usr/local after installation")
                set(LIBPOSTAL_LDFLAGS "/usr/local/lib/libpostal.a")
                set(LIBPOSTAL_INCLUDE_DIRS "/usr/local/include")
                set(LIBPOSTAL_FOUND TRUE)
            elseif(EXISTS "/usr/lib/libpostal.a" AND EXISTS "/usr/include/libpostal/libpostal.h")
                message(STATUS "Found libpostal in /usr/lib after installation")
                set(LIBPOSTAL_LDFLAGS "/usr/lib/libpostal.a")
                set(LIBPOSTAL_INCLUDE_DIRS "/usr/include")
                set(LIBPOSTAL_FOUND TRUE)
            else()
                message(FATAL_ERROR "Install script succeeded but libpostal still not found")
            endif()
        else()
            message(FATAL_ERROR "libpostal not found and install script failed: ${INSTALL_ERROR}")
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
    CURL::libcurl
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
    CURL::libcurl
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

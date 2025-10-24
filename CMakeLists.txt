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

# Libphonenumber and libpostal via pkg-config (system-installed, CI preferred)
find_package(PkgConfig REQUIRED)

# Add system paths for Alpine/Ubuntu packages (vcpkg may restrict search paths)
list(APPEND CMAKE_PREFIX_PATH "/usr" "/usr/local")

# Try pkg-config first (CI preferred method)
pkg_check_modules(LIBPHONENUMBER libphonenumber)
pkg_check_modules(LIBPOSTAL libpostal)

# Fallback: Try to find libphonenumber via library search
if(NOT LIBPHONENUMBER_FOUND)
    find_library(LIBPHONENUMBER_LIB
        NAMES phonenumber
        PATHS /usr/lib /usr/local/lib /usr/lib/x86_64-linux-gnu
    )
    find_path(LIBPHONENUMBER_INCLUDE_DIR
        NAMES phonenumbers/phonenumberutil.h
        PATHS /usr/include /usr/local/include
    )

    if(LIBPHONENUMBER_LIB AND LIBPHONENUMBER_INCLUDE_DIR)
        message(STATUS "Found libphonenumber via library search: ${LIBPHONENUMBER_LIB}")
        set(LIBPHONENUMBER_STATIC_LDFLAGS "${LIBPHONENUMBER_LIB}")
        set(LIBPHONENUMBER_INCLUDE_DIRS "${LIBPHONENUMBER_INCLUDE_DIR}")
        set(LIBPHONENUMBER_FOUND TRUE)
    else()
        message(STATUS "libphonenumber library search failed:")
        message(STATUS "  LIBPHONENUMBER_LIB: ${LIBPHONENUMBER_LIB}")
        message(STATUS "  LIBPHONENUMBER_INCLUDE_DIR: ${LIBPHONENUMBER_INCLUDE_DIR}")
    endif()
endif()

# Fallback: Try to find libpostal via library search
if(NOT LIBPOSTAL_FOUND)
    find_library(LIBPOSTAL_LIB
        NAMES postal libpostal
        PATHS /usr/lib /usr/local/lib /usr/lib/x86_64-linux-gnu
    )
    find_path(LIBPOSTAL_INCLUDE_DIR
        NAMES libpostal/libpostal.h
        PATHS /usr/include /usr/local/include
    )

    if(LIBPOSTAL_LIB AND LIBPOSTAL_INCLUDE_DIR)
        message(STATUS "Found libpostal via library search: ${LIBPOSTAL_LIB}")
        set(LIBPOSTAL_LDFLAGS "${LIBPOSTAL_LIB}")
        set(LIBPOSTAL_INCLUDE_DIRS "${LIBPOSTAL_INCLUDE_DIR}")
        set(LIBPOSTAL_FOUND TRUE)
    else()
        message(STATUS "libpostal library search failed:")
        message(STATUS "  LIBPOSTAL_LIB: ${LIBPOSTAL_LIB}")
        message(STATUS "  LIBPOSTAL_INCLUDE_DIR: ${LIBPOSTAL_INCLUDE_DIR}")
    endif()
endif()

if(NOT LIBPOSTAL_FOUND)
    message(FATAL_ERROR "libpostal not found. Install libpostal-dev (Debian/Ubuntu/Alpine) or libpostal-devel (RHEL/CentOS)")
endif()

if(NOT LIBPHONENUMBER_FOUND)
    message(FATAL_ERROR "libphonenumber not found. Install libphonenumber-dev or run scripts/install-deps-ci.sh")
endif()


# --- Build ---

set(EXTENSION_SOURCES
        src/anofox_tabular_extension.cpp
        src/anofox_diff.cpp
        src/anofox_postal.cpp
        src/anofox_phonenumber.cpp
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
    ${LIBPHONENUMBER_STATIC_LDFLAGS}
    c-ares::cares
    spdlog::spdlog_header_only
    ${LIBPOSTAL_LDFLAGS}
)

# Add include directories from dependencies
target_include_directories(${EXTENSION_NAME} PRIVATE
    ${LIBPHONENUMBER_INCLUDE_DIRS}
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
    ${LIBPHONENUMBER_STATIC_LDFLAGS}
    c-ares::cares
    spdlog::spdlog_header_only
    ${LIBPOSTAL_LDFLAGS}
)

# Add include directories from dependencies
target_include_directories(${LOADABLE_EXTENSION_NAME} PRIVATE
    ${LIBPHONENUMBER_INCLUDE_DIRS}
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

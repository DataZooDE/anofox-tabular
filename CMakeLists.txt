cmake_minimum_required(VERSION 3.10...3.29)

set(TARGET_NAME anofox_tabular)
set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})
include_directories(src/include)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Set CMake policy to suppress FindBoost warning
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()

# --- Dependencies ---

# Libs via vcpkg
include(CMakeFindDependencyMacro)
find_dependency(Boost REQUIRED COMPONENTS date_time system thread)
find_package(libphonenumber CONFIG REQUIRED)
find_package(absl CONFIG REQUIRED)
find_package(utf8_range CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(c-ares CONFIG REQUIRED)

# Libpostal via pkg-config
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBPOSTAL REQUIRED libpostal)


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
    libphonenumber::phonenumber
    c-ares::cares
    absl::log
    absl::log_internal_check_impl
    utf8_range::utf8_range
    spdlog::spdlog_header_only
    utf8_range::utf8_validity
    Boost::date_time
    Boost::system
    Boost::thread
    ${LIBPOSTAL_LDFLAGS}
)
target_compile_definitions(anofox_trace_obj PRIVATE SPDLOG_HEADER_ONLY=1 SPDLOG_FMT_EXTERNAL=0 SPDLOG_COMPILED_LIB=0)
target_compile_options(anofox_trace_obj PRIVATE -USPDLOG_FMT_EXTERNAL -USPDLOG_COMPILED_LIB)
target_compile_definitions(${EXTENSION_NAME} PRIVATE SPDLOG_HEADER_ONLY=1 SPDLOG_FMT_EXTERNAL=0 SPDLOG_COMPILED_LIB=0)
target_compile_options(${EXTENSION_NAME} PRIVATE -USPDLOG_FMT_EXTERNAL -USPDLOG_COMPILED_LIB)
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
    libphonenumber::phonenumber
    c-ares::cares
    absl::log
    absl::log_internal_check_impl
    utf8_range::utf8_range
    spdlog::spdlog_header_only
    utf8_range::utf8_validity
    Boost::date_time
    Boost::system
    Boost::thread
    ${LIBPOSTAL_LDFLAGS}
)
target_compile_definitions(${LOADABLE_EXTENSION_NAME} PRIVATE SPDLOG_HEADER_ONLY=1 SPDLOG_FMT_EXTERNAL=0 SPDLOG_COMPILED_LIB=0)
target_compile_options(${LOADABLE_EXTENSION_NAME} PRIVATE -USPDLOG_FMT_EXTERNAL -USPDLOG_COMPILED_LIB)

# Add include directories from dependencies
target_include_directories(${EXTENSION_NAME} PRIVATE
    ${LIBPOSTAL_INCLUDE_DIRS}
)
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

cmake_minimum_required(VERSION 3.5)

set(TARGET_NAME anofox_tabular)
set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})
include_directories(src/include)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- Dependencies ---

# Libs via vcpkg
include(CMakeFindDependencyMacro)
find_dependency(Boost REQUIRED COMPONENTS date_time system thread)
find_package(libphonenumber CONFIG REQUIRED)
find_package(absl CONFIG REQUIRED)
find_package(utf8_range CONFIG REQUIRED)

# Libpostal via pkg-config
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBPOSTAL REQUIRED libpostal)


# --- Build ---

set(EXTENSION_SOURCES
        src/anofox_tabular_extension.cpp
        src/postal/postal_functions.cpp
        src/postal/postal_manager.cpp
)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})


# --- Linking ---

# Link dependencies
target_link_libraries(${EXTENSION_NAME} 
    libphonenumber::phonenumber
    absl::log
    absl::log_internal_check_impl
    utf8_range::utf8_range
    utf8_range::utf8_validity
    Boost::date_time
    Boost::system
    Boost::thread
    ${LIBPOSTAL_LDFLAGS}
)
target_link_libraries(${LOADABLE_EXTENSION_NAME}
    libphonenumber::phonenumber
    absl::log
    absl::log_internal_check_impl
    utf8_range::utf8_range
    utf8_range::utf8_validity
    Boost::date_time
    Boost::system
    Boost::thread
    ${LIBPOSTAL_LDFLAGS}
)

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

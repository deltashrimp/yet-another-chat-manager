cmake_minimum_required(VERSION 3.20)

# asio release tarballs ship no CMake build system: we wrap headers in an
# INTERFACE target ourselves, which needs the (deprecated-in-4.x) populate.
set(CMAKE_POLICY_DEFAULT_CMP0135 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0169 OLD)

project(yet-another-chat-manager VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(YACM_BUILD_TESTS "Build unit tests" ON)

# --- header-only dependencies -------------------------------------------
include(FetchContent)

FetchContent_Declare(asio
  URL https://github.com/chriskohlhoff/asio/archive/refs/tags/asio-1-30-2.tar.gz
)
# asio's release tarball has no CMake build system — wrap the headers in an
# INTERFACE target (standalone mode, no Boost).
FetchContent_GetProperties(asio)
if(NOT asio_POPULATED)
  FetchContent_Populate(asio)
endif()
add_library(asio INTERFACE)
add_library(asio::asio ALIAS asio)
target_include_directories(asio INTERFACE ${asio_SOURCE_DIR}/asio/include)
target_compile_definitions(asio INTERFACE ASIO_STANDALONE)

FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/archive/refs/tags/v3.11.3.tar.gz
)
FetchContent_MakeAvailable(nlohmann_json)

# --- system dependencies -------------------------------------------------
find_package(spdlog 1.10 REQUIRED CONFIG)
find_package(fmt REQUIRED CONFIG)
find_package(CURL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)

# Some distros (Debian bookworm) ship sqlite3's CMake config without the
# SQLite3::SQLite3 imported target — rebuild it from the module variables
# so the same CMakeLists works everywhere.
if(NOT TARGET SQLite3::SQLite3)
  add_library(SQLite3::SQLite3 UNKNOWN IMPORTED)
  set_target_properties(SQLite3::SQLite3 PROPERTIES
    IMPORTED_LOCATION "${SQLite3_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${SQLite3_INCLUDE_DIRS}")
endif()

# --- build metadata ------------------------------------------------------
set(YACM_VERSION "${PROJECT_VERSION}")
string(TIMESTAMP YACM_BUILD_DATE "%Y-%m-%d %H:%M UTC" UTC)
configure_file(src/app/version.hpp.in "${CMAKE_BINARY_DIR}/generated/version.hpp" @ONLY)

add_subdirectory(src)

if(YACM_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
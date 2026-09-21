set(YACM_SOURCES
  app/app_context.cpp
  config/config.cpp
  handlers/common.cpp
  handlers/general.cpp
  handlers/moderation.cpp
  handlers/settings.cpp
  i18n/strings.cpp
  router/router.cpp
  services/moderation.cpp
  services/roles.cpp
  services/warns.cpp
  storage/db.cpp
  storage/repos.cpp
  telegram/client.cpp
  telegram/sender.cpp
  telegram/types.cpp
  util/format.cpp
  util/parsers.cpp
)

add_library(yacm_core STATIC ${YACM_SOURCES})

target_include_directories(yacm_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated
)

target_link_libraries(yacm_core PUBLIC
  spdlog::spdlog
  fmt::fmt
  CURL::libcurl
  SQLite3::SQLite3
  Threads::Threads
  asio::asio
  nlohmann_json::nlohmann_json
)

target_compile_definitions(yacm_core PUBLIC SPDLOG_FMT_EXTERNAL)

add_executable(bot app/main.cpp)
target_link_libraries(bot PRIVATE yacm_core)
# Drop the binary into build/ (not build/src/).
set_target_properties(bot PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
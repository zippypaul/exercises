# slowmail_core: deterministic business logic. NO Qt, NO network, NO SQLite.
file(GLOB_RECURSE SLOWMAIL_CORE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
add_library(slowmail_core STATIC ${SLOWMAIL_CORE_SOURCES})
target_include_directories(slowmail_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(slowmail_core PUBLIC nlohmann_json::nlohmann_json Threads::Threads)
slowmail_apply_warnings(slowmail_core)

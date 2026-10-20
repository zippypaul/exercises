# slowmail_graph: Microsoft Graph adapter (libcurl HTTP, device-code OAuth, delta sync, move/flag).
file(GLOB_RECURSE SLOWMAIL_GRAPH_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
add_library(slowmail_graph STATIC ${SLOWMAIL_GRAPH_SOURCES})
target_include_directories(slowmail_graph PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(slowmail_graph PUBLIC slowmail_core CURL::libcurl)
slowmail_apply_warnings(slowmail_graph)

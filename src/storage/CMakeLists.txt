# slowmail_storage: SQLite implementation of slowmail::core::Store.
file(GLOB_RECURSE SLOWMAIL_STORAGE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
add_library(slowmail_storage STATIC ${SLOWMAIL_STORAGE_SOURCES})
target_include_directories(slowmail_storage PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(slowmail_storage PUBLIC slowmail_core SQLite::SQLite3)
slowmail_apply_warnings(slowmail_storage)
